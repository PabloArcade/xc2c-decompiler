#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/wait.h>
#endif
#include "jed2vhdl.h"
#include "jedgeom.h"

#define XC2CDUMP_VERSION "v9.7"
#define XC2CDUMP_BANNER \
    "xc2cdump " XC2CDUMP_VERSION " -- CoolRunner-II .jed decompiler by Darksoft. " \
    "Use at your own risk."

static void banner(void) { fprintf(stderr, "%s\n", XC2CDUMP_BANNER); }

static void usage(const char *prog) {
    fprintf(stderr,
"usage: %s <command> [options] <file.jed> [outputs]\n"
"\n"
"commands:\n"
"  decode  <file.jed>                      structural report, one block per macrocell\n"
"  vhdl    <file.jed> [out.vhd] [out.ucf]  VHDL plus a matching constraints file\n"
"  info    <file.jed>                      device, fuse-map geometry, fuse counts\n"
"  dump    <file.jed>                      device and how many fuses are set\n"
"  used    <file.jed>                      which macrocells the design uses, per block\n"
"  config  <file.jed> <fb> <mc>            one macrocell\'s configuration fields\n"
"  diff    <a.jed> <b.jed>                 fuse ranges that differ between two bitstreams\n"
"  verify  <file.jed> <file.ucf>           check a UCF\'s pins against a bitstream\n"
"  pins    <device> <package>              the pin table for one part\n"
"  devices                                 supported devices and packages\n"
"  --version                               version line only\n"
"\n"
"options:\n"
"  --device NAME       override the device named in the JEDEC header (same density)\n"
"  --package NAME      override the package\n"
"  --ucf FILE          read an existing UCF to recover real signal names\n"
"  --entity NAME       entity name for the generated VHDL\n"
"  --lock-macrocells   add INST LOC constraints for buried macrocells\n"
"  --no-pin-table      name ports PAD_FBx_y, not by pin (the UCF keeps its LOCs)\n"
"  --json FILE         use an already-decoded netlist\n"
"\n"
"NOTE: when rebuilding the emitted VHDL in ISE, cpldfit MUST be given\n"
"      -unused float, or every unused pin is driven low.\n",
        prog);
}

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

/* ------------------------------------------------------------------
 * Shared load path: JED -> Model + Generator.
 * ------------------------------------------------------------------ */
typedef struct {
    const char *device, *package, *ucf, *entity, *json;
    int no_pin_table, lock_macrocells;
} Opts;


