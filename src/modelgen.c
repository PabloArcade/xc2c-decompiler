/* modelgen.c -- build the Model straight from the fuse map.
 *
 * Takes the rows jedgeom.c read out of the .jed and assembles the same Model
 * that model.c builds from a netlist file, so a decode needs nothing but the
 * fuse map. The field encodings below were each measured: a design was built
 * that makes the field vary, fitted, and the bits read back.
 *
 *   Clk    3-bit (F29): '000'->GCK0 '010'->GCK1 '001'->GCK2
 *          2-bit (F27): '00'->GCK0  '10'->GCK1  '01'->GCK2
 *   Aclk   '1' -> the FB's control term CTC (PT4)
 *          '0' -> the macrocell's own PTC (3m+10)
 *   R/P    '11' none, '01' GSR, '10' control term CTR/CTS
 *   Oe     all ten modes; CTE is PT7, PTB is 3m+9
 *   CGND   E='1' I='0'  -- drives LOW, not disabled
 *   open   E=macrocell I='0'
 *   RegCom '0' pad driven by the REGISTER, '1' by the XOR gate
 *   XorIn  00->('0',inv0) 01->(PTC,inv1) 10->(PTC,inv0)
 *          11->('0',inv1)
 *   CE     PTC (3m+10), NOT PTA
 *   input register sits BETWEEN the buffer and the ZIA
 *
 * Net ids are opaque; only identity matters. Constants use the reserved ids
 * NET_ZERO / NET_ONE, matching what model.c expects from the JSON's "0"/"1".
 */
#include "jed2vhdl.h"
#include "jedgeom.h"
#include "zia_tables_gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct {
    Model *m;
    int next;
    int mc_out[MAX_FB + 1][MAX_MC + 1];   /* XOR gate output          */
    int reg_out[MAX_FB + 1][MAX_MC + 1];  /* register Q               */
    int pad_net[MAX_FB + 1][MAX_MC + 1];  /* the physical pad         */
    int read_net[MAX_FB + 1][MAX_MC + 1]; /* what the buffer delivers */
    int pt_net[MAX_FB + 1][MAX_PTERM];    /* product term outputs     */
} Build;

static int newnet(Build *b, const char *fmt, int x, int y) {
    int id = b->next++;
    if (id >= MAX_NET) return NET_ZERO;
    if (fmt) {
        char buf[64];
        snprintf(buf, sizeof(buf), fmt, x, y);
        b->m->id2name[id] = strdup(buf);
    }
    return id;
}

/* ---- ZIA: pattern -> choice index -> (kind, fb, mc) -------------------- */

static const ZiaTable *zia_for(int density) {
    for (int i = 0; i < ZIA_NTABLES; i++)
        if (zia_tables[i].density == density) return &zia_tables[i];
    return NULL;
}

/* Returns the packed entry, or ZIA_NONE when the row is unused (all-ones).
 * A pattern that is neither all-ones nor in the table never gets here:
 * zia_check() refuses the whole bitstream before the model is built. */
static int zia_lookup(const ZiaTable *z, int row, const char *bits) {
    if (!z || row < 0 || row >= z->nrow) return ZIA_NONE;
    for (int i = 0; i < z->npat; i++)
        if (!strcmp(z->pat[i], bits))
            return z->map[row * z->nch + i];
    return ZIA_NONE;
}

/* Every ZIA row of every block must be all-ones (unused) or one of the
 * table's patterns. Anything else is reported by block, row and bits, and the
 * decode is refused: read as unused, the row would drop an input from the
 * logic and the output would be wrong with exit 0. Python prints the same
 * words (ziadecode.message). Returns 1 when every row is accounted for. */
