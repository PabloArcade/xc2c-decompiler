#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "jed2vhdl.h"


static char *xstrdup(const char *s) { return s ? strdup(s) : NULL; }

/* ------------------------------------------------------------------ */
/* Pad naming                                                          */
/* ------------------------------------------------------------------ */

static VectorDecl *find_or_add_vector(Generator *g, const char *base) {
    for (int i = 0; i < g->n_vectors; i++)
        if (strcmp(g->vectors[i].base, base) == 0) return &g->vectors[i];
    VectorDecl *v = &g->vectors[g->n_vectors++];
    v->base = xstrdup(base);
    v->max_index = -1;
    v->min_index = INT_MAX;
    return v;
}

/* ------------------------------------------------------------------ */
/* §208 (D208): identifier allocation                                  */
/* ------------------------------------------------------------------ */
/* Names are INHERITED (the board's UCF nets) or GENERATED (everything this
 * tool invents). Inherited names are fixed first and kept unless VHDL cannot
 * use them; every generated name is then allocated against all of them,
 * case-insensitively (VHDL folds case), and gives way: _i, _i2, _i3 ...
 * re-checked each time. One table per run: there is one generator. */
/* PROVENANCE: DEFINED IEEE 1076-2008 reserved words, plus the names this emitter references */
static const char *const VHDL_WORDS[] = {
    "abs","access","after","alias","all","and","architecture","array","assert","assume",
    "assume_guarantee","attribute","begin","block","body","buffer","bus","case","component",
    "configuration","constant","context","cover","default","disconnect","downto","else","elsif",
    "end","entity","exit","fairness","file","for","force","function","generate","generic","group",
    "guarded","if","impure","in","inertial","inout","is","label","library","linkage","literal",
    "loop","map","mod","nand","new","next","nor","not","null","of","on","open","or","others","out",
    "package","parameter","port","postponed","procedure","process","property","protected","pure",
    "range","record","register","reject","release","rem","report","restrict","restrict_guarantee",
    "return","rol","ror","select","sequence","severity","shared","signal","sla","sll","sra","srl",
    "strong","subtype","then","to","transport","type","unaffected","units","until","use",
    "variable","vmode","vprop","vunit","wait","when","while","with","xnor","xor", NULL };
/* PROVENANCE: DEFINED identifiers the emitted VHDL references by name */
static const char *const VHDL_USED[] = {
    "ieee","std","work","std_logic","std_logic_vector","std_logic_1164","rising_edge",
    "falling_edge","event","rtl", NULL };

typedef struct { char *key, *val; } IdEnt;
static char **id_taken; static int n_taken, cap_taken;
static IdEnt *id_map; static int n_map, cap_map;
static char **ren_from, **ren_to, **ren_why; static int n_ren, cap_ren;

static char *lower_dup(const char *s) {
    char *o = xstrdup(s);
    for (char *p = o; *p; p++) *p = (char)tolower((unsigned char)*p);
    return o;
}
static int is_taken(const char *s) {
    char *l = lower_dup(s);
    int hit = 0;
    for (int i = 0; i < n_taken && !hit; i++) hit = !strcmp(id_taken[i], l);
    free(l);
    return hit;
}
static void take(const char *s) {
    /* PROVENANCE: DEFINED growth size */
    if (n_taken == cap_taken) { cap_taken = cap_taken ? cap_taken * 2 : 256;
        id_taken = realloc(id_taken, sizeof(char *) * cap_taken); }
    id_taken[n_taken++] = lower_dup(s);
}
static int in_list(const char *s, const char *const *list) {
    char *l = lower_dup(s);
    int hit = 0;
    for (int i = 0; list[i] && !hit; i++) hit = !strcmp(list[i], l);
    free(l);
    return hit;
}
static void add_rename(const char *from, const char *to, char *why) {
    /* PROVENANCE: DEFINED growth size */
    if (n_ren == cap_ren) { cap_ren = cap_ren ? cap_ren * 2 : 16;
        ren_from = realloc(ren_from, sizeof(char *) * cap_ren);
        ren_to = realloc(ren_to, sizeof(char *) * cap_ren);
        ren_why = realloc(ren_why, sizeof(char *) * cap_ren); }
    ren_from[n_ren] = xstrdup(from); ren_to[n_ren] = xstrdup(to); ren_why[n_ren] = why; n_ren++;
}
static void ident_reset(void) {
    n_taken = n_map = n_ren = 0;
    for (int i = 0; VHDL_WORDS[i]; i++) take(VHDL_WORDS[i]);
    for (int i = 0; VHDL_USED[i]; i++) take(VHDL_USED[i]);
}

