#include <ctype.h>
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "jed2vhdl.h"

static int is_const(const char *s) { return strcmp(s, "'0'") == 0 || strcmp(s, "'1'") == 0; }

static void emit_comb(Generator *g, FILE *out) {
    Model *m = g->m;
    int any = 0;
    for (int fb = 1; fb <= MAX_FB && !any; fb++)
        for (int mc = 1; mc <= MAX_MC; mc++) if (g->needed_comb[fb][mc]) { any = 1; break; }
    if (!any) return;
    fprintf(out, "  -- Combinational macrocell equations (OR-of-products, with XOR/invert)\n");
    for (int fb = 1; fb <= MAX_FB; fb++)
        for (int mc = 1; mc <= MAX_MC; mc++) {
            if (!g->needed_comb[fb][mc]) continue;
            XorGate *x = &m->xorgates[fb][mc];
            char *or_expr = gen_expr(g, x->orterm);
            char *ptc_expr = gen_expr(g, x->ptc);
            char *body;
            /* Fold constant XOR operands. `'0' xor E` is E and
             * `'1' xor E` is not E -- but only for 0/1: in std_logic
             * `'0' xor 'Z'` is 'X', so the unfolded form turns a floating pad
             * read back through a macrocell into 'X'. Comparing this tool's
             * output against itself cannot show that up; only simulating a
             * bidirectional pad can. */
            int invert = x->invert;
            const char *keep[2]; int nkeep = 0;
            const char *cand[2]; cand[0] = or_expr; cand[1] = ptc_expr;
            for (int ci = 0; ci < 2; ci++) {
                if (strcmp(cand[ci], "'0'") == 0) continue;
                if (strcmp(cand[ci], "'1'") == 0) { invert = !invert; continue; }
                keep[nkeep++] = cand[ci];
            }
            if (nkeep == 0) {
                body = strdup("'0'");
            } else if (nkeep == 1) {
                body = strdup(keep[0]);
            } else {
                int a_par = strchr(keep[0], ' ') != NULL && keep[0][0] != '\'';
                int b_par = strchr(keep[1], ' ') != NULL && keep[1][0] != '\'';
                body = xasprintf("%s%s%s xor %s%s%s",
                                  a_par ? "(" : "", keep[0], a_par ? ")" : "",
                                  b_par ? "(" : "", keep[1], b_par ? ")" : "");
            }
            if (invert) {
                char *wrapped = xasprintf("not (%s)", body);
                free(body);
                body = wrapped;
            }
            /* The global-net code below needs to know whether a macrocell
             * folds to a LITERAL, and the fold happens right here, so record
             * it rather than work it out a second time. */
            g->comb_is_const[fb][mc] =
                (strcmp(body, "'0'") == 0 || strcmp(body, "'1'") == 0);
            char *sig = gen_comb_sig(fb, mc);
            fprintf(out, "  %s <= %s;\n", sig, body);
            free(sig); free(or_expr); free(ptc_expr); free(body);
        }
    fprintf(out, "\n");
}

static const char *edge_cond(char *buf, size_t bufsz, const char *clk_sig, int invert, int dualedge) {
    if (dualedge) snprintf(buf, bufsz, "%s'event", clk_sig);
    else snprintf(buf, bufsz, invert ? "falling_edge(%s)" : "rising_edge(%s)", clk_sig);
    return buf;
}

/* Emits an if/elsif chain for the async set/reset conditions, with the main
 * clocked or level-sensitive body as the last branch of that same chain. */
