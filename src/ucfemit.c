/*
 * ucfemit.c - emits a Xilinx UCF pin-constraint file matching the generated
 * VHDL, so the output can be recompiled in ISE and the resulting .jed diffed
 * against the original (round-trip validation).
 *
 * A recompile needs cpldfit's -unused float: without it every pin the design
 * does not drive is pulled low, which no UCF can undo.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "jed2vhdl.h"

/* VHDL "NAME(3)" -> UCF "NAME<3>". Scalars are copied unchanged. */
static void ucf_name(const char *vhdl_name, char *out, size_t outsz) {
    const char *paren = strchr(vhdl_name, '(');
    size_t len = strlen(vhdl_name);
    if (paren && len > 0 && vhdl_name[len - 1] == ')') {
        size_t blen = (size_t)(paren - vhdl_name);
        size_t ilen = len - blen - 2; /* digits between ( and ) */
        /* PROVENANCE: DEFINED separator room */
        if (blen + ilen + 3 < outsz) {
            memcpy(out, vhdl_name, blen);
            out[blen] = '<';
            memcpy(out + blen + 1, paren + 1, ilen);
            out[blen + 1 + ilen] = '>';
            out[blen + 2 + ilen] = 0;
            return;
        }
    }
    snprintf(out, outsz, "%s", vhdl_name);
}

void emit_ucf(Generator *g, const char *jed_filename, int lock_macrocells,
              FILE *out, int *out_npinned, int *out_nunpinned) {
    Model *m = g->m;
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    char datebuf[64];
    strftime(datebuf, sizeof(datebuf), "%Y-%m-%d %H:%M:%S", tmv);

    fprintf(out, "# written by xc2cdump\n");
    fprintf(out, "# Entity : %s\n", g->entity_name);
    fprintf(out, "# Device : %s-%s\n", m->part_name, m->part_pkg);
    if (jed_filename) fprintf(out, "# Source : %s\n", jed_filename);
    fprintf(out, "# Date   : %s\n", datebuf);
    fprintf(out, "#\n");
    fprintf(out, "# Purpose: recompile the companion .vhd in ISE with this file as the\n");
    fprintf(out, "# constraints file, then compare the resulting .jed against the\n");
    fprintf(out, "# original. Pin locations are taken from the decoded bitstream, so a\n");
    fprintf(out, "# faithful decompile plus this UCF should reproduce the original\n");
    fprintf(out, "# placement. Fit with -unused float.\n\n");

    /* Work out which vector bases have at least one live element, exactly as
     * generator_build_ports() does, so the .ucf and the .vhd never disagree
     * about which pads exist. */
    int *vec_used = calloc(g->n_vectors ? g->n_vectors : 1, sizeof(int));
    for (int fb = 1; fb <= MAX_FB; fb++)
        for (int mc = 1; mc <= MAX_MC; mc++) {
            if (!m->iob[fb][mc].valid) continue;
            int qualifies = m->iob[fb][mc].has_output || g->named_from_ucf[fb][mc] ||
                            g->used_input_pad[fb][mc];
            if (!qualifies) continue;
            char *name = g->pad_name[fb][mc];
            char *paren = strchr(name, '(');
            if (!paren) continue;
            char base[128];
            size_t blen = (size_t)(paren - name);
            if (blen >= sizeof(base)) blen = sizeof(base) - 1;
            strncpy(base, name, blen);
            base[blen] = 0;
            for (int i = 0; i < g->n_vectors; i++)
                if (strcmp(g->vectors[i].base, base) == 0) vec_used[i] = 1;
        }

    int npinned = 0, nunpinned = 0;
    char unpinned_buf[8192];
    unpinned_buf[0] = 0;

    fprintf(out, "# ---- Pin locations ----\n");
    for (int fb = 1; fb <= MAX_FB; fb++) {
        for (int mc = 1; mc <= MAX_MC; mc++) {
            if (!m->iob[fb][mc].valid) continue;
            if (!generator_pad_is_used(g, fb, mc, vec_used)) continue;

            const char *dir = generator_pin_dir(g, fb, mc);

            char nm[160];
            ucf_name(g->pad_name[fb][mc], nm, sizeof(nm));

            const char *pin_s = NULL;
            if (pin_table_lookup_str(m->part_name, m->part_pkg, fb, mc, &pin_s)) {
                /* Numeric pins take a P prefix; BGA ball IDs do not. */
                int numeric = 1;
                for (const char *p = pin_s; *p; p++)
                    if (*p < '0' || *p > '9') { numeric = 0; break; }
                fprintf(out, "NET \"%s\" LOC = \"%s%s\";   # FB%d_%d, %s\n",
                        nm, numeric ? "P" : "", pin_s, fb, mc, dir);
                npinned++;
            } else {
                char line[256];
                snprintf(line, sizeof(line), "#   %s  -> FB%d_%d  (pin unknown)\n",
                         nm, fb, mc);
                if (strlen(unpinned_buf) + strlen(line) < sizeof(unpinned_buf))
                    strcat(unpinned_buf, line);
                nunpinned++;
            }
        }
    }

    if (nunpinned) {
        fprintf(out, "\n# ---- WARNING: no pin number known for these pads ----\n");
        fprintf(out, "# No built-in pin table entry for %s/%s covers them, so they are\n",
                m->part_name, m->part_pkg);
        fprintf(out, "# left unconstrained and ISE will place them freely. A round-trip\n");
        fprintf(out, "# diff will almost certainly fail while these are unpinned. Add the\n");
        fprintf(out, "# device/package to pintable.c to fix this properly.\n");
        fputs(unpinned_buf, out);
    }

    if (lock_macrocells) {
        /* Lock the REGISTER, never the combinational signal: synthesis folds
         * a macrocell's logic into the register's D input, the signal no
         * longer exists, and a LOC on it stops ngdbuild with an error -- on
         * every packed XC2C128 to XC2C512 tested, before this was changed. A
         * macrocell with no register has only that signal, so it is listed
         * and left unlocked. */
        fprintf(out, "\n# ---- Macrocell placement (opt-in) ----\n");
        fprintf(out, "# Registers only. A combinational signal is often absorbed by\n");
        fprintf(out, "# synthesis, and a LOC on a signal that no longer exists stops\n");
        fprintf(out, "# ngdbuild, so macrocells with no register are listed, not locked.\n");
        for (int fb = 1; fb <= MAX_FB; fb++)
            for (int mc = 1; mc <= MAX_MC; mc++) {
                if (m->iob[fb][mc].valid &&
                    generator_pad_is_used(g, fb, mc, vec_used)) continue;
                if (g->needed_reg[fb][mc])
                    fprintf(out, "INST \"%s\" LOC = \"FB%d_%d\";\n",
                            gen_reg_sig(fb, mc), fb, mc);
                else if (g->needed_comb[fb][mc])
                    fprintf(out, "#   FB%d_%d  combinational only, not locked\n", fb, mc);
            }
    }

    fprintf(out, "\n");
    free(vec_used);
    if (out_npinned) *out_npinned = npinned;
    if (out_nunpinned) *out_nunpinned = nunpinned;
}
