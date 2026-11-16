/* SPDX-License-Identifier: GPL-3.0-or-later */
/* jedgeom.c -- fuse-map geometry, and the per-macrocell `config` report.
 *
 * Derives the geometry from the JED's own "Note Block N <section>" headers
 * rather than a device table, so it works on any density, and it reads the
 * fuse map directly -- which is what lets `config` report a macrocell whether
 * or not anything drives it. That case is exactly what the netlist cannot
 * express.
 *
 * THERE ARE THREE MACROCELL LAYOUTS: 27 bits on XC2C32/32A/64/64A, 29 bits
 * on XC2C128 and up, and a reduced 16-bit record for a buried macrocell --
 * each in a different field order. Reading one with another's order
 * mis-decodes the macrocell, and does it silently.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "jedgeom.h"

/* PROVENANCE: VENDOR largest part: 32 function blocks of 16 macrocells and 56 product terms: ISE device files and fitter reports */
#define MAXBLK 32
/* PROVENANCE: DEFINED row capacity */
#define MAXROW 64

typedef struct {
    char **rows;
    int n, w;
} Rows;

struct JedGeom {
    int nblk;
    Rows zia[MAXBLK], andr[MAXBLK], orr[MAXBLK], mc[MAXBLK];
    int qf;
    char dev[128];
    unsigned char *fuse;   /* flat fuse array, qf entries; NULL if QF absent */
};

static void rows_push(Rows *r, const char *bits, int len) {
    r->rows = realloc(r->rows, sizeof(char *) * (size_t)(r->n + 1));
    r->rows[r->n] = malloc((size_t)len + 1);
    memcpy(r->rows[r->n], bits, (size_t)len);
    r->rows[r->n][len] = 0;
    if (!r->w) r->w = len;
    r->n++;
}

/* Parse "Note Block N <section>" headers and the L records that follow each. */
JedGeom *jedgeom_read(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 2);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f); buf[got] = 0; fclose(f);

    JedGeom *g = calloc(1, sizeof(JedGeom));
    char *q = strstr(buf, "QF");
    g->qf = q ? atoi(q + 2) : 0;

    /* Flat fuse array. The global-net bits (GCK / GSR / GTS / termination) sit
     * in the tail of the map, outside every Note Block section, so the block
     * rows below cannot reach them. The JEDEC default state is the F field;
     * the programmed fuses follow in L records. */
    if (g->qf > 0 && (g->fuse = malloc((size_t)g->qf)) != NULL) {
        const char *fp = strstr(buf, "*F");
        unsigned char def = (fp && (fp[2] == '0' || fp[2] == '1'))
                            ? (unsigned char)(fp[2] - '0') : 0;
        memset(g->fuse, def, (size_t)g->qf);
    }
    char *d = strstr(buf, "N DEVICE");
    if (d) {
        /* PROVENANCE: DEFINED length of 'N DEVICE' */
        d += 8; while (*d == ' ') d++;
        size_t i = 0;
        while (d[i] && d[i] != '*' && d[i] != '\r' && d[i] != '\n'
               && i < sizeof(g->dev) - 1) { g->dev[i] = d[i]; i++; }
        g->dev[i] = 0;
    }

    int curblk = -1;
    Rows *cur = NULL;
    char *line = strtok(buf, "\r\n");
    while (line) {
        while (*line == ' ') line++;
        if (strncmp(line, "Note Block ", 11) == 0) {
            /* PROVENANCE: DEFINED length of 'Note Block ' */
            int b = atoi(line + 11);
            /* line+11 points at the block number; the section name starts
             * after the next space. */
            /* PROVENANCE: DEFINED length of 'Note Block ' */
            const char *rest = strchr(line + 11, ' ');
            if (rest) { rest++; while (*rest == ' ') rest++; }
            cur = NULL;
            if (b >= 0 && b < MAXBLK && rest) {
                if (b + 1 > curblk + 1) curblk = b;
                if (g->nblk < b + 1) g->nblk = b + 1;
                if (!strncmp(rest, "ZIA", 3))                      cur = &g->zia[b];
                else if (!strncmp(rest, "PLA AND array", 13))      cur = &g->andr[b];
                else if (!strncmp(rest, "PLA OR array", 12))       cur = &g->orr[b];
                else if (!strncmp(rest, "I/O Macrocell Config", 20)) cur = &g->mc[b];
            }
        } else if (strncmp(line, "Note", 4) == 0) {
            cur = NULL;
        } else if (line[0] == 'L') {
            /* PROVENANCE: DEFINED decimal */
            char *e; long addr = strtol(line + 1, &e, 10);
            while (*e == ' ') e++;
            char *s = e; int n = 0;
            while (s[n] == '0' || s[n] == '1') n++;
            if (n > 0 && cur) rows_push(cur, s, n);
            /* Every L record also lands in the flat array, inside a block or
             * not: that is the only way to reach the global-net tail. */
            if (n > 0 && g->fuse && addr >= 0)
                for (int i = 0; i < n && addr + i < g->qf; i++)
                    g->fuse[addr + i] = (unsigned char)(s[i] - '0');
        }
        line = strtok(NULL, "\r\n");
    }
    free(buf);
    if (g->nblk == 0) { free(g); return NULL; }
    return g;
}