static void emit_priority_if(FILE *out, const char *reg_sig, const char *pre_sig,
                              const char *clr_sig, void (*emit_main)(FILE *, void *), void *ctx) {
    int pre_always = strcmp(pre_sig, "'1'") == 0;
    int pre_active = !is_const(pre_sig);
    int clr_always = strcmp(clr_sig, "'1'") == 0;
    int clr_active = !is_const(clr_sig);

    if (pre_always) {
        fprintf(out, "    %s <= '1';\n", reg_sig);
        return;
    }

    int chain_opened = 0;
    const char *kw = "if";
    if (pre_active) {
        fprintf(out, "    %s %s = '1' then\n", kw, pre_sig);
        fprintf(out, "      %s <= '1';\n", reg_sig);
        kw = "elsif";
        chain_opened = 1;
    }
    if (clr_always) {
        fprintf(out, "    %s\n", chain_opened ? "else" : "if true then");
        fprintf(out, "      %s <= '0';\n", reg_sig);
        fprintf(out, "    end if;\n");
        return;
    }
    if (clr_active) {
        fprintf(out, "    %s %s = '1' then\n", kw, clr_sig);
        fprintf(out, "      %s <= '0';\n", reg_sig);
        kw = "elsif";
        chain_opened = 1;
    }
    /* Final clause: either "elsif <edge> then ... end if;" (closing the
     * chain opened above) or, if no pre/clr clause exists at all, just
     * the bare "if <edge> then ... end if;" that emit_main already
     * closes on its own - no extra wrapping "end if;" in that case. */
    fprintf(out, "    %s ", chain_opened ? "elsif" : "if");
    emit_main(out, ctx);
    /* emit_main always closes its own `if`/`elsif` with one `end if;`, and an
     * `elsif` continues the SAME if-statement rather than opening a new one --
     * so the chain needs no second `end if;`. Emitting one here produced
     * unparseable VHDL ("end" must be followed by 'process') for every register
     * with an async set/reset. */
}

typedef struct { Generator *g; Reg *r; const char *reg_sig; char *d_expr; char *clk_sig; char *ce_sig; } MainCtx;

static void emit_main_body(FILE *out, void *vctx) {
    MainCtx *ctx = vctx;
    Reg *r = ctx->r;
    if (r->is_latch) {
        char gate_test[128];
        if (r->invert_clk) snprintf(gate_test, sizeof(gate_test), "not %s", ctx->clk_sig);
        else snprintf(gate_test, sizeof(gate_test), "%s", ctx->clk_sig);
        fprintf(out, "%s = '1' then\n", gate_test);
        fprintf(out, "      %s <= %s;\n", ctx->reg_sig, ctx->d_expr);
        fprintf(out, "    end if;\n");
    } else {
        char edgebuf[160];
        edge_cond(edgebuf, sizeof(edgebuf), ctx->clk_sig, r->invert_clk, r->is_dualedge);
        fprintf(out, "%s then\n", edgebuf);
        if (ctx->ce_sig) {
            fprintf(out, "      if %s = '1' then\n", ctx->ce_sig);
            if (r->is_tff) fprintf(out, "        %s <= (%s) xor %s;\n", ctx->reg_sig, ctx->d_expr, ctx->reg_sig);
            else fprintf(out, "        %s <= %s;\n", ctx->reg_sig, ctx->d_expr);
            fprintf(out, "      end if;\n");
        } else {
            if (r->is_tff) fprintf(out, "      %s <= (%s) xor %s;\n", ctx->reg_sig, ctx->d_expr, ctx->reg_sig);
            else fprintf(out, "      %s <= %s;\n", ctx->reg_sig, ctx->d_expr);
        }
        fprintf(out, "    end if;\n");
    }
}

