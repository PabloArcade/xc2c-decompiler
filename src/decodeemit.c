/*
 * decodeemit.c -- structural decode report and UCF cross-check.
 *
 * The decode format is the same one xc9500dump prints, so that a script can
 * read either. The indentation and the section names are load-bearing:
 *
 *   [FB1_MC3 pin13   SIGNAME]
 *     clk=GCK1   RISE | ff=D init=RST | oe=1(always-on)
 *     dev: <raw config, a report line, NOT a product term>
 *     D/T input (2 PTs):        <- section header, TWO spaces, ends in ':'
 *       PT1: A AND NOT_B        <- product term, FOUR spaces
 *
 * Literals are ' AND '-joined, negation is a NOT_ prefix, and a term with
 * no literals prints as (empty).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "jed2vhdl.h"

/* A literal's display name. Delegates to the generator's own resolver so
 * that decode and vhdl can never disagree about what a signal is called. */
static const char *lit_name(Generator *g, int net_id) {
    static char buf[64];
    /* The upper bound matters as much as the lower one: an always-on OE and
     * similar sentinels come through as ids outside the net table, and
     * indexing rev[] with one of those reads past the end. */
    if (net_id < 0 || net_id >= MAX_NET) return NULL;

    /* A macrocell feedback is named FB<n>MC<m>, matching xc9500dump. The rev
     * table generator.c fills carries fb and mc already 1-based. */
    RevEntry *r = &g->rev[net_id];
    if (r->kind == REV_XOR) {
        snprintf(buf, sizeof(buf), "FB%dMC%d", r->fb, r->idx);
        return buf;
    }
    if (r->kind == REV_REG) {
        snprintf(buf, sizeof(buf), "FB%dMC%d_Q", r->fb, r->idx);
        return buf;
    }

    const char *n = gen_leaf_name(g, net_id);
    if (!n) return NULL;
    if (n[0] == '\'') return NULL;   /* a VHDL constant, not a literal */
    return n;
}

static int find_andterm_for_net(Generator *g, int net_id, int *out_fb, int *out_idx) {
    Model *m = g->m;
    if (net_id < 0 || net_id >= MAX_NET) return 0;
    for (int fb = 1; fb <= MAX_FB; fb++)
        for (int i = 0; i < MAX_PTERM; i++) {
            AndTerm *a = &m->andterms[fb][i];
            if (a->valid && a->out == net_id) { *out_fb = fb; *out_idx = i; return 1; }
        }
    return 0;
}

static void print_andterm(Generator *g, int fb, int idx, FILE *out) {
    AndTerm *a = &g->m->andterms[fb][idx];
    int first = 1;
    for (int i = 0; i < a->n_true; i++) {
        const char *n = lit_name(g, a->true_in[i]);
        if (!n) continue;
        fprintf(out, "%s%s", first ? "" : " AND ", n); first = 0;
    }
    for (int i = 0; i < a->n_comp; i++) {
        const char *n = lit_name(g, a->comp_in[i]);
        if (!n) continue;
        fprintf(out, "%sNOT_%s", first ? "" : " AND ", n); first = 0;
    }
    if (first) fprintf(out, "(empty)");
}

/* `used` -- which macrocells the design actually uses, per function block.
 * Modelled on xc7300dump's `used`. Reports from the netlist, so it answers
 * "what does this design DO", not "what bits are set". */