/* The global-net fuse block (GCK / GSR / GTS / termination).
 *
 *   gsr + 0        gsr_invert
 *   gsr + 1        gsr_enable
 *   gts + 2n       gts_invert[n]
 *   gts + 2n + 1   NOT gts_enable[n]        stored inverted
 *   term           global termination is a pull-up
 *
 * An unprogrammed part reads all ones, which is "disabled, acts as !T": that
 * is why the enable bit is stored the other way up. gts = gsr + 2 and
 * term = gts + 8 on every density; only the base moves.
 */
typedef struct { const char *dev; int gsr; } GlobBase;
/* PROVENANCE: THIRD-PARTY global-net fuse addresses (gsr, gts = gsr+2, term = gts+8): GTS2 and GTS3 from xc2bit (openfpga); not independently measured here; GSR, GTS0, GTS1 and termination measured from designs fitted for the purpose */
static const GlobBase GLOB_BASE[] = {
    {"XC2C32",   12259}, {"XC2C32A",  12259},
    {"XC2C64",   25795}, {"XC2C64A",  25795},
    {"XC2C128",  55324}, {"XC2C256", 123232},
    {"XC2C384", 209336}, {"XC2C512", 296382},
};

int jedgeom_globals(const JedGeom *g, JedGlobals *out) {
    if (!g || !g->fuse || !out) return 0;
    /* The device field carries speed and package too -- "XC2C128-7-VQ100" --
     * so match the family token at its head, not the whole string, and keep
     * the A apart from the part it is a variant of. */
    char fam[16]; size_t k = 0;
    for (const char *p = g->dev; *p && k < sizeof fam - 1; p++) {
        if (k < 4) { if (*p != "XC2C"[k]) return 0; fam[k++] = *p; continue; }
        if (*p >= '0' && *p <= '9') { fam[k++] = *p; continue; }
        /* PROVENANCE: DEFINED length of 'XC2C' */
        if (*p == 'A' && k > 4) fam[k++] = *p;
        break;
    }
    fam[k] = 0;
    int base = -1;
    for (size_t i = 0; i < sizeof GLOB_BASE / sizeof *GLOB_BASE; i++)
        if (!strcmp(fam, GLOB_BASE[i].dev)) { base = GLOB_BASE[i].gsr; break; }
    if (base < 0) return 0;
    /* PROVENANCE: THIRD-PARTY global-net fuse addresses (gsr, gts = gsr+2, term = gts+8): GTS2 and GTS3 from xc2bit (openfpga); not independently measured here; GSR, GTS0, GTS1 and termination measured from designs fitted for the purpose */
    int gts = base + 2, term = gts + 8;
    if (term >= g->qf) return 0;
    out->gsr_invert = g->fuse[base] != 0;
    out->gsr_enable = g->fuse[base + 1] != 0;
    out->global_pu  = g->fuse[term] != 0;
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files */
    for (int n = 0; n < 4; n++) {
        out->gts_invert[n] = g->fuse[gts + 2 * n] != 0;
        out->gts_enable[n] = g->fuse[gts + 2 * n + 1] == 0;
    }
    return 1;
}