/* vhdl_ident, then the two rules it missed: no doubled underscore and no
 * trailing underscore in a basic identifier. */
static char *legal_ident(const char *name) {
    char *id = vhdl_ident(name);
    char *w = id;
    for (char *r = id; *r; r++) {
        if (*r == '_' && w > id && w[-1] == '_') continue;
        *w++ = *r;
    }
    *w = 0;
    while (strlen(id) > 1 && id[strlen(id) - 1] == '_') id[strlen(id) - 1] = 0;
    return id;
}

/* A GENERATED identifier, distinct from every inherited name, the reserved
 * words and every identifier already generated. Returns a new string. */
char *gen_ident(const char *raw) {
    char *key = lower_dup(raw);
    for (int i = 0; i < n_map; i++)
        if (!strcmp(id_map[i].key, key)) { free(key); return xstrdup(id_map[i].val); }
    char *cand = xstrdup(raw);
    for (int n = 2; is_taken(cand); n++) {
        free(cand);
        cand = n == 2 ? xasprintf("%s_i", raw) : xasprintf("%s_i%d", raw, n - 1);
    }
    take(cand);
    /* PROVENANCE: DEFINED growth size */
    if (n_map == cap_map) { cap_map = cap_map ? cap_map * 2 : 256;
        id_map = realloc(id_map, sizeof(IdEnt) * cap_map); }
    id_map[n_map].key = key; id_map[n_map].val = xstrdup(cand); n_map++;
    if (strcmp(cand, raw)) add_rename(raw, cand, xasprintf("generated name; %s is taken", raw));
    return cand;
}

static int ren_cmp(const void *a, const void *b) {
    int i = *(const int *)a, j = *(const int *)b;
    int c = strcmp(ren_from[i], ren_from[j]);
    if (c) return c;
    c = strcmp(ren_to[i], ren_to[j]);
    return c ? c : strcmp(ren_why[i], ren_why[j]);
}
/* The rename comment lines, sorted as the Python implementation sorts them. */
void gen_write_renames(FILE *out) {
    int *ix = malloc(sizeof(int) * (n_ren ? n_ren : 1));
    for (int i = 0; i < n_ren; i++) ix[i] = i;
    qsort(ix, n_ren, sizeof(int), ren_cmp);
    for (int k = 0; k < n_ren; k++)
        fprintf(out, "-- renamed: %s -> %s  (%s)\n", ren_from[ix[k]], ren_to[ix[k]], ren_why[ix[k]]);
    free(ix);
}

static int str_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
/* Inherited names: each distinct UCF net, in sorted order, made legal and
 * kept unless VHDL cannot use it; then _pin, _pin2 ... */
static char **inh_from, **inh_to; static int n_inh;
static void inherit_names(Generator *g) {
    n_inh = 0;
    if (!g->ucf) return;
    const char **names = malloc(sizeof(char *) * (g->ucf->count ? g->ucf->count : 1));
    int n = 0;
    for (int e = 0; e < g->ucf->count; e++) {
        int dup = 0;
        for (int k = 0; k < n && !dup; k++) dup = !strcmp(names[k], g->ucf->entries[e].name);
        if (!dup) names[n++] = g->ucf->entries[e].name;
    }
    qsort(names, n, sizeof(char *), str_cmp);
    inh_from = malloc(sizeof(char *) * (n ? n : 1)); inh_to = malloc(sizeof(char *) * (n ? n : 1));
    for (int k = 0; k < n; k++) {
        char *legal = legal_ident(names[k]);
        char *cand = xstrdup(legal);
        for (int i = 2; is_taken(cand); i++) {
            free(cand);
            cand = i == 2 ? xasprintf("%s_pin", legal) : xasprintf("%s_pin%d", legal, i - 1);
        }
        take(cand);
        inh_from[n_inh] = xstrdup(names[k]); inh_to[n_inh] = cand; n_inh++;
        if (strcmp(cand, names[k])) {
            const char *why = in_list(legal, VHDL_WORDS) ? "a VHDL reserved word"
                            : in_list(legal, VHDL_USED) ? "a name the VHDL itself uses"
                            : strcmp(cand, legal) ? "already taken by another UCF net"
                            : "not a legal VHDL identifier";
            add_rename(names[k], cand, xasprintf("UCF net: %s", why));
        }
        free(legal);
    }
    free(names);
}
static const char *inherited(const char *base) {
    for (int k = 0; k < n_inh; k++) if (!strcmp(inh_from[k], base)) return inh_to[k];
    return base;
}