int zia_check(const struct JedGeom *g) {
    const ZiaTable *z = zia_for(jedgeom_density(g));
    if (!z) return 1;
    int ok = 1;
    for (int b = 0; b < jedgeom_nblk(g); b++) {
        int n = jedgeom_zia_n(g, b);
        for (int r = 0; r < n && r < z->nrow; r++) {
            const char *bits = jedgeom_zia_row(g, b, r);
            if (!bits || !strchr(bits, '0')) continue;
            int known = 0;
            for (int i = 0; i < z->npat && !known; i++)
                known = !strcmp(z->pat[i], bits);
            if (!known) {
                fprintf(stderr, "error: ZIA row %d of FB%d holds fuse pattern %s, which "
                                "selects none of that row's inputs.\n       Refusing: an "
                                "unrecognised pattern is not an unused row.\n", r, b + 1, bits);
                ok = 0;
            }
        }
    }
    return ok;
}

/* ---- field helpers ----------------------------------------------------- */

static int fld_eq(const JedGeom *g, int fb, int mc, const char *name,
                  const char *want) {
    char buf[16];
    if (!jedgeom_field(g, fb, mc, name, buf, sizeof(buf))) return 0;
    return !strcmp(buf, want);
}

static const char *fld(const JedGeom *g, int fb, int mc, const char *name,
                       char *buf, size_t n) {
    if (!jedgeom_field(g, fb, mc, name, buf, n)) { buf[0] = 0; return buf; }
    return buf;
}

/* The field is called `InReg` on the 27-bit record and `UseIOB` on the
 * 29-bit one. Reading only the first name makes every input register on a
 * large part invisible. */
static int inreg_in_use(const JedGeom *g, int fb, int mc) {
    char b[16];
    if (jedgeom_field(g, fb, mc, "InReg", b, sizeof(b))) return !strcmp(b, "0");
    if (jedgeom_field(g, fb, mc, "UseIOB", b, sizeof(b))) return !strcmp(b, "0");
    return 0;
}

/* `McFB` when present, else `FB`. A location is not a signal. */
static void fbsrc(const JedGeom *g, int fb, int mc, char *out, size_t n) {
    if (jedgeom_field(g, fb, mc, "McFB", out, n)) return;
    if (jedgeom_field(g, fb, mc, "FB", out, n)) return;
    out[0] = 0;
}

/* ---- the ZIA row a product-term literal refers to ---------------------- */

static int zia_net(Build *b, const JedGeom *g, const ZiaTable *z,
                   int fb, int row) {
    const char *bits = jedgeom_zia_row(g, fb - 1, row);
    if (!bits) return -1;
    int v = zia_lookup(z, row, bits);
    if (v == ZIA_NONE) return -1;
    int kind = ZIA_KIND(v), sfb = ZIA_FB(v), smc = ZIA_MC(v);
    if (sfb < 1 || sfb > MAX_FB || smc < 1 || smc > MAX_MC) return -1;
    if (kind == ZIA_KIND_MC) {
        char f[16];
        fbsrc(g, sfb, smc, f, sizeof(f));
        /* PROVENANCE: MEASURED feedback code 10 = from the flip-flop, 00 = XOR gate: §203 fieldderive/README.md */
        return !strcmp(f, "10") ? b->reg_out[sfb][smc] : b->mc_out[sfb][smc];
    }
    /* PAD row. An input register in use means the ZIA reads its Q, unless
     * the macrocell feedback IS that register: then it is already reachable
     * on the `mc` row and this row carries the raw pad. */
    {
        char f[16];
        fbsrc(g, sfb, smc, f, sizeof(f));
        /* PROVENANCE: MEASURED feedback code 10 = from the flip-flop, 00 = XOR gate: §203 fieldderive/README.md */
        if (inreg_in_use(g, sfb, smc) && strcmp(f, "10"))
            return b->reg_out[sfb][smc];
    }
    return b->read_net[sfb][smc];
}

/* ---- the build --------------------------------------------------------- */