static int load_design(const char *jed_path, const Opts *o, Model **out_m, Generator **out_g) {
    const char *json_path = o->json;

    /* The netlist is assembled from the fuse map itself: modelgen.c builds the
     * same Model that model.c builds from a netlist file, so nothing is
     * spawned and no other program is needed. --json still reads a netlist
     * written earlier. */
    Model *m_own = NULL;
    if (!json_path) {
        /* Say which of the two went wrong. A mistyped path and a file whose
         * headers are missing are different problems and read the same way
         * once both are reported as "cannot be read". */
        FILE *probe = fopen(jed_path, "rb");
        if (!probe) {
            fprintf(stderr, "error: cannot open %s\n", jed_path);
            return 0;
        }
        fclose(probe);
        JedGeom *jg = jedgeom_read(jed_path);
        if (jg) {
            if (!zia_check(jg)) {      /* already reported, row by row */
                jedgeom_free(jg);
                return 0;
            }
            m_own = model_from_jed(jg);
            jedgeom_free(jg);
        }
        if (!m_own) {
            fprintf(stderr, "error: %s carries no fuse-map section headers, so its\n"
                            "       geometry cannot be read.\n", jed_path);
            return 0;
        }
    }

    Model *m = m_own;
    if (!m) {
        m = model_load(json_path);
        if (!m) { fprintf(stderr, "error: cannot read netlist %s\n", json_path); return 0; }
    }
    /* --device names the part whose PIN TABLE is used; it cannot change the
     * fuse map, whose block count is already read from the file. A part of
     * another density gave exit 0 with every LOC taken from the wrong part's
     * pinout (§206), so the override must agree with the geometry: the A and
     * non-A parts of one density share it, nothing else does. */
    if (o->device) {
        /* PROVENANCE: VENDOR function blocks per density: ISE device files (num_of_fbs), ziaderive/chp.py (§206) */
        static const struct { const char *name; int fbs; } dens[] = {
            {"XC2C32A", 2}, {"XC2C32", 2}, {"XC2C64A", 4}, {"XC2C64", 4},
            {"XC2C128", 8}, {"XC2C256", 16}, {"XC2C384", 24}, {"XC2C512", 32}};
        int want = -1, have = m_own ? m->num_fb : -1;
        for (size_t k = 0; k < sizeof dens / sizeof dens[0]; k++) {
            if (strcasecmp(o->device, dens[k].name) == 0) want = dens[k].fbs;
            /* a --json netlist: its own part name, not its highest used FB */
            if (!m_own && strcasecmp(m->part_name, dens[k].name) == 0) have = dens[k].fbs;
        }
        if (want < 0) {
            fprintf(stderr, "error: --device %s is not a CoolRunner-II part this tool knows\n"
                            "       (see `devices`).\n", o->device);
            return 0;
        }
        if (have >= 0 && want != have) {
            fprintf(stderr, "error: --device %s has %d function blocks, but this fuse map has %d.\n"
                            "       The override chooses a pin table; it cannot change the\n"
                            "       density the bitstream was built for.\n",
                    o->device, want, have);
            return 0;
        }
        snprintf(m->part_name, sizeof(m->part_name), "%s", o->device);
        for (char *c = m->part_name; *c; c++) *c = (char)toupper((unsigned char)*c);
    }
    if (o->package) snprintf(m->part_pkg,  sizeof(m->part_pkg),  "%s", o->package);

    UcfFile *uf = NULL;
    if (o->ucf) uf = ucf_parse(o->ucf);

    Generator *g = generator_new(m, uf, !o->no_pin_table,
                                 o->entity ? o->entity : "cpld_top");
    generator_mark_needed(g);
    *out_m = m; *out_g = g;
    return 1;
}

/* ---- fuse-array helpers for dump/diff ---- */
static int *read_fuses(const char *path, int *out_qf, char *dev, size_t devsz) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f); buf[got] = 0; fclose(f);

    char *q = strstr(buf, "QF");
    if (!q) { fprintf(stderr, "error: %s has no QF field\n", path); free(buf); return NULL; }
    int qf = atoi(q + 2);
    if (qf <= 0) { free(buf); return NULL; }

    if (dev) {
        dev[0] = 0;
        char *d = strstr(buf, "N DEVICE");
        if (d) {
            /* PROVENANCE: DEFINED length of 'N DEVICE' */
            d += 8; while (*d == ' ') d++;
            size_t i = 0;
            while (d[i] && d[i] != '*' && d[i] != '\r' && d[i] != '\n' && i < devsz - 1) { dev[i] = d[i]; i++; }
            dev[i] = 0;
        }
    }

    int deflt = 0;
    char *fp = strstr(buf, "*F");
    if (fp && (fp[2] == '0' || fp[2] == '1')) deflt = fp[2] - '0';

    int *fuses = malloc(sizeof(int) * (size_t)qf);
    for (int i = 0; i < qf; i++) fuses[i] = deflt;

    char *p = buf;
    while ((p = strchr(p, 'L')) != NULL) {
        if (p != buf && p[-1] != '\n' && p[-1] != '\r' && p[-1] != '*') { p++; continue; }
        /* PROVENANCE: DEFINED decimal */
        char *e; long addr = strtol(p + 1, &e, 10);
        if (e == p + 1) { p++; continue; }
        while (*e == ' ' || *e == '\r' || *e == '\n') e++;
        long idx = addr;
        while (*e && *e != '*') {
            if (*e == '0' || *e == '1') { if (idx < qf) fuses[idx] = *e - '0'; idx++; }
            e++;
        }
        p = (*e) ? e : p + 1;
    }
    free(buf);
    *out_qf = qf;
    return fuses;
}