static void emit_registers(Generator *g, FILE *out) {
    Model *m = g->m;
    int any = 0;
    for (int fb = 1; fb <= MAX_FB && !any; fb++)
        for (int mc = 1; mc <= MAX_MC; mc++) if (g->needed_reg[fb][mc]) { any = 1; break; }
    if (!any) return;
    fprintf(out, "  -- Registers (D/T flip-flops or latches, as configured per macrocell)\n");

    for (int fb = 1; fb <= MAX_FB; fb++)
        for (int mc = 1; mc <= MAX_MC; mc++) {
            if (!g->needed_reg[fb][mc]) continue;
            Reg *r = &m->regs[fb][mc];
            char *reg_sig = gen_reg_sig(fb, mc);
            char *d_expr = gen_expr(g, r->d);
            char *clk_sig = gen_control_sig(g, "clk", fb, mc, r->clk);
            char *pre_sig = gen_control_sig(g, "pre", fb, mc, r->pre);
            char *clr_sig = gen_control_sig(g, "clr", fb, mc, r->clr);
            char *ce_sig = (r->ce != NET_NONE) ? gen_control_sig(g, "ce", fb, mc, r->ce) : NULL;

            fprintf(out, "  -- FB%d macrocell %d  (%s)\n", fb, mc, r->celltype);
            if (r->is_dualedge)
                fprintf(out, "  -- NOTE: DualEDGE register approximated as \"any edge triggers\" below.\n");

            /* sensitivity list: unique non-constant signals */
            const char *sens[8]; int nsens = 0;
            /* PROVENANCE: DEFINED sensitivity-list candidates */
            const char *cands[4] = { clk_sig, pre_sig, clr_sig, r->is_latch ? d_expr : NULL };
            /* PROVENANCE: DEFINED sensitivity-list candidates */
            for (int i = 0; i < 4; i++) {
                if (!cands[i] || is_const(cands[i])) continue;
                int dup = 0;
                for (int k = 0; k < nsens; k++) if (strcmp(sens[k], cands[i]) == 0) dup = 1;
                if (!dup) sens[nsens++] = cands[i];
            }
            fprintf(out, "  process(");
            for (int i = 0; i < nsens; i++) fprintf(out, "%s%s", i ? ", " : "", sens[i]);
            fprintf(out, ")\n  begin\n");

            MainCtx ctx = { g, r, reg_sig, d_expr, clk_sig, ce_sig };
            emit_priority_if(out, reg_sig, pre_sig, clr_sig, emit_main_body, &ctx);

            fprintf(out, "  end process;\n\n");

            free(reg_sig); free(d_expr); free(clk_sig); free(pre_sig); free(clr_sig);
            if (ce_sig) free(ce_sig);
        }
}

static void emit_outputs(Generator *g, FILE *out) {
    Model *m = g->m;
    fprintf(out, "  -- Output pin drivers\n");
    for (int fb = 1; fb <= MAX_FB; fb++)
        for (int mc = 1; mc <= MAX_MC; mc++) {
            Iob *b = &m->iob[fb][mc];
            if (!b->valid || !b->has_output) continue;
            const char *port = g->pad_name[fb][mc];
            char *drive_expr = gen_expr(g, b->drive);

            /* OPEN-DRAIN POLARITY. An open-drain cell holds I = '0' and puts
             * the macrocell output F on E, and the cell drives I when E = '1'
             * -- so taking the generic path would pull low when F = 1. The pad
             * realises F by pulling LOW when F = 0. Checked against the
             * fitter's own equation for a hand-written open-drain design. */
            int od = (strcmp(b->obuf_mode, "open-drain") == 0);
            /* CGND is a hard ground TIE, not a driver. VHDL cannot say
             * that, so it decodes to constant '0' -- right at the pin, but it
             * rebuilds as push-pull. Mark it rather than let it be silent. */
            const char *tag = (strcmp(b->obuf_mode, "CGND") == 0)
                ? "  -- CGND: pad tied to ground in the original, not driven" : "";
            if (b->oe == NET_NONE) {
                fprintf(out, "  %s <= %s;%s\n", port, drive_expr, tag);
            } else {
                char *oe_sig = gen_control_sig(g, "oe", fb, mc, b->oe);
                const char *active = od ? "'0'" : "'1'";
                if (strcmp(oe_sig, "'1'") == 0) {
                    if (od) fprintf(out, "  %s <= 'Z';  -- open drain, function always 1\n", port);
                    else    fprintf(out, "  %s <= %s;%s\n", port, drive_expr, tag);
                } else if (strcmp(oe_sig, "'0'") == 0) {
                    if (od) fprintf(out, "  %s <= '0';  -- open drain, function always 0\n", port);
                    else    fprintf(out, "  %s <= 'Z';  -- output permanently disabled\n", port);
                } else {
                    fprintf(out, "  %s <= %s when %s = %s else 'Z';\n",
                            port, drive_expr, oe_sig, active);
                }
                free(oe_sig);
            }
            free(drive_expr);
        }
    fprintf(out, "\n");
}