void emit_used(Generator *g, FILE *out) {
    Model *m = g->m;
    int total = 0, inuse = 0, blocks = 0;
    fprintf(out, "macrocells IN USE\n\n");
    for (int fb = 1; fb <= MAX_FB; fb++) {
        int printed = 0;
        for (int mc = 1; mc <= MAX_MC; mc++) {
            Iob *b = &m->iob[fb][mc];
            if (!b->valid) continue;
            total++;
            int is_comb = g->needed_comb[fb][mc];
            int is_reg  = g->needed_reg[fb][mc];
            int is_out  = b->has_output;
            int is_in   = g->used_input_pad[fb][mc];
            if (!is_comb && !is_reg && !is_out && !is_in) continue;
            if (!printed) { fprintf(out, "  --- FB%d ---\n", fb); printed = 1; blocks++; }
            char kinds[40]; kinds[0] = 0;
            if (is_reg)  strcat(kinds, "reg,");
            if (is_comb) strcat(kinds, "comb,");
            if (is_out)  strcat(kinds, "out,");
            if (is_in)   strcat(kinds, "in,");
            size_t kl = strlen(kinds);
            if (kl && kinds[kl - 1] == ',') kinds[kl - 1] = 0;
            const char *pin = NULL;
            pin_table_lookup_str(m->part_name, m->part_pkg, fb, mc, &pin);
            char pinbuf[24];
            if (pin) snprintf(pinbuf, sizeof(pinbuf), "pin%s", pin);
            else     snprintf(pinbuf, sizeof(pinbuf), "pin-");
            fprintf(out, "    FB%d_MC%-3d %-8s %-14s %s\n", fb, mc, pinbuf, kinds,
                    g->pad_name[fb][mc] ? g->pad_name[fb][mc] : "");
            inuse++;
        }
    }
    fprintf(out, "\n%d macrocell(s) in use across %d function block(s).\n",
            inuse, blocks);
    fprintf(out, "%d of %d macrocells configured but unused are not listed.\n",
            total - inuse, total);
}