void jedgeom_free(JedGeom *g) {
    if (!g) return;
    free(g->fuse);
    for (int b = 0; b < MAXBLK; b++) {
        /* PROVENANCE: DEFINED the four sections */
        Rows *all[4] = { &g->zia[b], &g->andr[b], &g->orr[b], &g->mc[b] };
        /* PROVENANCE: DEFINED the four sections */
        for (int k = 0; k < 4; k++) {
            for (int i = 0; i < all[k]->n; i++) free(all[k]->rows[i]);
            free(all[k]->rows);
        }
    }
    free(g);
}

int jedgeom_nblk(const JedGeom *g) { return g->nblk; }
int jedgeom_qf(const JedGeom *g) { return g->qf; }
const char *jedgeom_dev(const JedGeom *g) { return g->dev; }
int jedgeom_nmc(const JedGeom *g, int b) { return g->mc[b].n; }
int jedgeom_mcw(const JedGeom *g, int b) { return g->mc[b].w; }
const char *jedgeom_mc(const JedGeom *g, int b, int i) { return g->mc[b].rows[i]; }
int jedgeom_zia_n(const JedGeom *g, int b) { return g->zia[b].n; }
int jedgeom_zia_w(const JedGeom *g, int b) { return g->zia[b].w; }
int jedgeom_and_n(const JedGeom *g, int b) { return g->andr[b].n; }
int jedgeom_and_w(const JedGeom *g, int b) { return g->andr[b].w; }
int jedgeom_or_n(const JedGeom *g, int b) { return g->orr[b].n; }
int jedgeom_or_w(const JedGeom *g, int b) { return g->orr[b].w; }

/* ---- field layouts. The 27-bit order is the JED's own "N Aclk ClkOp ..."
 * note. The fields of the other two were measured from designs fitted for
 * the purpose. ---- */
/* Position 3 of the 29- and 16-bit records, and offset 4 of the 27-bit one,
 * is the DUAL-EDGE flag. */
typedef struct { const char *name; int w; } Field;

/* PROVENANCE: VENDOR 27-bit field order and widths: the N-line ISE writes into every JED */
static const Field F27[] = {
    {"Aclk",1},{"ClkOp",1},{"Clk",2},{"DDR",1},{"R",2},{"P",2},{"RegMod",2},
    {"INz",2},{"FB",2},{"InReg",1},{"St",1},{"XorIn",2},{"RegCom",1},{"Oe",4},
    {"Tm",1},{"Slw",1},{"Pu",1},{NULL,0}
};
/* A BURIED macrocell -- one with no pad, which large parts have because a
 * function block holds more macrocells than pins -- gets a REDUCED 16-bit
 * record. Slicing one with the 29-bit layout reads garbage. */
/* PROVENANCE: MEASURED 16-bit field order: measured from designs fitted for the purpose */
static const Field F16[] = {
    {"Clk",3},{"DDR",1},{"ClkOp",1},{"McFB",2},{"P",2},{"St",1},
    {"RegMod",2},{"R",2},{"XorIn",2},{NULL,0}
};
/* PROVENANCE: MEASURED 29-bit field order: measured from designs fitted for the purpose */
static const Field F29[] = {
    {"Clk",3},{"DDR",1},{"ClkOp",1},{"DataGate",1},{"McFB",2},{"INz",2},
    {"UseIOB",1},{"FB",2},{"Oe",4},{"P",2},{"St",1},{"RegCom",1},{"RegMod",2},
    {"R",2},{"Slw",1},{"Tm",1},{"XorIn",2},{NULL,0}
};