/* Constant a global net resolves to when its buffer input is tied off
 * (global disabled), already accounting for the buffer's INVERT.
 * Returns 0 when the global is genuinely pad-driven. */
/* Is `w` in `text` as a whole identifier? */
static int has_word(const char *text, const char *w) {
    size_t n = strlen(w);
    for (const char *p = strstr(text, w); p; p = strstr(p + 1, w)) {
        int l = p == text || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        int r = !(isalnum((unsigned char)p[n]) || p[n] == '_');
        if (l && r) return 1;
    }
    return 0;
}

static char global_const_value(Generator *g, int idx) {
    int gnet;
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    if (idx < 3)      gnet = g->m->gck[idx];
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    else if (idx < 7) gnet = g->m->gts[idx - 3];
    else              gnet = g->m->gsr;
    if (gnet < 0 || gnet >= MAX_NET) return 0;
    char c = g->m->global_const[gnet];
    if (!c) return 0;
    /* GTS/GSR use OE = INVERT ? pad : not pad. BUFG (clock, idx<3)
     * exports no INVERT and is a plain pass-through. */
    /* PROVENANCE: MEASURED pad-driven GTS/GSR polarity, INVERT=1 is active high: §37 (gtshi, gtslo, srhi, srlo) */
    if (idx >= 3 && !g->m->global_invert[gnet]) c = (c == '1') ? '0' : '1';
    return c;
}