void emit_decode(Generator *g, const char *jed_filename, FILE *out) {
    Model *m = g->m;

    fprintf(out, "device: %s-%s  (%s-%s)\n",
            m->part_name, m->part_pkg, m->part_name, m->part_pkg);
    if (jed_filename) fprintf(out, "source: %s\n", jed_filename);

    fprintf(out, "globals:");
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    static const char *gnames[8] = {"gck_0","gck_1","gck_2","gts_0","gts_1","gts_2","gts_3","gsr"};
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    static const char *glabel[8] = {"GCK0","GCK1","GCK2","GTS0","GTS1","GTS2","GTS3","GSR"};
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    for (int i = 0; i < 8; i++)
        fprintf(out, " %s=%s", glabel[i], g->globals_used[i] ? "used" : "-");
    (void)gnames;
    fprintf(out, "\n\n");

    for (int fb = 1; fb <= MAX_FB; fb++) {
        int any = 0;
        /* §207: a BURIED macrocell (no pad, so no Iob) is listed too when the
         * design uses its logic or register -- it was never reported. */
        for (int mc = 1; mc <= MAX_MC; mc++)
            if (m->iob[fb][mc].valid || g->needed_comb[fb][mc] || g->needed_reg[fb][mc]) { any = 1; break; }
        if (!any) continue;
        fprintf(out, "--- FB%d ---\n", fb);

        for (int mc = 1; mc <= MAX_MC; mc++) {
            Iob *b = &m->iob[fb][mc];
            int is_comb = g->needed_comb[fb][mc];
            /* Only a macrocell the generator resolved as a register IS one.
             * m->regs carries entries for cells the design never clocks;
             * reporting those as flip-flops gives a combinational node state
             * in any downstream simulation. */
            int is_reg  = g->needed_reg[fb][mc];
            if (!b->valid && !is_comb && !is_reg) continue;
            if (!is_comb && !is_reg && !b->has_output && !g->used_input_pad[fb][mc])
                continue;

            const char *pin_s = NULL;
            const char *sig = g->pad_name[fb][mc] ? g->pad_name[fb][mc] : "";
            if (!b->valid)
                fprintf(out, "[FB%d_MC%d buried]\n", fb, mc);
            else if (pin_table_lookup_str(m->part_name, m->part_pkg, fb, mc, &pin_s))
                fprintf(out, "[FB%d_MC%d pin%-4s %s]\n", fb, mc, pin_s, sig);
            else
                fprintf(out, "[FB%d_MC%d pin-    %s]\n", fb, mc, sig);

            Reg *r = is_reg ? &m->regs[fb][mc] : NULL;
            /* '-' not "RST": the macrocell's register is unused, so its
             * power-up state means nothing, and printing one would read as a
             * decoded value. */
            const char *clk_s = "COMB", *edge = "RISE", *ff = "C", *init = "-";
            int clk_fb = 0, clk_idx = 0, clk_is_pt = 0;

            if (r) {
                ff = r->is_tff ? "T" : (r->is_latch ? "L" : "D");
                init = r->init ? "SET" : "RST";
                edge = r->is_dualedge ? "BOTH" : (r->invert_clk ? "FALL" : "RISE");
                if (r->is_latch) clk_s = "LATCH";
                else if (find_andterm_for_net(g, r->clk, &clk_fb, &clk_idx)) {
                    /* Product-term clock. A cycle-based checker is not valid
                     * on these -- it clocks every register every cycle --
                     * so name it PT, which says plainly that a cycle-based
                     * comparison does not apply here. */
                    clk_s = "PT"; clk_is_pt = 1;
                } else {
                    const char *n = lit_name(g, r->clk);
                    clk_s = n ? n : "COMB";
                }
            }

            const char *oe_s;
            /* NET_ONE is a constant-1 output enable -- permanently driven,
             * not a product-term OE -- so it has to be tested for by name.
             * A test for a negative id alone calls such a pad PT-OE. */
            if (!b->valid)                               oe_s = "-(buried)";
            else if (!b->has_output)                     oe_s = "0(input-only)";
            else if (b->oe < 0 || b->oe == NET_ONE ||
                     b->oe == NET_NONE)                  oe_s = "1(always-on)";
            else                                         oe_s = "3(PT-OE)";

            fprintf(out, "  clk=%-6s %s | ff=%s init=%s | oe=%s\n",
                    clk_s, edge, ff, init, oe_s);

            fprintf(out, "  dev:");
            if (is_comb) fprintf(out, " XORINV=%d", m->xorgates[fb][mc].invert ? 1 : 0);
            if (r) fprintf(out, " CELL=%s CE=%d", r->celltype, r->has_ce ? 1 : 0);
            if (!is_comb && !r) fprintf(out, " (none)");
            fprintf(out, "\n");

            /* D/T input product terms */
            if (is_comb || is_reg) {
                OrTerm *o = &m->orterms[fb][mc];
                if (o->valid && o->n_in > 0) {
                    int n = 0;
                    for (int k = 0; k < o->n_in; k++) {
                        int afb = 0, aidx = 0;
                        if (find_andterm_for_net(g, o->in[k], &afb, &aidx)) n++;
                    }
                    if (n > 0) {
                        fprintf(out, "  D/T input (%d PT%s):\n", n, n == 1 ? "" : "s");
                        int seq = 1;
                        for (int k = 0; k < o->n_in; k++) {
                            int afb = 0, aidx = 0;
                            if (!find_andterm_for_net(g, o->in[k], &afb, &aidx)) continue;
                            fprintf(out, "    PT%d: ", seq++);
                            print_andterm(g, afb, aidx, out);
                            fprintf(out, "\n");
                        }
                    }
                }
            }

            if (clk_is_pt) {
                fprintf(out, "  PT-CLK clock term:\n    ");
                print_andterm(g, clk_fb, clk_idx, out);
                fprintf(out, "\n");
            }

            if (r) {
                /* PROVENANCE: DEFINED report labels */
                struct { const char *label; int net; } ctl[3] = {
                    { "ASYNC_RESET",  r->clr },
                    { "ASYNC_PRESET", r->pre },
                    { "CLK_ENABLE",   r->has_ce ? r->ce : -1 },
                };
                /* PROVENANCE: DEFINED report labels */
                for (int c = 0; c < 3; c++) {
                    if (ctl[c].net < 0) continue;
                    int afb = 0, aidx = 0;
                    if (find_andterm_for_net(g, ctl[c].net, &afb, &aidx)) {
                        fprintf(out, "  %s:\n    ", ctl[c].label);
                        print_andterm(g, afb, aidx, out);
                        fprintf(out, "\n");
                    } else {
                        const char *n = lit_name(g, ctl[c].net);
                        if (n) fprintf(out, "  %s:\n    %s\n", ctl[c].label, n);
                    }
                }
            }

            if (b->has_output && b->oe >= 0 &&
                b->oe != NET_ONE && b->oe != NET_ZERO && b->oe != NET_NONE) {
                int afb = 0, aidx = 0;
                if (find_andterm_for_net(g, b->oe, &afb, &aidx)) {
                    fprintf(out, "  OE condition:\n    ");
                    print_andterm(g, afb, aidx, out);
                    fprintf(out, "\n");
                } else {
                    const char *n = lit_name(g, b->oe);
                    if (n) fprintf(out, "  OE condition:\n    %s\n", n);
                }
            }
        }
    }
}