int jedgeom_is_ddr(const char *cfg) {
    /* PROVENANCE: MEASURED dual-edge bit: 27-bit position 4, 29/16-bit position 3: measured from designs fitted for the purpose */
    return (int)strlen(cfg) == 27 ? cfg[4] == '1' : cfg[3] == '1';
}

/* The register's power-up state. Three layouts, three positions, stored
 * INVERTED in all three: 27-bit bit 26, which the JED calls `Pu`; 29-bit bit
 * 19 and 16-bit bit 9, which the other two layouts call `St`. */
int jedgeom_init_state(const char *cfg) {
    int n = (int)strlen(cfg);
    /* PROVENANCE: MEASURED power-up bit, stored inverted: 27-bit 26, 29-bit 19, 16-bit 9: measured from designs fitted for the purpose */
    int bit = (n == 27) ? 26 : ((n == 29) ? 19 : 9);
    if (bit >= n) return -1;
    return cfg[bit] == '0' ? 1 : 0;
}

/* FIELD-VALUE ANNOTATIONS -- for the human-readable `config` output only;
 * the decoder itself never consults them. Each mapping's PROVENANCE tag says
 * where it comes from. */
static const char *xorin_note(const char *v) {
    /* PROVENANCE: MEASURED XorIn 00 const 0, 11 const 1, 10 the PTC-row term, 01 its complement: measured from designs fitted for the purpose */
    if (!strcmp(v, "00")) return "ZERO (pass OR)";
    /* PROVENANCE: MEASURED XorIn 00 const 0, 11 const 1, 10 the PTC-row term, 01 its complement: measured from designs fitted for the purpose */
    if (!strcmp(v, "11")) return "ONE (invert OR)";
    /* PROVENANCE: MEASURED XorIn 00 const 0, 11 const 1, 10 the PTC-row term, 01 its complement: measured from designs fitted for the purpose */
    if (!strcmp(v, "10")) return "PTC  (OR xor PTC)";
    /* PROVENANCE: MEASURED XorIn 00 const 0, 11 const 1, 10 the PTC-row term, 01 its complement: measured from designs fitted for the purpose */
    if (!strcmp(v, "01")) return "PTCB (OR xnor PTC)";
    return "";
}
static const char *oe_note(const char *v) {
    /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111, 1010 0110, 0100 PTB and 1000 CTE with their rows: measured from designs fitted for the purpose */
    if (!strcmp(v, "1111")) return "disabled";
    /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111, 1010 0110, 0100 PTB and 1000 CTE with their rows: measured from designs fitted for the purpose */
    if (!strcmp(v, "0000")) return "push-pull";
    /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111, 1010 0110, 0100 PTB and 1000 CTE with their rows: measured from designs fitted for the purpose */
    if (!strcmp(v, "0001")) return "open-drain";
    /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111, 1010 0110, 0100 PTB and 1000 CTE with their rows: measured from designs fitted for the purpose */
    if (!strcmp(v, "1100")) return "GTS0";
    /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111, 1010 0110, 0100 PTB and 1000 CTE with their rows: measured from designs fitted for the purpose */
    if (!strcmp(v, "0010")) return "GTS1";
    /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111, 1010 0110, 0100 PTB and 1000 CTE with their rows: measured from designs fitted for the purpose */
    if (!strcmp(v, "1010")) return "GTS2";
    /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111, 1010 0110, 0100 PTB and 1000 CTE with their rows: measured from designs fitted for the purpose */
    if (!strcmp(v, "0110")) return "GTS3";
    /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111, 1010 0110, 0100 PTB and 1000 CTE with their rows: measured from designs fitted for the purpose */
    if (!strcmp(v, "0100")) return "PTB";
    /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111, 1010 0110, 0100 PTB and 1000 CTE with their rows: measured from designs fitted for the purpose */
    if (!strcmp(v, "1000")) return "CTE";
    /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111, 1010 0110, 0100 PTB and 1000 CTE with their rows: measured from designs fitted for the purpose */
    if (!strcmp(v, "1110")) return "CGND";
    return "INVALID";
}
static const char *ff_note(const char *v) {
    /* PROVENANCE: MEASURED register type 00 D, 01 latch, 10 T, 11 D + clock enable: measured from designs fitted for the purpose */
    if (!strcmp(v, "00")) return "D flip-flop";
    /* PROVENANCE: MEASURED register type 00 D, 01 latch, 10 T, 11 D + clock enable: measured from designs fitted for the purpose */
    if (!strcmp(v, "01")) return "transparent latch";
    /* PROVENANCE: MEASURED register type 00 D, 01 latch, 10 T, 11 D + clock enable: measured from designs fitted for the purpose */
    if (!strcmp(v, "10")) return "T flip-flop";
    /* PROVENANCE: MEASURED register type 00 D, 01 latch, 10 T, 11 D + clock enable: measured from designs fitted for the purpose */
    if (!strcmp(v, "11")) return "D flip-flop with clock-enable";
    return "";
}
static const char *ibuf_note(const char *v) {
    /* PROVENANCE: MEASURED input-buffer codes 00/11/10 and 01 VREF pin: measured from designs fitted for the purpose */
    if (!strcmp(v, "00")) return "no VREF, no Schmitt trigger";
    /* PROVENANCE: MEASURED input-buffer codes 00/11/10 and 01 VREF pin: measured from designs fitted for the purpose */
    if (!strcmp(v, "11")) return "no VREF, Schmitt trigger (default)";
    /* PROVENANCE: MEASURED input-buffer codes 00/11/10 and 01 VREF pin: measured from designs fitted for the purpose */
    if (!strcmp(v, "10")) return "uses VREF (HSTL/SSTL)";
    /* PROVENANCE: MEASURED input-buffer codes 00/11/10 and 01 VREF pin: measured from designs fitted for the purpose */
    if (!strcmp(v, "01")) return "is a VREF pin";
    return "";
}
/* The 27-bit record's `FB` field is the MACROCELL feedback source, the same
 * thing as `McFB` on the 29- and 16-bit records: annotated with mcfb_note. */