/* ---- info / used / config / pins. Shapes follow xc9500dump and
 * xc7300dump. `info` and `config` read the fuse map directly, so they work on
 * bitstreams that no netlist could represent. ---- */

static int cmd_info(const char *path) {
    int qf = 0; char dev[128];
    int *f = read_fuses(path, &qf, dev, sizeof(dev));
    if (!f) return 1;
    long ones = 0; for (int i = 0; i < qf; i++) ones += f[i];
    free(f);

    printf("file            : %s\n", path);
    printf("part            : %s\n", dev[0] ? dev : "(not stated)");
    if (!strncmp(dev, "XC2C", 4)) {
        char fam[16]; size_t i = 0;
        while (i < sizeof(fam) - 1 && dev[i] && dev[i] != '-') { fam[i] = dev[i]; i++; }
        fam[i] = 0;
        printf("family          : %s\n", fam);
    }
    JedGeom *g = jedgeom_read(path);
    if (g) {
        int nb = jedgeom_nblk(g);
        printf("  function blocks: %d\n", nb);
        printf("  macrocells     : %d  (%d per block)\n",
               nb * jedgeom_nmc(g, 0), jedgeom_nmc(g, 0));
        printf("  ZIA            : %d rows x %d bits\n", jedgeom_zia_n(g, 0), jedgeom_zia_w(g, 0));
        printf("  PLA AND        : %d rows x %d bits\n", jedgeom_and_n(g, 0), jedgeom_and_w(g, 0));
        printf("  PLA OR         : %d rows x %d bits\n", jedgeom_or_n(g, 0), jedgeom_or_w(g, 0));
        printf("  MC config      : %d bits  (%s-device layout)\n",
               /* PROVENANCE: VENDOR record widths 27 / 29 / 16 bits: the N-lines ISE writes into every JED (§47, §57) */
               jedgeom_mcw(g, 0), jedgeom_mcw(g, 0) == 29 ? "large" : "small");
        jedgeom_free(g);
    } else {
        printf("  (no \"Note Block\" headers -- the geometry cannot be read\n");
        printf("   from this file)\n");
    }
    printf("\n");
    printf("QF (fuse count) : %d\n", qf);
    printf("fuses set   (1) : %ld\n", ones);
    printf("fuses clear (0) : %ld   (%.2f%% of the array)\n",
           qf - ones, qf ? 100.0 * (double)(qf - ones) / qf : 0.0);
    return 0;
}

static int cmd_config_c(const char *path, int fb, int mc) {
    JedGeom *g = jedgeom_read(path);
    if (!g) {
        fprintf(stderr, "error: no \"Note Block\" headers in %s, so the fuse\n"
                        "       geometry cannot be derived from the file.\n", path);
        return 2;
    }
    int rc = jedgeom_print_config(g, fb, mc);
    jedgeom_free(g);
    return rc;
}

static int cmd_pins_c(const char *device, const char *pkg) {
    int any = 0;
    /* 32 function blocks: an XC2C512 has 512 macrocells across 32 of them,
     * and a lower bound here silently truncates its pin table. */
    /* PROVENANCE: VENDOR largest part: 32 function blocks of 16 macrocells and 56 product terms, ISE device files and fitter reports, ziaderive/chp.py (§206) */
    for (int fb = 1; fb <= 32; fb++)
        for (int mc = 1; mc <= 16; mc++) {
            const char *s = NULL;
            if (pin_table_lookup_str(device, pkg, fb, mc, &s)) any++;
        }
    if (!any) {
        fprintf(stderr, "error: no pin table for %s/%s. `devices` lists what is known.\n",
                device, pkg);
        return 2;
    }
    printf("%s / %s   %d bonded pads\n\n", device, pkg, any);
    printf("  %-12s %s\n", "macrocell", "pin");
    /* PROVENANCE: VENDOR largest part: 32 function blocks of 16 macrocells and 56 product terms, ISE device files and fitter reports, ziaderive/chp.py (§206) */
    for (int fb = 1; fb <= 32; fb++) {
        for (int mc = 1; mc <= 16; mc++) {
            const char *s = NULL;
            /* str form, not the numeric one: a BGA ball is "C1", not a number */
            if (pin_table_lookup_str(device, pkg, fb, mc, &s))
                printf("  FB%d_MC%-6d %s\n", fb, mc, s);
        }
    }
    return 0;
}