Model *model_from_jed(const struct JedGeom *g) {
    Model *m = calloc(1, sizeof(Model));
    if (!m) return NULL;
    Build bb, *b = &bb;
    memset(b, 0, sizeof(bb));
    b->m = m;
    b->next = 2;   /* jed2vhdl.h reserves NET_ZERO/NET_ONE as sentinels */

    const char *dev = jedgeom_dev(g);
    snprintf(m->part_name, sizeof(m->part_name), "%s", dev ? dev : "");
    /* The device string is "XC2C64A-7-VQ100": family, speed, package. The
     * file states it on its own `N DEVICE` line, so nothing is inferred. */
    if (dev) {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "%s", dev);
        char *a = strchr(tmp, '-');
        if (a) {
            *a++ = 0;
            char *bp = strchr(a, '-');
            if (bp) *bp++ = 0;
            /* Each token is truncated to its field rather than refused: a
             * device line longer than the field is malformed, not fatal. */
            snprintf(m->part_name,  sizeof(m->part_name),  "%.31s", tmp);
            snprintf(m->part_speed, sizeof(m->part_speed), "%.7s",  a);
            if (bp) snprintf(m->part_pkg, sizeof(m->part_pkg), "%.31s", bp);
        }
    }
    int nfb = jedgeom_nblk(g);
    m->num_fb = nfb;
    const ZiaTable *z = zia_for(jedgeom_density(g));

    /* one net per macrocell output, register output, pad and readback */
    for (int fb = 1; fb <= nfb; fb++) {
        int nmc = jedgeom_mc_n(g, fb - 1);
        for (int mc = 1; mc <= nmc && mc <= MAX_MC; mc++) {
            b->mc_out[fb][mc]   = newnet(b, "comb_fb%d_mc%d", fb, mc);
            b->reg_out[fb][mc]  = newnet(b, "reg_fb%d_mc%d", fb, mc);
            b->pad_net[fb][mc]  = newnet(b, "PAD_FB%d_%d", fb, mc);
            b->read_net[fb][mc] = newnet(b, "rd_fb%d_mc%d", fb, mc);
        }
    }

    /* Globals. Whether a global net is driven from its pad or tied off is
     * stated by the global-net fuse block, not by whether anything happens to
     * use the net: a design can select GTS1 on three macrocells while the GTS1
     * buffer input is tied off, which means the net is driven by the buried
     * macrocell sitting at the GTS1 location instead. Reading "tied off" as
     * "unused" puts the pad on the net and the wrong signal on three output
     * enables. `global_const` therefore comes from the fuses alone.
     *
     * The buffers also carry an INVERT bit, and for GTS / GSR invert=0 means
     * the pad drives the enable INVERTED. Losing it flips every pad-driven
     * output enable. */
    JedGlobals gl;
    int have_gl = jedgeom_globals(g, &gl);
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    for (int i = 0; i < 3; i++) m->gck[i] = newnet(b, "gck_%d", i, 0);
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    for (int i = 0; i < 4; i++) m->gts[i] = newnet(b, "gts_%d", i, 0);
    m->gsr = newnet(b, "gsr", 0, 0);
    /* A global clock has no enable bit of its own: it is pad-driven exactly
     * when a register selects it, which the register pass below records. */
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    for (int i = 0; i < 3; i++) m->global_const[m->gck[i]] = '0';
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    for (int i = 0; i < 4; i++) {
        m->global_const[m->gts[i]] = (have_gl && gl.gts_enable[i]) ? 0 : '0';
        m->global_invert[m->gts[i]] = have_gl ? gl.gts_invert[i] : 0;
    }
    m->global_const[m->gsr] = (have_gl && gl.gsr_enable) ? 0 : '0';
    m->global_invert[m->gsr] = have_gl ? gl.gsr_invert : 0;

    /* product terms */
    for (int fb = 1; fb <= nfb; fb++) {
        int npt = jedgeom_and_n(g, fb - 1);
        for (int p = 0; p < npt && p < MAX_PTERM; p++) {
            AndTerm *a = &m->andterms[fb][p];
            const char *row = jedgeom_and_row(g, fb - 1, p);
            a->n_true = a->n_comp = 0;
            if (row) {
                int w = (int)strlen(row);
                for (int i = 0; i + 1 < w; i += 2) {
                    int zr = i / 2;
                    /* '0' asserts: the fuse is BLOWN to include the literal. */
                    if (row[i] == '0') {
                        int n = zia_net(b, g, z, fb, zr);
                        /* PROVENANCE: DEFINED capacity */
                        if (n >= 0 && a->n_true < 64) a->true_in[a->n_true++] = n;
                    }
                    if (row[i + 1] == '0') {
                        int n = zia_net(b, g, z, fb, zr);
                        /* PROVENANCE: DEFINED capacity */
                        if (n >= 0 && a->n_comp < 64) a->comp_in[a->n_comp++] = n;
                    }
                }
            }
            a->out = b->pt_net[fb][p] = newnet(b, "pt_fb%d_%d", fb, p);
            a->valid = 1;
        }
    }

    /* OR gates, XOR gates, registers, I/O */
    for (int fb = 1; fb <= nfb; fb++) {
        int nmc = jedgeom_mc_n(g, fb - 1);
        int npt = jedgeom_and_n(g, fb - 1);
        for (int mc = 1; mc <= nmc && mc <= MAX_MC; mc++) {
            OrTerm *o = &m->orterms[fb][mc];
            o->n_in = 0;
            for (int p = 0; p < npt && p < MAX_PTERM; p++) {
                const char *orow = jedgeom_or_row(g, fb - 1, p);
                if (orow && (int)strlen(orow) >= mc && orow[mc - 1] == '0'
                    && o->n_in < MAX_PTERM)
                    o->in[o->n_in++] = b->pt_net[fb][p];
            }
            o->out = newnet(b, "or_fb%d_mc%d", fb, mc);
            o->valid = 1;

            char xin[16];
            fld(g, fb, mc, "XorIn", xin, sizeof(xin));
            XorGate *x = &m->xorgates[fb][mc];
            /* PROVENANCE: MEASURED own terms PTA=8+3k, PTB=9+3k, PTC=10+3k (k = mc-1), clean-room: ctlderive/README.md (§208) */
            int ptc = (3 * (mc - 1) + 10) % (npt ? npt : 1);
            /* PROVENANCE: MEASURED XorIn 00 const 0, 11 const 1, 10 the PTC-row term, 01 its complement: ctlderive/README.md (§208) */
            if (!strcmp(xin, "00"))      { x->ptc = NET_ZERO; x->invert = 0; }
            /* PROVENANCE: MEASURED XorIn 00 const 0, 11 const 1, 10 the PTC-row term, 01 its complement: ctlderive/README.md (§208) */
            else if (!strcmp(xin, "11")) { x->ptc = NET_ZERO; x->invert = 1; }
            /* PROVENANCE: MEASURED XorIn 00 const 0, 11 const 1, 10 the PTC-row term, 01 its complement: ctlderive/README.md (§208) */
            else if (!strcmp(xin, "10")) { x->ptc = b->pt_net[fb][ptc]; x->invert = 0; }
            else                         { x->ptc = b->pt_net[fb][ptc]; x->invert = 1; }
            x->orterm = o->out;
            x->out = b->mc_out[fb][mc];
            x->valid = 1;

            /* register */
            Reg *r = &m->regs[fb][mc];
            char rm[16], clk[16], aclk[16], rr[16], pp[16];
            fld(g, fb, mc, "RegMod", rm, sizeof(rm));
            fld(g, fb, mc, "Clk", clk, sizeof(clk));
            fld(g, fb, mc, "Aclk", aclk, sizeof(aclk));
            fld(g, fb, mc, "R", rr, sizeof(rr));
            fld(g, fb, mc, "P", pp, sizeof(pp));
            /* PROVENANCE: MEASURED register type 00 D, 01 latch, 10 T (§203), 11 D + clock enable (§206 valuecamp.py) */
            r->is_latch    = !strcmp(rm, "01");
            /* PROVENANCE: MEASURED register type 00 D, 01 latch, 10 T (§203), 11 D + clock enable (§206 valuecamp.py) */
            r->is_tff      = !strcmp(rm, "10");
            /* PROVENANCE: MEASURED register type 00 D, 01 latch, 10 T (§203), 11 D + clock enable (§206 valuecamp.py) */
            r->has_ce      = !strcmp(rm, "11");
            { const char *cfg = jedgeom_mc(g, fb - 1, mc - 1); r->is_dualedge = cfg ? jedgeom_is_ddr(cfg) : 0; r->init = cfg ? jedgeom_init_state(cfg) : 0; }
            r->invert_clk  = fld_eq(g, fb, mc, "ClkOp", "1");
            /* CELL TYPE. The base name is not enough: a DUAL-EDGE register is
             * FDDCP/FDDCPE/FTDCP, and an INVERTED clock appends `_N`, so the
             * base name alone names the wrong cell for either.
             *
             * A latch's dual-edge form does not exist -- `is_dualedge` reads
             * the same bit but means nothing for LDCP -- so it is not applied
             * there. */
            {
                const char *base = r->is_latch ? "LDCP"
                                 : r->is_tff   ? (r->is_dualedge ? "FTDCP" : "FTCP")
                                 : r->has_ce   ? (r->is_dualedge ? "FDDCPE" : "FDCPE")
                                 :               (r->is_dualedge ? "FDDCP"  : "FDCP");
                snprintf(r->celltype, sizeof(r->celltype), "%s%s",
                         base, r->invert_clk ? "_N" : "");
            }


            int sel = -1;
            /* PROVENANCE: VENDOR record widths 27 / 29 / 16 bits: the N-lines ISE writes into every JED (§47, §57) */
            if (strlen(clk) >= 3) {
                /* PROVENANCE: MEASURED 3-bit clock codes 000 GCK0, 010 GCK1, 001 GCK2: §148, §203 */
                if (!strcmp(clk, "000")) sel = 0;
                /* PROVENANCE: MEASURED 3-bit clock codes 000 GCK0, 010 GCK1, 001 GCK2: §148, §203 */
                else if (!strcmp(clk, "010")) sel = 1;
                /* PROVENANCE: MEASURED 3-bit clock codes 000 GCK0, 010 GCK1, 001 GCK2: §148, §203 */
                else if (!strcmp(clk, "001")) sel = 2;
            } else if (strlen(clk) == 2) {
                /* PROVENANCE: MEASURED 2-bit clock codes 00 GCK0, 10 GCK1, 01 GCK2: §150 */
                if (!strcmp(clk, "00")) sel = 0;
                /* PROVENANCE: MEASURED 2-bit clock codes 00 GCK0, 10 GCK1, 01 GCK2: §150 */
                else if (!strcmp(clk, "10")) sel = 1;
                /* PROVENANCE: MEASURED 2-bit clock codes 00 GCK0, 10 GCK1, 01 GCK2: §150 */
                else if (!strcmp(clk, "01")) sel = 2;
            }
            if (sel >= 0) {
                r->clk = m->gck[sel];
                m->global_const[m->gck[sel]] = 0;   /* pad-driven */
            } else {
                /* `Aclk` exists on the 27-bit record ONLY -- F29 has no such
                 * field, because the 3-bit `Clk` carries there what F27 splits
                 * across `Clk`(2) + `Aclk`(1). So an ABSENT `Aclk` must mean
                 * the shared control term, not the macrocell's own PTC, and
                 * the test has to be written that way round: PTC only when
                 * `Aclk` is explicitly "0", CTC otherwise. Testing for "1"
                 * instead sends every large-die control-term clock to the
                 * wrong product term. */
                /* §208 (D206-PTC): on the 29/16-bit record the 3-bit Clk
                 * says it -- 011 own PTC, 111 CTC (ctlderive/). The absent
                 * Aclk sent every 29/16-bit PTC clock to CTC. */
                /* PROVENANCE: MEASURED clock 011 PTC / 111 CTC (29/16-bit), Aclk 0 PTC / 1 CTC with Clk 11 (27-bit): ctlderive/README.md (§208) */
                int own_ptc = strlen(clk) >= 3 ? !strcmp(clk, "011") : !strcmp(aclk, "0");
                r->clk = own_ptc ? b->pt_net[fb][ptc]
                                            /* PROVENANCE: MEASURED shared terms CTC=4, CTR=5, CTS=6, CTE=7, clean-room: ctlderive/README.md (§208) */
                                            : b->pt_net[fb][4];
            }
            /* PROVENANCE: MEASURED R/P 11 none, 01 GSR, 10 CTR/CTS (rows 5/6), 00 PTA (row 8+3k): ctlderive/README.md (§208) */
            r->clr = !strcmp(rr, "01") ? m->gsr
                   /* PROVENANCE: MEASURED shared terms CTC=4, CTR=5, CTS=6, CTE=7, clean-room: ctlderive/README.md (§208) */
                   : !strcmp(rr, "10") ? b->pt_net[fb][5]
                   /* §208 (D206-PTA): 00 is the macrocell's own PTA, row 8+3k; it was NET_ZERO. */
                   /* PROVENANCE: MEASURED own terms PTA=8+3k, PTB=9+3k, PTC=10+3k (k = mc-1), clean-room: ctlderive/README.md (§208) */
                   : !strcmp(rr, "00") ? b->pt_net[fb][(3 * (mc - 1) + 8) % (npt ? npt : 1)] : NET_ZERO;
            /* PROVENANCE: MEASURED R/P 11 none, 01 GSR, 10 CTR/CTS (rows 5/6), 00 PTA (row 8+3k): ctlderive/README.md (§208) */
            r->pre = !strcmp(pp, "01") ? m->gsr
                   /* PROVENANCE: MEASURED shared terms CTC=4, CTR=5, CTS=6, CTE=7, clean-room: ctlderive/README.md (§208) */
                   : !strcmp(pp, "10") ? b->pt_net[fb][6]
                   /* PROVENANCE: MEASURED own terms PTA=8+3k, PTB=9+3k, PTC=10+3k (k = mc-1), clean-room: ctlderive/README.md (§208) */
                   : !strcmp(pp, "00") ? b->pt_net[fb][(3 * (mc - 1) + 8) % (npt ? npt : 1)] : NET_ZERO;
            /* PROVENANCE: MEASURED R/P 11 none, 01 GSR, 10 CTR/CTS (rows 5/6), 00 PTA (row 8+3k): ctlderive/README.md (§208) */
            if (!strcmp(rr, "01") || !strcmp(pp, "01"))
                m->global_const[m->gsr] = 0;
            /* A register with no clock enable must carry NET_NONE, the
             * sentinel emit.c tests:
             *     ce_sig = (r->ce != NET_NONE) ? gen_control_sig(...) : NULL;
             * NET_ONE and 0 are both REAL net ids to that test, so either one
             * makes the emitter write `if net_0 = '1'` into the process, which
             * no VHDL analyser accepts. */
            r->ce = r->has_ce ? b->pt_net[fb][ptc] : NET_NONE;
            /* an input register captures the BUFFER OUTPUT, not the raw
             * pad -- wiring D to the pad leaves the buffer unread and the pad
             * is declared `out` instead of `inout`. */
            r->d = inreg_in_use(g, fb, mc) ? b->read_net[fb][mc]
                                           : b->mc_out[fb][mc];
            r->q = b->reg_out[fb][mc];
            r->valid = 1;

            /* I/O buffer */
            /* PROVENANCE: VENDOR record widths 27 / 29 / 16 bits: the N-lines ISE writes into every JED (§47, §57) */
            if (jedgeom_mc_w(g, fb - 1, mc - 1) == 16) continue;  /* buried */
            Iob *io = &m->iob[fb][mc];
            char oe[16];
            fld(g, fb, mc, "Oe", oe, sizeof(oe));
            const char *mode =
                /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111 (§203), 1010 0110 (§206), 0100 PTB and 1000 CTE with their rows: ctlderive/README.md (§208) */
                !strcmp(oe, "1111") ? "disabled"   :
                /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111 (§203), 1010 0110 (§206), 0100 PTB and 1000 CTE with their rows: ctlderive/README.md (§208) */
                !strcmp(oe, "0000") ? "push-pull"  :
                /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111 (§203), 1010 0110 (§206), 0100 PTB and 1000 CTE with their rows: ctlderive/README.md (§208) */
                !strcmp(oe, "0001") ? "open-drain" :
                /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111 (§203), 1010 0110 (§206), 0100 PTB and 1000 CTE with their rows: ctlderive/README.md (§208) */
                !strcmp(oe, "1100") ? "GTS0" : !strcmp(oe, "0010") ? "GTS1" :
                /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111 (§203), 1010 0110 (§206), 0100 PTB and 1000 CTE with their rows: ctlderive/README.md (§208) */
                !strcmp(oe, "1010") ? "GTS2" : !strcmp(oe, "0110") ? "GTS3" :
                /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111 (§203), 1010 0110 (§206), 0100 PTB and 1000 CTE with their rows: ctlderive/README.md (§208) */
                !strcmp(oe, "0100") ? "PTB"  : !strcmp(oe, "1000") ? "CTE"  :
                /* PROVENANCE: MEASURED output-enable codes 0000 1100 0010 0001 1110 1111 (§203), 1010 0110 (§206), 0100 PTB and 1000 CTE with their rows: ctlderive/README.md (§208) */
                !strcmp(oe, "1110") ? "CGND" : "disabled";
            snprintf(io->obuf_mode, sizeof(io->obuf_mode), "%s", mode);
            int drive = fld_eq(g, fb, mc, "RegCom", "0") ? b->reg_out[fb][mc]
                                                         : b->mc_out[fb][mc];
            io->pad      = b->pad_net[fb][mc];
            io->readback = b->read_net[fb][mc];
            io->drive    = drive;
            if (!strcmp(mode, "disabled")) { io->has_output = 0; io->oe = NET_ZERO; }
            else if (!strcmp(mode, "push-pull")) { io->has_output = 1; io->oe = NET_ONE; }
            else if (!strcmp(mode, "open-drain")) {
                io->has_output = 1; io->oe = drive; io->drive = NET_ZERO;
            } else if (!strcmp(mode, "CGND")) {
                io->has_output = 1; io->oe = NET_ONE; io->drive = NET_ZERO;
            } else if (!strcmp(mode, "PTB")) {
                io->has_output = 1;
                /* PROVENANCE: MEASURED own terms PTA=8+3k, PTB=9+3k, PTC=10+3k (k = mc-1), clean-room: ctlderive/README.md (§208) */
                io->oe = b->pt_net[fb][(3 * (mc - 1) + 9) % (npt ? npt : 1)];
            } else if (!strcmp(mode, "CTE")) {
                /* PROVENANCE: MEASURED shared terms CTC=4, CTR=5, CTS=6, CTE=7, clean-room: ctlderive/README.md (§208) */
                io->has_output = 1; io->oe = b->pt_net[fb][7];
            } else {                                     /* GTS0..GTS3 */
                /* PROVENANCE: DEFINED index read from our own mode name 'GTSn' */
                int gi = mode[3] - '0';
                io->has_output = 1;
                /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
                io->oe = m->gts[gi & 3];
                /* Selecting a GTS says nothing about where that GTS comes
                 * from; its fuse block, read above, is what says that. */
            }
            io->valid = 1;
        }
    }

    /* PORT NAMES and the readback redirect.
     *
     * `gen_leaf_name` names a literal that reads a pad through
     * `readback_to_port`, which is derived from `portname_of_id`. Both must be
     * filled here, or a pad reachable ONLY through its readback -- an
     * INPUT-ONLY macrocell, `Oe=1111` -- has no name and its literal prints as
     * nothing. A macrocell feeding its block's CTC clock term is the usual
     * case.
     */
    for (int fb = 1; fb <= nfb; fb++) {
        int nmc = jedgeom_mc_n(g, fb - 1);
        for (int mc = 1; mc <= nmc && mc <= MAX_MC; mc++) {
            Iob *io = &m->iob[fb][mc];
            if (!io->valid) continue;
            char nm[40];
            snprintf(nm, sizeof(nm), "PAD_FB%d_%d", fb, mc);
            if (io->pad >= 0 && io->pad < MAX_NET && !m->portname_of_id[io->pad])
                m->portname_of_id[io->pad] = strdup(nm);
            if (io->readback >= 0 && io->readback < MAX_NET
                && io->pad >= 0 && io->pad < MAX_NET
                && m->portname_of_id[io->pad])
                m->readback_to_port[io->readback] =
                    strdup(m->portname_of_id[io->pad]);
        }
    }
    return m;
}