/* Whether the ZIA reads the PAD directly: 27-bit `INz` (bits 11-12) and the
 * 29-bit field at bits 11-12 (named `FB` in the table above): '00' when the
 * ZIA reads the pad directly, '11' when the pad is unused or read only through
 * the input register. The ZIA decode names the pad source on its
 * own, so the decoder does not need this field; it is annotated only. */
static const char *padzia_note(const char *v) {
    /* PROVENANCE: MEASURED 29-bit pad-to-ZIA codes 00 yes / 11 no: measured from designs fitted for the purpose */
    if (!strcmp(v, "00")) return "pad read by the ZIA directly";
    /* PROVENANCE: MEASURED 29-bit pad-to-ZIA codes 00 yes / 11 no: measured from designs fitted for the purpose */
    if (!strcmp(v, "11")) return "pad not read directly";
    return "";
}
/* The feedback source decides whether the ZIA reads the XOR gate or the
 * register. */
static const char *mcfb_note(const char *v) {
    /* PROVENANCE: THIRD-PARTY feedback codes: 01 from xc2bit (openfpga); not independently measured here; 00, 10 and 11 measured from designs fitted for the purpose */
    if (!strcmp(v, "01") || !strcmp(v, "11")) return "disabled (default)";
    /* PROVENANCE: THIRD-PARTY feedback codes: 01 from xc2bit (openfpga); not independently measured here; 00, 10 and 11 measured from designs fitted for the purpose */
    if (!strcmp(v, "00")) return "from the XOR gate";
    /* PROVENANCE: THIRD-PARTY feedback codes: 01 from xc2bit (openfpga); not independently measured here; 00, 10 and 11 measured from designs fitted for the purpose */
    if (!strcmp(v, "10")) return "from the flip-flop";
    return "";
}
/* Clock source, as three bits (on the 27-bit record, Aclk then Clk):
 * '000'->GCK0, '010'->GCK1, '001'->GCK2, '011' the macrocell's own PTC,
 * '111' the block's CTC. The 1xx readings of the global clocks are
 * xc2bit's. */