static void build_pad_names(Generator *g) {
    Model *m = g->m;
    ident_reset();
    inherit_names(g);
    int wire_bond = g->have_pin_table && pin_table_wire_bond(m->part_name, m->part_pkg);
    for (int fb = 1; fb <= MAX_FB; fb++) {
        for (int mc = 1; mc <= MAX_MC; mc++) {
            if (!m->iob[fb][mc].valid) continue;
            const char *pin_s = NULL;
            /* Pins are strings: a BGA gives a ball ID ("D1", or "P12" in row
             * P), a wire-bond package a number. UCF entries are matched to
             * them through pin_canon, which strips a P prefix only on a
             * wire-bond part. */
            int have_pin = g->have_pin_table &&
                pin_table_lookup_str(m->part_name, m->part_pkg, fb, mc, &pin_s);
            char pad_c[16];
            if (have_pin) pin_canon(pin_s, wire_bond, pad_c, sizeof(pad_c));

            const char *base = NULL;
            int has_index = 0, index = 0;
            if (have_pin && g->ucf) {
                for (int e = 0; e < g->ucf->count && !base; e++) {
                    char ucf_c[16];
                    pin_canon(g->ucf->entries[e].loc, wire_bond, ucf_c, sizeof(ucf_c));
                    if (strcmp(ucf_c, pad_c) == 0) {
                        base = g->ucf->entries[e].name;
                        has_index = g->ucf->entries[e].has_index;
                        index = g->ucf->entries[e].index;
                    }
                }
            }

            if (base) {
                char *ident = xstrdup(inherited(base));
                g->named_from_ucf[fb][mc] = 1;
                if (has_index) {
                    VectorDecl *v = find_or_add_vector(g, ident);
                    if (index > v->max_index) v->max_index = index;
                    if (index < v->min_index) v->min_index = index;
                    g->pad_name[fb][mc] = xasprintf("%s(%d)", ident, index);
                    free(ident);
                } else {
                    g->pad_name[fb][mc] = ident;
                }
            } else if (have_pin) {
                char *raw = xasprintf("PIN_%s", pin_s);
                g->pad_name[fb][mc] = gen_ident(raw);
                free(raw);
            } else {
                char *raw = xasprintf("PAD_FB%d_%d", fb, mc);
                g->pad_name[fb][mc] = gen_ident(raw);
                free(raw);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Constructor + rev-map                                               */
/* ------------------------------------------------------------------ */

Generator *generator_new(Model *m, UcfFile *ucf, int use_pin_table, const char *entity_name) {
    Generator *g = calloc(1, sizeof(Generator));
    g->m = m;
    g->ucf = ucf;
    g->have_pin_table = use_pin_table;
    strncpy(g->entity_name, entity_name, sizeof(g->entity_name) - 1);

    for (int i = 0; i < MAX_NET; i++) g->rev[i].kind = REV_NONE;

    for (int fb = 1; fb <= MAX_FB; fb++) {
        for (int idx = 0; idx < MAX_PTERM; idx++) {
            AndTerm *a = &m->andterms[fb][idx];
            if (a->valid && a->out >= 0 && a->out < MAX_NET) {
                g->rev[a->out].kind = REV_AND; g->rev[a->out].fb = fb; g->rev[a->out].idx = idx;
            }
        }
        for (int mc = 1; mc <= MAX_MC; mc++) {
            OrTerm *o = &m->orterms[fb][mc];
            if (o->valid && o->out >= 0 && o->out < MAX_NET) {
                g->rev[o->out].kind = REV_OR; g->rev[o->out].fb = fb; g->rev[o->out].idx = mc;
            }
            XorGate *x = &m->xorgates[fb][mc];
            if (x->valid && x->out >= 0 && x->out < MAX_NET) {
                g->rev[x->out].kind = REV_XOR; g->rev[x->out].fb = fb; g->rev[x->out].idx = mc;
            }
            Reg *r = &m->regs[fb][mc];
            if (r->valid && r->q >= 0 && r->q < MAX_NET) {
                g->rev[r->q].kind = REV_REG; g->rev[r->q].fb = fb; g->rev[r->q].idx = mc;
            }
        }
    }

    build_pad_names(g);
    return g;
}

/* ------------------------------------------------------------------ */
/* Dead-code elimination                                               */
/* ------------------------------------------------------------------ */

static void mark(Generator *g, int net_id) {
    if (net_id < 0 || net_id >= MAX_NET) return; /* constants / none */
    RevEntry *r = &g->rev[net_id];
    Model *m = g->m;
    switch (r->kind) {
        case REV_REG: {
            if (g->needed_reg[r->fb][r->idx]) return;
            g->needed_reg[r->fb][r->idx] = 1;
            Reg *reg = &m->regs[r->fb][r->idx];
            mark(g, reg->d);
            mark(g, reg->clk);
            mark(g, reg->pre);
            mark(g, reg->clr);
            if (reg->ce != NET_NONE) mark(g, reg->ce);
            break;
        }
        case REV_XOR: {
            if (g->needed_comb[r->fb][r->idx]) return;
            g->needed_comb[r->fb][r->idx] = 1;
            XorGate *x = &m->xorgates[r->fb][r->idx];
            mark(g, x->orterm);
            mark(g, x->ptc);
            break;
        }
        case REV_OR: {
            if (g->needed_or[r->fb][r->idx]) return;
            g->needed_or[r->fb][r->idx] = 1;
            OrTerm *o = &m->orterms[r->fb][r->idx];
            for (int i = 0; i < o->n_in; i++) mark(g, o->in[i]);
            break;
        }
        case REV_AND: {
            if (g->needed_and[r->fb][r->idx]) return;
            g->needed_and[r->fb][r->idx] = 1;
            AndTerm *a = &m->andterms[r->fb][r->idx];
            for (int i = 0; i < a->n_true; i++) mark(g, a->true_in[i]);
            for (int i = 0; i < a->n_comp; i++) mark(g, a->comp_in[i]);
            break;
        }
        default: break; /* leaf: pad / global net */
    }
}

/* Is global net `net` read by an output enable or by a needed register? */
static int global_net_referenced(Generator *g, int net) {
    Model *m = g->m;
    for (int fb = 1; fb <= MAX_FB; fb++)
        for (int mc = 1; mc <= MAX_MC; mc++) {
            Iob *b = &m->iob[fb][mc];
            if (b->valid && b->has_output && b->oe == net) return 1;
            if (g->needed_reg[fb][mc]) {
                Reg *r = &m->regs[fb][mc];
                if (r->clk == net || r->pre == net || r->clr == net || r->ce == net) return 1;
            }
        }
    return 0;
}

/* §207 (D207-GTS). A global whose buffer input is tied off may still be
 * driven by the macrocell at its fixed location (§72/§73). The walk stops at
 * a global net -- it is a leaf -- so a driver macrocell that nothing ELSE
 * reads was never marked, the emitter saw it as dead, and the global folded
 * to a constant: gtsprobe/t2_hi's six pads came out always enabled against
 * cpldfit's `q.OE = en_MC.GLB`. Mark the driver when the global is
 * referenced, tied off, and its macrocell has product terms (on its OR gate,
 * or one on the XOR's PTC input);
 * repeat, since that logic may reference another global. */
static void mark_global_drivers(Generator *g) {
    Model *m = g->m;
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    static const char *gn[8] = { "gck_0", "gck_1", "gck_2", "gts_0", "gts_1", "gts_2", "gts_3", "gsr" };
    for (int changed = 1; changed; ) {
        changed = 0;
        /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
        for (int i = 0; i < 8; i++) {
            /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
            int net = i < 3 ? m->gck[i] : i < 7 ? m->gts[i - 3] : m->gsr;
            if (net < 0 || net >= MAX_NET || !m->global_const[net]) continue;
            if (!global_net_referenced(g, net)) continue;
            int fb, mc;
            if (!gen_global_driver_loc(g, gn[i], &fb, &mc)) continue;
            if (fb < 1 || fb > MAX_FB || mc < 1 || mc > MAX_MC) continue;
            XorGate *x = &m->xorgates[fb][mc];
            if (!x->valid || g->needed_comb[fb][mc]) continue;
            if (m->orterms[fb][mc].n_in <= 0 && (x->ptc < 0 || x->ptc >= MAX_NET)) continue;
            mark(g, x->out);
            changed = 1;
        }
    }
}

void generator_mark_needed(Generator *g) {
    Model *m = g->m;
    for (int fb = 1; fb <= MAX_FB; fb++)
        for (int mc = 1; mc <= MAX_MC; mc++) {
            Iob *b = &m->iob[fb][mc];
            if (b->valid && b->has_output) {
                mark(g, b->drive);
                if (b->oe != NET_NONE) mark(g, b->oe);
            }
        }
    mark_global_drivers(g);

    /* used_input_pads: any non-output pad whose readback net got
     * referenced while marking the needed and/or/xor/reg graph above. */
    for (int fb = 1; fb <= MAX_FB; fb++) {
        for (int mc = 1; mc <= MAX_MC; mc++) {
            if (!m->iob[fb][mc].valid || m->iob[fb][mc].has_output) continue;
            int rb = m->iob[fb][mc].readback;
            if (rb < 0 || rb >= MAX_NET) continue;
            int used = 0;
            /* referenced by any needed andterm true/comp */
            for (int fb2 = 1; fb2 <= MAX_FB && !used; fb2++)
                for (int idx = 0; idx < MAX_PTERM && !used; idx++) {
                    if (!g->needed_and[fb2][idx]) continue;
                    AndTerm *a = &m->andterms[fb2][idx];
                    for (int i = 0; i < a->n_true; i++) if (a->true_in[i] == rb) used = 1;
                    for (int i = 0; i < a->n_comp; i++) if (a->comp_in[i] == rb) used = 1;
                }
            /* referenced by any needed register's d/clk/pre/clr/ce */
            for (int fb2 = 1; fb2 <= MAX_FB && !used; fb2++)
                for (int mc2 = 1; mc2 <= MAX_MC && !used; mc2++) {
                    if (!g->needed_reg[fb2][mc2]) continue;
                    Reg *reg = &m->regs[fb2][mc2];
                    if (reg->d == rb || reg->clk == rb || reg->pre == rb ||
                        reg->clr == rb || reg->ce == rb) used = 1;
                }
            /* referenced by any needed xorgate's orterm/ptc */
            for (int fb2 = 1; fb2 <= MAX_FB && !used; fb2++)
                for (int mc2 = 1; mc2 <= MAX_MC && !used; mc2++) {
                    if (!g->needed_comb[fb2][mc2]) continue;
                    XorGate *x = &m->xorgates[fb2][mc2];
                    if (x->orterm == rb || x->ptc == rb) used = 1;
                }
            if (used) g->used_input_pad[fb][mc] = 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Name / expression resolution                                        */
/* ------------------------------------------------------------------ */

char *gen_comb_sig(int fb, int mc) {
    char *raw = xasprintf("comb_fb%d_mc%d", fb, mc), *id = gen_ident(raw);
    free(raw); return id;
}
char *gen_reg_sig(int fb, int mc) {
    char *raw = xasprintf("reg_fb%d_mc%d", fb, mc), *id = gen_ident(raw);
    free(raw); return id;
}

static void mark_global_used(Generator *g, const char *nm) {
    if (strcmp(nm, "gck_0") == 0) g->globals_used[0] = 1;
    else if (strcmp(nm, "gck_1") == 0) g->globals_used[1] = 1;
    else if (strcmp(nm, "gck_2") == 0) g->globals_used[2] = 1;
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    else if (strcmp(nm, "gts_0") == 0) g->globals_used[3] = 1;
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    else if (strcmp(nm, "gts_1") == 0) g->globals_used[4] = 1;
    else if (strcmp(nm, "gts_2") == 0) g->globals_used[5] = 1;
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    else if (strcmp(nm, "gts_3") == 0) g->globals_used[6] = 1;
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    else if (strcmp(nm, "gsr") == 0) g->globals_used[7] = 1;
}

char *gen_leaf_name(Generator *g, int net_id) {
    if (net_id == NET_ZERO) return xstrdup("'0'");
    if (net_id == NET_ONE) return xstrdup("'1'");
    Model *m = g->m;
    for (int fb = 1; fb <= MAX_FB; fb++)
        for (int mc = 1; mc <= MAX_MC; mc++) {
            Iob *b = &m->iob[fb][mc];
            if (!b->valid) continue;
            if (b->readback == net_id || b->pad == net_id) return xstrdup(g->pad_name[fb][mc]);
        }
    const char *nm = model_name_of(m, net_id);
    char *raw = vhdl_ident(nm);
    char *ident = gen_ident(raw);
    free(raw);
    if (strncmp(nm, "gck_", 4) == 0 || strncmp(nm, "gts_", 4) == 0 || strcmp(nm, "gsr") == 0)
        mark_global_used(g, nm);
    return ident;
}

char *gen_expr(Generator *g, int net_id) {
    if (net_id == NET_ZERO) return xstrdup("'0'");
    if (net_id == NET_ONE) return xstrdup("'1'");
    if (net_id < 0 || net_id >= MAX_NET) return xstrdup("'0'");

    RevEntry *r = &g->rev[net_id];
    Model *m = g->m;

    if (r->kind == REV_REG) return gen_reg_sig(r->fb, r->idx);
    if (r->kind == REV_XOR) return gen_comb_sig(r->fb, r->idx);

    if (r->kind == REV_OR) {
        OrTerm *o = &m->orterms[r->fb][r->idx];
        if (o->n_in == 0) return xstrdup("'0'");
        char **parts = malloc(sizeof(char *) * o->n_in);
        for (int i = 0; i < o->n_in; i++) parts[i] = gen_expr(g, o->in[i]);
        char *result;
        if (o->n_in == 1) {
            result = xstrdup(parts[0]);
        } else {
            char *acc = xstrdup("");
            for (int i = 0; i < o->n_in; i++) {
                char *piece = xasprintf("%s%s(%s)", acc, i > 0 ? " or " : "", parts[i]);
                free(acc);
                acc = piece;
            }
            result = acc;
        }
        for (int i = 0; i < o->n_in; i++) free(parts[i]);
        free(parts);
        return result;
    }

    if (r->kind == REV_AND) {
        AndTerm *a = &m->andterms[r->fb][r->idx];
        int n = a->n_true + a->n_comp;
        if (n == 0) return xstrdup("'1'");
        char **parts = malloc(sizeof(char *) * n);
        int k = 0;
        for (int i = 0; i < a->n_true; i++) parts[k++] = gen_expr(g, a->true_in[i]);
        for (int i = 0; i < a->n_comp; i++) {
            char *inner = gen_expr(g, a->comp_in[i]);
            parts[k++] = xasprintf("not (%s)", inner);
            free(inner);
        }
        char *result;
        if (n == 1) {
            result = xstrdup(parts[0]);
        } else {
            char *acc = xstrdup("");
            for (int i = 0; i < n; i++) {
                char *piece = xasprintf("%s%s(%s)", acc, i > 0 ? " and " : "", parts[i]);
                free(acc);
                acc = piece;
            }
            result = acc;
        }
        for (int i = 0; i < n; i++) free(parts[i]);
        free(parts);
        return result;
    }

    return gen_leaf_name(g, net_id);
}

char *gen_control_sig(Generator *g, const char *prefix, int fb, int mc, int net_id) {
    if (net_id == NET_ZERO) return xstrdup("'0'");
    if (net_id == NET_ONE) return xstrdup("'1'");
    if (net_id < 0 || net_id >= MAX_NET) return xstrdup("'0'");

    if (g->control_seen[net_id]) return xstrdup(g->control_seen[net_id]);

    RevEntry *r = &g->rev[net_id];
    if (r->kind == REV_NONE) {
        char *name = gen_leaf_name(g, net_id);
        g->control_seen[net_id] = xstrdup(name);
        return name;
    }

    char *rawn = xasprintf("%s_fb%d_mc%d", prefix, fb, mc);
    char *name = gen_ident(rawn);
    free(rawn);
    char *e = gen_expr(g, net_id);
    g->control_sigs[g->n_control_sigs].name = xstrdup(name);
    g->control_sigs[g->n_control_sigs].expr = e;
    g->n_control_sigs++;
    g->control_seen[net_id] = xstrdup(name);
    return name;
}

int gen_global_driver_loc(Generator *g, const char *global_name, int *out_fb, int *out_mc) {
    int fb = 0, mc = 0;
    if (strncmp(global_name, "gck_", 4) == 0) {
        /* PROVENANCE: DEFINED length of 'gck_' */
        if (!global_gck_loc(g->m->part_name, atoi(global_name + 4), &fb, &mc)) return 0;
    } else if (strncmp(global_name, "gts_", 4) == 0) {
        /* PROVENANCE: DEFINED length of 'gts_' */
        if (!global_gts_loc(g->m->part_name, atoi(global_name + 4), &fb, &mc)) return 0;
    } else if (strcmp(global_name, "gsr") == 0) {
        if (!global_gsr_loc(g->m->part_name, &fb, &mc)) return 0;
    } else {
        return 0;
    }
    if (out_fb) *out_fb = fb;
    if (out_mc) *out_mc = mc;
    return 1;
}

const char *gen_global_driver_pad(Generator *g, const char *global_name) {
    int fb, mc;
    if (strncmp(global_name, "gck_", 4) == 0) {
        /* PROVENANCE: DEFINED length of 'gck_' */
        int idx = atoi(global_name + 4);
        if (!global_gck_loc(g->m->part_name, idx, &fb, &mc)) return NULL;
    } else if (strncmp(global_name, "gts_", 4) == 0) {
        /* PROVENANCE: DEFINED length of 'gts_' */
        int idx = atoi(global_name + 4);
        if (!global_gts_loc(g->m->part_name, idx, &fb, &mc)) return NULL;
    } else if (strcmp(global_name, "gsr") == 0) {
        if (!global_gsr_loc(g->m->part_name, &fb, &mc)) return NULL;
    } else {
        return NULL;
    }
    return g->pad_name[fb][mc];
}

/* ------------------------------------------------------------------ */
/* Entity port list                                                     */
/* ------------------------------------------------------------------ */

const char *generator_pin_dir(Generator *g, int fb, int mc) {
    Model *m = g->m;
    Iob *b = &m->iob[fb][mc];
    if (!b->has_output) return "in";
    int rb = b->readback;
    if (rb >= 0 && rb < MAX_NET && g->rev[rb].kind != REV_NONE) return "inout";
    for (int fb2 = 1; fb2 <= MAX_FB; fb2++)
        for (int idx = 0; idx < MAX_PTERM; idx++) {
            AndTerm *a = &m->andterms[fb2][idx];
            if (!a->valid) continue;
            for (int i = 0; i < a->n_true; i++) if (a->true_in[i] == rb) return "inout";
            for (int i = 0; i < a->n_comp; i++) if (a->comp_in[i] == rb) return "inout";
        }
    /* The AND-term test above MISSES a readback that feeds a REGISTER's D
     * input directly, or a XOR gate's OR/PTC input. Such a pad would be
     * declared `out` and then read in the body, which is illegal VHDL: XST
     * rejects it with HDLParsers:1401 and ghdl with `port cannot be read`.
     * So consider those consumers too. */
    for (int fb2 = 1; fb2 <= MAX_FB; fb2++)
        for (int mc2 = 1; mc2 <= MAX_MC; mc2++) {
            Reg *r = &m->regs[fb2][mc2];
            if (r->d == rb || r->clk == rb || r->pre == rb ||
                r->clr == rb || r->ce == rb) return "inout";
            XorGate *x = &m->xorgates[fb2][mc2];
            if (x->orterm == rb || x->ptc == rb) return "inout";
        }
    if (rb == m->gsr) return "inout";
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    for (int i = 0; i < 3; i++) if (rb == m->gck[i]) return "inout";
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    for (int i = 0; i < 4; i++) if (rb == m->gts[i]) return "inout";
    return "out";
}

int generator_pad_is_used(Generator *g, int fb, int mc, int *vector_used_flags) {
    Model *m = g->m;
    Iob *b = &m->iob[fb][mc];
    if (b->has_output) return 1;
    char *name = g->pad_name[fb][mc];
    char *paren = strchr(name, '(');
    if (paren) {
        char base[128];
        size_t blen = paren - name;
        if (blen >= sizeof(base)) blen = sizeof(base) - 1;
        strncpy(base, name, blen);
        base[blen] = 0;
        for (int i = 0; i < g->n_vectors; i++)
            if (strcmp(g->vectors[i].base, base) == 0 && vector_used_flags[i]) return 1;
    }
    if (g->named_from_ucf[fb][mc]) return 1;
    return g->used_input_pad[fb][mc];
}

PortDecl *generator_build_ports(Generator *g, int *out_count) {
    Model *m = g->m;
    int *vec_used = calloc(g->n_vectors, sizeof(int));

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
            size_t blen = paren - name;
            if (blen >= sizeof(base)) blen = sizeof(base) - 1;
            strncpy(base, name, blen);
            base[blen] = 0;
            for (int i = 0; i < g->n_vectors; i++)
                if (strcmp(g->vectors[i].base, base) == 0) vec_used[i] = 1;
        }

    /* PROVENANCE: DEFINED room for the global ports */
    PortDecl *ports = malloc(sizeof(PortDecl) * (MAX_FB * MAX_MC + 8));
    int n = 0;
    int *emitted_vec = calloc(g->n_vectors, sizeof(int));

    for (int fb = 1; fb <= MAX_FB; fb++) {
        for (int mc = 1; mc <= MAX_MC; mc++) {
            if (!m->iob[fb][mc].valid) continue;
            if (!generator_pad_is_used(g, fb, mc, vec_used)) continue;
            char *name = g->pad_name[fb][mc];
            char *paren = strchr(name, '(');
            if (paren) {
                char base[128];
                size_t blen = paren - name;
                if (blen >= sizeof(base)) blen = sizeof(base) - 1;
                strncpy(base, name, blen);
                base[blen] = 0;
                int vidx = -1;
                for (int i = 0; i < g->n_vectors; i++)
                    if (strcmp(g->vectors[i].base, base) == 0) vidx = i;
                if (vidx < 0 || emitted_vec[vidx]) continue;
                emitted_vec[vidx] = 1;
                /* The declared range is the range the UCF actually names. A bus
                 * whose low bits are absent -- AD(22 downto 15) -- must not be
                 * widened to (22 downto 0): the extra ports would carry no LOC
                 * constraint and the fitter would place them on free pins. */
                int vhi = g->vectors[vidx].max_index;
                int vlo = g->vectors[vidx].min_index;
                if (vlo > vhi) vlo = vhi;

                const char *dir = NULL;
                int mixed = 0;
                for (int fb2 = 1; fb2 <= MAX_FB && !mixed; fb2++)
                    for (int mc2 = 1; mc2 <= MAX_MC && !mixed; mc2++) {
                        if (!m->iob[fb2][mc2].valid) continue;
                        char *nm2 = g->pad_name[fb2][mc2];
                        size_t bl = strlen(base);
                        if (strncmp(nm2, base, bl) == 0 && nm2[bl] == '(' &&
                            generator_pad_is_used(g, fb2, mc2, vec_used)) {
                            const char *d2 = generator_pin_dir(g, fb2, mc2);
                            if (!dir) dir = d2;
                            else if (strcmp(dir, d2) != 0) mixed = 1;
                        }
                    }
                ports[n].name = xstrdup(base);
                ports[n].direction = xstrdup(mixed || !dir ? "inout" : dir);
                ports[n].is_vector = 1;
                ports[n].hi = vhi;
                ports[n].lo = vlo;
                n++;
            } else {
                ports[n].name = xstrdup(name);
                ports[n].direction = xstrdup(generator_pin_dir(g, fb, mc));
                ports[n].is_vector = 0;
                ports[n].hi = ports[n].lo = 0;
                n++;
            }
        }
    }
    free(vec_used);
    free(emitted_vec);
    *out_count = n;
    return ports;
}