static int cmd_dump(const char *path) {
    int qf = 0; char dev[128];
    int *f = read_fuses(path, &qf, dev, sizeof(dev));
    if (!f) return 1;
    long ones = 0; for (int i = 0; i < qf; i++) ones += f[i];
    printf("file   : %s\n", path);
    printf("device : %s\n", dev[0] ? dev : "not stated");
    printf("fuses  : %d\n", qf);
    printf("set    : %ld (%.1f%%)\n", ones, qf ? 100.0 * (double)ones / qf : 0.0);
    printf("clear  : %ld\n", (long)qf - ones);
    free(f);
    return 0;
}

static int cmd_diff(const char *pa, const char *pb) {
    /* PROVENANCE: DEFINED text buffers */
    int qa = 0, qb = 0; char da[128], db[128];
    int *fa = read_fuses(pa, &qa, da, sizeof(da)); if (!fa) return 1;
    int *fb = read_fuses(pb, &qb, db, sizeof(db)); if (!fb) { free(fa); return 1; }
    printf("a: %s  (QF=%d, %s)\n", pa, qa, da[0] ? da : "?");
    printf("b: %s  (QF=%d, %s)\n\n", pb, qb, db[0] ? db : "?");
    if (da[0] && db[0] && strcmp(da, db) != 0)
        printf("WARNING: different devices. A fuse comparison across parts means nothing.\n\n");
    if (qa != qb) { printf("DIFFER: fuse counts do not match (%d vs %d).\n", qa, qb); free(fa); free(fb); return 1; }
    long n = 0; for (int i = 0; i < qa; i++) if (fa[i] != fb[i]) n++;
    if (!n) { printf("IDENTICAL: all %d fuses match.\n", qa); free(fa); free(fb); return 0; }
    printf("DIFFER: %ld of %d fuses (%.3f%%)\n\n", n, qa, 100.0 * (double)n / qa);
    printf("NOTE: a fuse-level percentage is NOT a measure of decompiler quality.\n");
    printf("      Unprogrammed fuses match each other, so a decode that lost logic\n");
    printf("      still scores well. Use a behavioural round-trip.\n\n");
    int first = -1, prev = -1, count = 0, shown = 0;
    for (int i = 0; i < qa; i++) {
        if (fa[i] == fb[i]) continue;
        if (first < 0) { first = prev = i; count = 1; continue; }
        /* PROVENANCE: DEFINED diff clustering and display limits */
        if (i - prev > 64) {
            if (shown < 25) printf("  [%6d - %6d]  %d fuses\n", first, prev, count);
            shown++; first = i; count = 0;
        }
        prev = i; count++;
    }
    /* PROVENANCE: DEFINED diff display limit */
    if (first >= 0 && shown < 25) printf("  [%6d - %6d]  %d fuses\n", first, prev, count);
    free(fa); free(fb);
    return 1;
}

/* decode, used and verify read generator state that is filled in only while
 * the VHDL body is built, so they build it into the null device first. If
 * that cannot be done the state is incomplete and the command would describe
 * the design wrongly -- for verify, pass a UCF that does not match -- so the
 * caller refuses instead. */
static int build_into_sink(Generator *g, const char *jed_filename) {
    FILE *sink = fopen(NULLDEV, "w");
    if (!sink) {
        fprintf(stderr, "error: cannot open %s to build the design\n", NULLDEV);
        return 0;
    }
    int rc = emit_vhdl(g, jed_filename, sink);
    fclose(sink);
    return rc == 0;
}