/* ---- verify: does this UCF belong to this bitstream? -------------------
 *
 * A bitstream carries no signal names, so a UCF can only be checked against
 * it pin by pin. Three things contradict a pairing and fail it:
 *
 *   - a pin the bitstream uses that the UCF does not name at all;
 *   - a UCF signal placed on a location this part does not have;
 *   - a conflict: one pin named by two signals, or one signal placed twice.
 *
 * A UCF signal on a pin the bitstream leaves unused is REPORTED but does not
 * fail: the fitter removes inputs the source declared but never read, and a
 * board's own UCF still names them. An exact repeat of an earlier line (same
 * signal, same pin) is not a conflict either; it is ignored and counted.
 *
 * Passing is a necessary condition, not proof of origin. A design that uses
 * nearly every pin cannot be told apart, by pins alone, from another design
 * with the same footprint; the output says so.
 */

static void vfy_name(const UcfEntry *e, char *out, size_t n) {
    if (e->has_index) snprintf(out, n, "%s(%d)", e->name, e->index);
    else              snprintf(out, n, "%s", e->name);
}

int emit_verify(Generator *g, const char *ucf_path, FILE *out) {
    Model *m = g->m;
    UcfFile *uf = ucf_parse(ucf_path);
    if (!uf) { fprintf(stderr, "error: cannot read %s\n", ucf_path); return 2; }

    fprintf(out, "device: %s-%s\n", m->part_name, m->part_pkg);
    fprintf(out, "UCF signals: %d\n\n", (int)uf->count);

    if (!g->have_pin_table) {
        fprintf(out, "No pin table for this device/package, so UCF pins cannot be\n");
        fprintf(out, "mapped to macrocells. Nothing to cross-check.\n");
        return 2;
    }

    /* Every pad's pin, and whether this part is wire-bond (all pins numeric). */
    static const char *pad_pin[MAX_FB + 1][MAX_MC + 1];
    /* PROVENANCE: DEFINED pin text buffer */
    static char pad_canon[MAX_FB + 1][MAX_MC + 1][16];
    /* The same comparison --ucf uses to name ports, so the two cannot disagree. */
    int wire_bond = pin_table_wire_bond(m->part_name, m->part_pkg);
    for (int fb = 0; fb <= MAX_FB; fb++)
        for (int mc = 0; mc <= MAX_MC; mc++) {
            pad_pin[fb][mc] = NULL;
            const char *s = NULL;
            if (fb >= 1 && mc >= 1 &&
                pin_table_lookup_str(m->part_name, m->part_pkg, fb, mc, &s)) {
                pad_pin[fb][mc] = s;
                pin_canon(s, wire_bond, pad_canon[fb][mc], sizeof(pad_canon[0][0]));
            }
        }

    /* Which pads the BITSTREAM uses, decided from the bitstream alone. */
    static int used[MAX_FB + 1][MAX_MC + 1], named[MAX_FB + 1][MAX_MC + 1];
    int n_used = 0;
    for (int fb = 1; fb <= MAX_FB; fb++)
        for (int mc = 1; mc <= MAX_MC; mc++) {
            Iob *b = &m->iob[fb][mc];
            used[fb][mc] = pad_pin[fb][mc] && b->valid &&
                           (b->has_output || g->used_input_pad[fb][mc]);
            named[fb][mc] = 0;
            n_used += used[fb][mc];
        }

    int *at_fb = calloc(uf->count ? uf->count : 1, sizeof(int));
    int *at_mc = calloc(uf->count ? uf->count : 1, sizeof(int));
    int on_used = 0, on_unused = 0, off_part = 0, conflicts = 0, not_named = 0, dups = 0;

    fprintf(out, "%-26s %-6s %-12s %s\n", "signal", "pin", "location", "in bitstream?");
    for (int i = 0; i < uf->count; i++) {
        const UcfEntry *e = &uf->entries[i];
        char nm[300], want[16];
        vfy_name(e, nm, sizeof(nm));
        pin_canon(e->loc, wire_bond, want, sizeof(want));
        int dup = 0;
        for (int j = 0; j < i && !dup; j++) {
            const UcfEntry *p = &uf->entries[j];
            char prev[16];
            pin_canon(p->loc, wire_bond, prev, sizeof(prev));
            dup = !strcmp(p->name, e->name) && p->has_index == e->has_index &&
                  (!e->has_index || p->index == e->index) && !strcmp(prev, want);
        }
        if (dup) {
            fprintf(out, "%-26s %-6s %-12s duplicate of an earlier line, ignored\n", nm, e->loc, "-");
            dups++;
            continue;
        }
        int ffb = 0, fmc = 0;
        for (int fb = 1; fb <= MAX_FB && !ffb; fb++)
            for (int mc = 1; mc <= MAX_MC && !ffb; mc++)
                if (pad_pin[fb][mc] && strcmp(pad_canon[fb][mc], want) == 0) { ffb = fb; fmc = mc; }
        if (!ffb) {
            fprintf(out, "%-26s %-6s %-12s pin not on this package\n", nm, e->loc, "-");
            off_part++;
            continue;
        }
        char loc[32]; snprintf(loc, sizeof(loc), "FB%d_MC%d", ffb, fmc);
        Iob *b = &m->iob[ffb][fmc];
        const char *state;
        if (!b->valid) { state = "macrocell not present"; off_part++; }
        else {
            at_fb[i] = ffb; at_mc[i] = fmc;
            named[ffb][fmc]++;
            if (b->has_output)                    { state = "drives output"; on_used++; }
            else if (g->used_input_pad[ffb][fmc]) { state = "read as input"; on_used++; }
            else                                  { state = "unused in this bitstream"; on_unused++; }
        }
        fprintf(out, "%-26s %-6s %-12s %s\n", nm, e->loc, loc, state);
    }

    for (int fb = 1; fb <= MAX_FB; fb++)
        for (int mc = 1; mc <= MAX_MC; mc++)
            if (named[fb][mc] > 1) {
                if (!conflicts) fprintf(out, "\nConflicting assignments:\n");
                fprintf(out, "  pin %s (FB%d_MC%d) is named by %d UCF signals\n",
                        pad_pin[fb][mc], fb, mc, named[fb][mc]);
                conflicts++;
            }
    for (int i = 0; i < uf->count; i++)
        for (int j = i + 1; j < uf->count; j++) {
            const UcfEntry *a = &uf->entries[i], *b = &uf->entries[j];
            if (!at_fb[i] || !at_fb[j]) continue;
            if (strcmp(a->name, b->name) || a->has_index != b->has_index ||
                (a->has_index && a->index != b->index)) continue;
            if (at_fb[i] == at_fb[j] && at_mc[i] == at_mc[j]) continue;
            char nm[300]; vfy_name(a, nm, sizeof(nm));
            if (!conflicts) fprintf(out, "\nConflicting assignments:\n");
            fprintf(out, "  %s is placed at both %s and %s\n", nm, a->loc, b->loc);
            conflicts++;
        }

    for (int fb = 1; fb <= MAX_FB; fb++)
        for (int mc = 1; mc <= MAX_MC; mc++)
            if (used[fb][mc] && !named[fb][mc]) {
                if (!not_named) fprintf(out, "\nPins this bitstream uses that the UCF does not name:\n");
                fprintf(out, "  pin %-6s FB%d_MC%-3d %s\n", pad_pin[fb][mc], fb, mc,
                        m->iob[fb][mc].has_output ? "drives output" : "read as input");
                not_named++;
            }
    free(at_fb); free(at_mc);

    fprintf(out, "\nPins the bitstream uses : %d, named by this UCF %d, NOT named %d\n",
            n_used, n_used - not_named, not_named);
    fprintf(out, "UCF signals             : %d on used pins, %d on pins this bitstream leaves unused,\n"
                 "                          %d on locations this part does not have\n",
            on_used, on_unused, off_part);
    if (conflicts) fprintf(out, "Conflicting assignments : %d\n", conflicts);
    if (dups) fprintf(out, "%d duplicate constraint(s) ignored (same signal, same pin)\n", dups);

    if (not_named || off_part || conflicts) {
        fprintf(out, "\nRESULT: this UCF does NOT match this bitstream.\n");
        return 1;
    }
    fprintf(out, "\nRESULT: consistent. Every pin this bitstream uses is named in this UCF,\n"
                 "        at that location.\n");
    if (on_unused)
        fprintf(out, "        %d UCF signal(s) sit on pins the bitstream leaves unused: normal\n"
                     "        when the fitter removed an input the source declared but never read.\n",
                on_unused);
    fprintf(out, "        This is a necessary condition, not proof that the UCF was written for\n"
                 "        this design: a UCF from another design that uses the same pins passes\n"
                 "        too, and on a board that uses nearly every pin that is likely.\n");
    return 0;
}