static const char *clksrc_note(const char *v) {
    /* PROVENANCE: THIRD-PARTY clock-source codes 1xx (100, 110, 101; on the 27-bit record Aclk 1 with Clk 00, 10, 01): xc2bit (openfpga); not independently measured here */
    if (!strcmp(v, "000") || !strcmp(v, "100")) return "GCK0";
    /* PROVENANCE: THIRD-PARTY clock-source codes 1xx (100, 110, 101; on the 27-bit record Aclk 1 with Clk 00, 10, 01): xc2bit (openfpga); not independently measured here */
    if (!strcmp(v, "010") || !strcmp(v, "110")) return "GCK1";
    /* PROVENANCE: THIRD-PARTY clock-source codes 1xx (100, 110, 101; on the 27-bit record Aclk 1 with Clk 00, 10, 01): xc2bit (openfpga); not independently measured here */
    if (!strcmp(v, "001") || !strcmp(v, "101")) return "GCK2";
    /* PROVENANCE: MEASURED clock 011 PTC / 111 CTC (29/16-bit), Aclk 0 PTC / 1 CTC with Clk 11 (27-bit): measured from designs fitted for the purpose */
    if (!strcmp(v, "011")) return "PTC";
    /* PROVENANCE: MEASURED clock 011 PTC / 111 CTC (29/16-bit), Aclk 0 PTC / 1 CTC with Clk 11 (27-bit): measured from designs fitted for the purpose */
    if (!strcmp(v, "111")) return "CTC";
    return "";
}
static const char *src_note(const char *v, int is_reset) {
    /* PROVENANCE: MEASURED R/P 11 none, 01 GSR, 10 CTR/CTS (rows 5/6), 00 PTA (row 8+3k): measured from designs fitted for the purpose */
    if (!strcmp(v, "11")) return "disabled";
    /* PROVENANCE: MEASURED R/P 11 none, 01 GSR, 10 CTR/CTS (rows 5/6), 00 PTA (row 8+3k): measured from designs fitted for the purpose */
    if (!strcmp(v, "00")) return "PTA";
    /* PROVENANCE: MEASURED R/P 11 none, 01 GSR, 10 CTR/CTS (rows 5/6), 00 PTA (row 8+3k): measured from designs fitted for the purpose */
    if (!strcmp(v, "01")) return "GSR";
    /* PROVENANCE: MEASURED R/P 11 none, 01 GSR, 10 CTR/CTS (rows 5/6), 00 PTA (row 8+3k): measured from designs fitted for the purpose */
    if (!strcmp(v, "10")) return is_reset ? "CTR" : "CTS";
    return "";
}