int main(int argc, char **argv) {
    /* The banner goes to stderr, first, on every run, so that it never mixes
     * with a redirected decode. */
    banner();
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--version") == 0) return 0;
    if (argc < 2) { usage(argv[0]); return 1; }

    Opts o; memset(&o, 0, sizeof(o));
    char *rest[64]; int nrest = 0;
    rest[nrest++] = argv[0];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc)      { o.device = argv[++i]; continue; }
        if (strncmp(argv[i], "--device=", 9) == 0)                 { o.device = argv[i] + 9; continue; }
        if (strcmp(argv[i], "--package") == 0 && i + 1 < argc)     { o.package = argv[++i]; continue; }
        if (strncmp(argv[i], "--package=", 10) == 0)               { o.package = argv[i] + 10; continue; }
        if (strcmp(argv[i], "--ucf") == 0 && i + 1 < argc)         { o.ucf = argv[++i]; continue; }
        if (strcmp(argv[i], "--entity") == 0 && i + 1 < argc)      { o.entity = argv[++i]; continue; }
        if (strcmp(argv[i], "--json") == 0 && i + 1 < argc)        { o.json = argv[++i]; continue; }
        if (strcmp(argv[i], "--lock-macrocells") == 0 ||
            strcmp(argv[i], "--ucf-lock-macrocells") == 0)         { o.lock_macrocells = 1; continue; }
        if (strcmp(argv[i], "--no-pin-table") == 0)                { o.no_pin_table = 1; continue; }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { usage(argv[0]); return 0; }
        /* PROVENANCE: DEFINED argument handling */
        if (nrest < 64) rest[nrest++] = argv[i];
    }

    if (nrest < 2) { usage(argv[0]); return 1; }
    const char *cmd = rest[1];

    if (strcmp(cmd, "devices") == 0) { pintable_list(); return 0; }
    /* PROVENANCE: DEFINED argument counts */
    if (nrest < 3) { usage(argv[0]); return 1; }

    if (strcmp(cmd, "dump") == 0) return cmd_dump(rest[2]);
    if (strcmp(cmd, "info") == 0) return cmd_info(rest[2]);
    if (strcmp(cmd, "config") == 0) {
        /* PROVENANCE: DEFINED argument counts */
        if (nrest < 5) { fprintf(stderr, "usage: config <file.jed> <fb> <mc>\n"); return 2; }
        return cmd_config_c(rest[2], atoi(rest[3]), atoi(rest[4]));
    }
    if (strcmp(cmd, "pins") == 0) {
        /* PROVENANCE: DEFINED argument counts */
        if (nrest < 4) { fprintf(stderr, "usage: pins <device> <package>\n"); return 2; }
        return cmd_pins_c(rest[2], rest[3]);
    }
    if (strcmp(cmd, "diff") == 0) {
        /* PROVENANCE: DEFINED argument counts */
        if (nrest < 4) { usage(argv[0]); return 1; }
        return cmd_diff(rest[2], rest[3]);
    }

    Model *m = NULL; Generator *g = NULL;

    if (strcmp(cmd, "decode") == 0) {
        if (!load_design(rest[2], &o, &m, &g)) return 1;
        /* globals_used is populated as a side effect of building the VHDL
         * body, so build it into a throwaway sink first -- otherwise decode
         * reports every global as unused, and decode and vhdl would be
         * describing the same design differently. */
        if (!build_into_sink(g, basename_of(rest[2]))) return 1;
        emit_decode(g, basename_of(rest[2]), stdout);
        return 0;
    }

    if (strcmp(cmd, "used") == 0) {
        if (!load_design(rest[2], &o, &m, &g)) return 1;
        /* same reason as decode: the dead-code sets are populated while the
         * VHDL body is built, so build it into a sink first. */
        if (!build_into_sink(g, basename_of(rest[2]))) return 1;
        emit_used(g, stdout);
        return 0;
    }

    if (strcmp(cmd, "verify") == 0) {
        /* PROVENANCE: DEFINED argument counts */
        if (nrest < 4) { usage(argv[0]); return 2; }
        /* exit 0 consistent, 1 does not match, 2 could not check */
        if (!load_design(rest[2], &o, &m, &g)) return 2;
        /* Global-clock and global-control pads are marked used while the
         * VHDL body is built. Without them fewer pins count as used, and a
         * UCF that does not match could pass, so failing to build is exit 2. */
        if (!build_into_sink(g, basename_of(rest[2]))) return 2;
        /* PROVENANCE: DEFINED argument index */
        return emit_verify(g, rest[3], stdout);
    }

    if (strcmp(cmd, "vhdl") == 0) {
        if (!load_design(rest[2], &o, &m, &g)) return 1;

        /* A design that drives no pin has no entity to write: emitting one
         * gives `port( );`, which is not legal VHDL and shows up only as a
         * parser error well into the file. Refuse it by name instead. */
        {
            int np = 0;
            PortDecl *pd = generator_build_ports(g, &np);
            (void)pd;
            if (np == 0) {
                fprintf(stderr,
                    "error: %s decodes to no input or output pads, so there is no\n"
                    "       entity to write. This is normal for a configuration-probe\n"
                    "       bitstream that drives no pin; there is nothing to rebuild.\n"
                    "       Use `decode` to inspect it instead.\n",
                    basename_of(rest[2]));
                return 2;
            }
        }

        char vhd[1024], ucfp[1024];
        /* PROVENANCE: DEFINED argument index */
        if (nrest > 3) snprintf(vhd, sizeof(vhd), "%s", rest[3]);
        else {
            snprintf(vhd, sizeof(vhd), "%s", rest[2]);
            char *dot = strrchr(vhd, '.'); if (dot) *dot = 0;
            strncat(vhd, ".vhd", sizeof(vhd) - strlen(vhd) - 1);
        }
        /* PROVENANCE: DEFINED argument index */
        if (nrest > 4) snprintf(ucfp, sizeof(ucfp), "%s", rest[4]);
        else {
            snprintf(ucfp, sizeof(ucfp), "%s", vhd);
            size_t L = strlen(ucfp);
            if (L > 4 && strcmp(ucfp + L - 4, ".vhd") == 0) ucfp[L - 4] = 0;
            strncat(ucfp, ".ucf", sizeof(ucfp) - strlen(ucfp) - 1);
        }

        /* ISE requires the entity name to equal the .vhd filename stem. */
        if (!o.entity) {
            const char *b = basename_of(vhd);
            char stem[64]; snprintf(stem, sizeof(stem), "%s", b);
            char *dot = strrchr(stem, '.'); if (dot) *dot = 0;
            snprintf(g->entity_name, sizeof(g->entity_name), "%s", stem);
        }

        FILE *out = fopen(vhd, "w");
        if (!out) { fprintf(stderr, "error: cannot open %s\n", vhd); return 1; }
        int erc = emit_vhdl(g, basename_of(rest[2]), out);
        fclose(out);
        if (erc != 0) { remove(vhd); return 1; }

        int nc = 0, nr = 0;
        for (int fb = 1; fb <= MAX_FB; fb++)
            for (int mc = 1; mc <= MAX_MC; mc++) {
                if (g->needed_comb[fb][mc]) nc++;
                if (g->needed_reg[fb][mc]) nr++;
            }
        printf("wrote %s  (%d combinational macrocells, %d registers, entity %s)\n",
               vhd, nc, nr, g->entity_name);

        FILE *uf = fopen(ucfp, "w");
        if (!uf) { fprintf(stderr, "error: cannot open %s\n", ucfp); return 1; }
        int npin = 0, nunpin = 0;
        emit_ucf(g, basename_of(rest[2]), o.lock_macrocells, uf, &npin, &nunpin);
        fclose(uf);
        printf("wrote %s  (%d pins constrained)\n", ucfp, npin);

        if (nunpin) {
            fprintf(stderr,
                "error: %d pad(s) have no pin number for %s/%s. The UCF is incomplete,\n"
                "       so a rebuild will not reproduce the original placement.\n"
                "       Add the device/package to pintable.c.\n",
                nunpin, m->part_name, m->part_pkg);
            return 1;
        }
        return 0;
    }

    fprintf(stderr, "unknown command: %s\n", cmd);
    usage(argv[0]);
    return 1;
}