int emit_vhdl(Generator *g, const char *jed_filename, FILE *out) {
    Model *m = g->m;

    /* ---- Build body sections FIRST (into a memory buffer): this is
     * what populates control_sigs / globals_used as a side effect ---- */
    char *body_buf = NULL;
    size_t body_size = 0;
    FILE *body = xc2c_open_memstream(&body_buf, &body_size);
    if (!body) {
        fprintf(stderr, "error: cannot open a working buffer for the VHDL body\n");
        return -1;
    }
    emit_comb(g, body);
    emit_registers(g, body);
    emit_outputs(g, body);
    xc2c_close_memstream(body);



    /* ---- Global buffer nets, now that we know which were used ---- */
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    const char *global_names[8] = { "gck_0", "gck_1", "gck_2", "gts_0", "gts_1", "gts_2", "gts_3", "gsr" };

    /* globals_used over-counts: it is set whenever a global net is RESOLVED,
     * so one referenced only by logic that dead-code elimination later pruned
     * stays in the set. Trust the emitted body text instead: a global is
     * live only if its name actually appears there.
     * Then promote its driver pad to a port, or the architecture references
     * an undeclared signal. Ports must therefore be built AFTER the body. */
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    int live_global[8];
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    /* §208: the name as emitted (it gives way to a clashing UCF net), found
     * as a whole word, as the Python implementation finds it. */
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    char *gv[8];
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    for (int i = 0; i < 8; i++)   /* allocated only if used, as in Python */
        gv[i] = g->globals_used[i] ? gen_ident(global_names[i]) : strdup(global_names[i]);
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    for (int i = 0; i < 8; i++) {
        live_global[i] = g->globals_used[i] && body_buf && has_word(body_buf, gv[i]);
        if (!live_global[i]) continue;
        /* A disabled global resolves to a constant, so its silicon pad must
         * NOT be promoted to a port: that invents an input pin the design
         * never had. */
        if (global_const_value(g, i)) continue;
        int fb = 0, mc = 0;
        if (gen_global_driver_loc(g, global_names[i], &fb, &mc) &&
            m->iob[fb][mc].valid)
            g->used_input_pad[fb][mc] = 1;
    }

    int nports;
    PortDecl *ports = generator_build_ports(g, &nports);
    char *global_decl = NULL, *global_drive = NULL;
    size_t gd_sz = 0, gv_sz = 0;
    FILE *gdecl_f = xc2c_open_memstream(&global_decl, &gd_sz);
    FILE *gdrive_f = gdecl_f ? xc2c_open_memstream(&global_drive, &gv_sz) : NULL;
    if (!gdecl_f || !gdrive_f) {
        if (gdecl_f) xc2c_close_memstream(gdecl_f);
        fprintf(stderr, "error: cannot open a working buffer for the global nets\n");
        free(global_decl); free(body_buf);
        return -1;
    }
    int any_global = 0;
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    for (int i = 0; i < 8; i++) {
        if (!live_global[i]) continue;
        any_global = 1;
        fprintf(gdecl_f, "  signal %s : std_logic;\n", gv[i]);
        const char *src = gen_global_driver_pad(g, global_names[i]);
        /* The global buffer itself can invert. */
        int gnet = -1;
        /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
        if (i < 3) gnet = g->m->gck[i];
        /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
        else if (i < 7) gnet = g->m->gts[i - 3];
        else gnet = g->m->gsr;
        int ginv = (gnet >= 0 && gnet < MAX_NET) ? g->m->global_invert[gnet] : 0;
        char gconst = global_const_value(g, i);
        /* A global tri-state can be driven by a BURIED MACROCELL sitting at
         * the same fixed FB/MC the dedicated pad occupies. The fuse block says
         * only that the buffer input is tied off, which reads as "no driver";
         * the fitter's own equation for such a design says
         *     MC_A17.OE = MC_OE_N_PAL_not0001_MC.GLB;
         * and the fitter is right, because it built the bitstream.
         *
         * Two conditions. Take this branch only when the net would otherwise
         * fold to a CONSTANT: an enabled, pad-driven global keeps the pad
         * branch below even if its location also carries buried logic. And
         * only when that macrocell is real logic, since one folding to '0' is
         * said more simply by the constant.
         *
         * POLARITY is DIRECT here -- the macrocell output as it stands, active
         * high -- even when the buffer's own invert bit is clear. The pad rule
         * further down does not carry over to a macrocell driver; taking the
         * inverted reading instead leaves thousands of pads floating or
         * driving against the original. */
        int mfb = 0, mmc = 0;
        const char *mcdrv = NULL;
        if (gconst && gen_global_driver_loc(g, global_names[i], &mfb, &mmc) &&
            mfb >= 1 && mfb <= MAX_FB && mmc >= 1 && mmc <= MAX_MC &&
            g->needed_comb[mfb][mmc] && !g->comb_is_const[mfb][mmc])
            mcdrv = gen_comb_sig(mfb, mmc);
        if (mcdrv) {
            fprintf(gdrive_f, "  %s <= %s;  -- driven by a buried macrocell\n",
                    gv[i], mcdrv);
            free((void *)mcdrv);
        }
        else if (gconst)
            fprintf(gdrive_f, "  %s <= '%c';  -- global disabled (buffer input tied off)\n",
                    gv[i], gconst);
        /* PAD POLARITY: INVERT=1 -> OE = pad, INVERT=0 -> OE = not pad.
         * Checked against the fitter's own equations. */
        else if (src) fprintf(gdrive_f, "  %s <= %s%s;\n", gv[i],
                         /* PROVENANCE: MEASURED pad-driven GTS/GSR polarity, INVERT=1 is active high: §37 (gtshi, gtslo, srhi, srlo) */
                         (i < 3) ? "" : (ginv ? "" : "not "), src);
        else fprintf(gdrive_f, "  %s <= '0';  -- source pad unknown for this device/package\n", gv[i]);
    }
    xc2c_close_memstream(gdecl_f); xc2c_close_memstream(gdrive_f);

    /* ---- header ---- */
    time_t now = time(NULL);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S", localtime(&now));

    fprintf(out, "--------------------------------------------------------------------------------\n");
    fprintf(out, "-- xc2cdump -- CoolRunner-II fuse map decompiler by Darksoft\n");
    fprintf(out, "-- Source JED : %s\n", jed_filename);
    fprintf(out, "-- Device     : %s  Package: %s  Speed: -%s\n", m->part_name, m->part_pkg, m->part_speed);
    fprintf(out, "-- Generated  : %s\n", timebuf);
    fprintf(out, "--\n");
    fprintf(out, "-- This file was reconstructed from the fuse map, not from the original RTL.\n");
    fprintf(out, "-- It is functionally equivalent (same product terms / OR terms / macrocell\n");
    fprintf(out, "-- configuration as programmed into the device) but variable names, control-\n");
    fprintf(out, "-- flow structure, and comments from the original design are NOT recoverable\n");
    fprintf(out, "-- and are not represented here.\n");
    gen_write_renames(out);   /* §208: one line per renamed identifier */
    fprintf(out, "--------------------------------------------------------------------------------\n");
    fprintf(out, "library ieee;\n");
    fprintf(out, "use ieee.std_logic_1164.all;\n\n");
    fprintf(out, "entity %s is\n  port(\n", g->entity_name);
    for (int i = 0; i < nports; i++) {
        if (ports[i].is_vector)
            fprintf(out, "    %s : %s std_logic_vector(%d downto %d)%s\n",
                    ports[i].name, ports[i].direction, ports[i].hi, ports[i].lo,
                    i == nports - 1 ? "" : ";");
        else
            fprintf(out, "    %s : %s std_logic%s\n",
                    ports[i].name, ports[i].direction, i == nports - 1 ? "" : ";");
    }
    fprintf(out, "  );\nend entity %s;\n\n", g->entity_name);
    fprintf(out, "architecture rtl of %s is\n\n", g->entity_name);

    if (any_global) fprintf(out, "%s", global_decl);
    for (int fb = 1; fb <= MAX_FB; fb++)
        for (int mc = 1; mc <= MAX_MC; mc++)
            if (g->needed_comb[fb][mc]) {
                char *sig = gen_comb_sig(fb, mc);
                fprintf(out, "  signal %s : std_logic;\n", sig);
                free(sig);
            }
    for (int fb = 1; fb <= MAX_FB; fb++)
        for (int mc = 1; mc <= MAX_MC; mc++)
            if (g->needed_reg[fb][mc]) {
                char *sig = gen_reg_sig(fb, mc);
                fprintf(out, "  signal %s : std_logic := '%d';\n", sig, m->regs[fb][mc].init ? 1 : 0);
                free(sig);
            }
    for (int i = 0; i < g->n_control_sigs; i++)
        fprintf(out, "  signal %s : std_logic;\n", g->control_sigs[i].name);

    fprintf(out, "\nbegin\n\n");

    if (any_global) {
        fprintf(out, "  -- Global buffer nets (GCK/GTS/GSR), driven from their fixed silicon pins\n");
        fprintf(out, "%s\n", global_drive);
    }
    if (g->n_control_sigs > 0) {
        fprintf(out, "  -- Shared / derived control nets (clock, set, reset, clock-enable, output-enable)\n");
        for (int i = 0; i < g->n_control_sigs; i++)
            fprintf(out, "  %s <= %s;\n", g->control_sigs[i].name, g->control_sigs[i].expr);
        fprintf(out, "\n");
    }

    fwrite(body_buf, 1, body_size, out);

    fprintf(out, "end architecture rtl;\n");

    free(body_buf);
    free(global_decl);
    free(global_drive);
    return 0;
}