int jedgeom_print_config(const JedGeom *g, int fb, int mc) {
    int b = fb - 1;
    if (b < 0 || b >= g->nblk || mc < 1 || mc > g->mc[b].n) {
        fprintf(stderr, "error: FB%d MC%d out of range (%d blocks, %d macrocells each)\n",
                fb, mc, g->nblk, g->nblk ? g->mc[0].n : 0);
        return 2;
    }
    const char *cfg = g->mc[b].rows[mc - 1];
    int len = (int)strlen(cfg);
    /* PROVENANCE: VENDOR record widths 27 / 29 / 16 bits: the N-lines ISE writes into every JED */
    const Field *L = (len == 16) ? F16 : ((len == 29) ? F29 : F27);
    printf("FB%d_MC%d configuration   (%d-bit %s-device record)\n",
           /* PROVENANCE: VENDOR record widths 27 / 29 / 16 bits: the N-lines ISE writes into every JED */
           fb, mc, len, len == 29 ? "large" : "small");
    printf("  raw            : %s\n", cfg);
    int i = 0;
    for (const Field *f = L; f->name; f++) {
        char v[8];
        memcpy(v, cfg + i, (size_t)f->w); v[f->w] = 0;
        i += f->w;
        const char *note = "";
        if (!strcmp(f->name, "XorIn"))       note = xorin_note(v);
        else if (!strcmp(f->name, "Oe"))     note = oe_note(v);
        else if (!strcmp(f->name, "RegMod")) note = ff_note(v);
        else if (!strcmp(f->name, "R"))      note = src_note(v, 1);
        else if (!strcmp(f->name, "P"))      note = src_note(v, 0);
        /* `INz` is the pad-direct field on the 27-bit record but the input
         * buffer mode on the 29-bit one (bits 8-9); `FB` is the macrocell
         * feedback on the 27-bit record but the pad-direct field on the
         * 29-bit one (bits 11-12). Same name, different bits: annotate by
         * record length. */
        /* PROVENANCE: VENDOR record widths 27 / 29 / 16 bits: the N-lines ISE writes into every JED */
        else if (!strcmp(f->name, "INz"))    note = (len == 27) ? padzia_note(v) : ibuf_note(v);
        /* PROVENANCE: VENDOR record widths 27 / 29 / 16 bits: the N-lines ISE writes into every JED */
        else if (!strcmp(f->name, "FB"))     note = (len == 29) ? padzia_note(v) : mcfb_note(v);
        else if (!strcmp(f->name, "McFB"))   note = mcfb_note(v);
        else if (!strcmp(f->name, "Slw"))    note = !strcmp(v, "0") ? "fast (default)" : "slow";
        else if (!strcmp(f->name, "RegCom")) note = !strcmp(v, "0") ? "flip-flop" : "XOR gate (default)";
        else if (!strcmp(f->name, "Tm"))     note = !strcmp(v, "0") ? "no" : "yes";
        /* On the 27-bit record the register's initial state is a field of its
         * own, `Pu`, and `St` is the SCHMITT TRIGGER. The 29- and 16-bit
         * records have no `Pu`: there `St` IS the initial state, stored
         * inverted, which is what jedgeom_init_state() reads -- same name,
         * different meaning. */
        else if (!strcmp(f->name, "St"))
            /* PROVENANCE: MEASURED Schmitt trigger: 27-bit St, 29-bit INz 11; on the 29/16-bit record St is the power-up bit: measured from designs fitted for the purpose */
            note = (len == 27) ? (!strcmp(v, "1") ? "Schmitt trigger on" : "Schmitt trigger off")
                 : (!strcmp(v, "0") ? "initial state 1" : "initial state 0");
        else if (!strcmp(f->name, "ClkOp"))  note = !strcmp(v, "1") ? "clock inverted" : "clock not inverted";
        else if (!strcmp(f->name, "Aclk") || !strcmp(f->name, "Clk"))
                                             note = "(part of the clock source, shown below)";
        else if (!strcmp(f->name, "DDR"))    note = "dual-edge (shown below)";
        else if (!strcmp(f->name, "DataGate")) note = !strcmp(v, "1") ? "DataGate enabled" : "DataGate off";
        else if (!strcmp(f->name, "UseIOB") || !strcmp(f->name, "InReg"))
                                             note = !strcmp(v, "0") ? "input register in use" : "no input register";
        else if (!strcmp(f->name, "Pu"))     note = !strcmp(v, "0") ? "initial state 1" : "initial state 0";
        printf("  %-14s : %-4s %s\n", f->name, v, note);
    }
    /* clock source is 3 bits: on the 29-bit record `Clk` IS 3 bits; on the
     * 27-bit record it is Aclk (bit 0) then Clk (bits 2-3). */
    char cs[4];
    /* PROVENANCE: MEASURED 27-bit Aclk|Clk 0|00 GCK0, 0|10 GCK1, 0|01 GCK2, 0|11 PTC, 1|11 CTC: measured from designs fitted for the purpose */
    if (len == 29) { cs[0]=cfg[0]; cs[1]=cfg[1]; cs[2]=cfg[2]; }
    /* PROVENANCE: MEASURED 27-bit Aclk|Clk 0|00 GCK0, 0|10 GCK1, 0|01 GCK2, 0|11 PTC, 1|11 CTC: measured from designs fitted for the purpose */
    else           { cs[0]=cfg[0]; cs[1]=cfg[2]; cs[2]=cfg[3]; }
    /* PROVENANCE: DEFINED terminator */
    cs[3] = 0;
    printf("  clock source   : %-4s %s\n", cs, clksrc_note(cs));
    printf("  initial state  : %d\n", jedgeom_init_state(cfg));
    printf("  DualEDGE       : %s\n", jedgeom_is_ddr(cfg) ? "yes" : "no");
    return 0;
}

/* ---- accessors the netlist builder needs ------------------------
 * `modelgen.c` assembles the Model straight from these rows. */

const char *jedgeom_zia_row(const JedGeom *g, int b, int r) {
    if (b < 0 || b >= g->nblk || r < 0 || r >= g->zia[b].n) return NULL;
    return g->zia[b].rows[r];
}
const char *jedgeom_and_row(const JedGeom *g, int b, int p) {
    if (b < 0 || b >= g->nblk || p < 0 || p >= g->andr[b].n) return NULL;
    return g->andr[b].rows[p];
}
const char *jedgeom_or_row(const JedGeom *g, int b, int p) {
    if (b < 0 || b >= g->nblk || p < 0 || p >= g->orr[b].n) return NULL;
    return g->orr[b].rows[p];
}
int jedgeom_mc_n(const JedGeom *g, int b) {
    return (b < 0 || b >= g->nblk) ? 0 : g->mc[b].n;
}
int jedgeom_mc_w(const JedGeom *g, int b, int i) {
    if (b < 0 || b >= g->nblk || i < 0 || i >= g->mc[b].n) return 0;
    return (int)strlen(g->mc[b].rows[i]);
}

/* Slice a NAMED field out of a macrocell record. Which layout applies is
 * decided by the record's own LENGTH -- 27, 29 or the reduced 16 of a buried
 * macrocell -- never by the device, because a single function block
 * holds both kinds on the large dies. */
int jedgeom_field(const JedGeom *g, int fb, int mc, const char *name,
                  char *out, size_t n) {
    if (fb < 1 || fb > g->nblk) return 0;
    int b = fb - 1;
    if (mc < 1 || mc > g->mc[b].n) return 0;
    const char *cfg = g->mc[b].rows[mc - 1];
    int len = (int)strlen(cfg);
    /* PROVENANCE: VENDOR record widths 27 / 29 / 16 bits: the N-lines ISE writes into every JED */
    const Field *tab = len == 27 ? F27 : len == 16 ? F16 : F29;
    int off = 0;
    for (const Field *f = tab; f->name; f++) {
        if (!strcmp(f->name, name)) {
            if (off + f->w > len || (size_t)f->w >= n) return 0;
            memcpy(out, cfg + off, (size_t)f->w);
            out[f->w] = 0;
            return 1;
        }
        off += f->w;
    }
    return 0;
}

/* XC2C64A -> 64. Skip the "XC2C" prefix FIRST: scanning to the first digit
 * finds the '2' in "XC2C" and returns 2 for every part. */
int jedgeom_density(const JedGeom *g) {
    const char *p = g->dev;
    if (!strncmp(p, "XC2C", 4) || !strncmp(p, "xc2c", 4)) p += 4;
    else while (*p && (*p < '0' || *p > '9')) p++;
    return atoi(p);
}
