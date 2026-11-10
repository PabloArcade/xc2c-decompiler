#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jed2vhdl.h"

static char *xstrdup(const char *s) { return s ? strdup(s) : NULL; }

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (fread(buf, 1, sz, f) != (size_t)sz) { /* ignore short read on trusted local files */ }
    buf[sz] = 0;
    fclose(f);
    return buf;
}

/* Parses "FB2_P49" -> fb=2, idx=49 (0-based p-term#). Returns 1 on match. */
static int parse_fbp(const char *loc, int *fb, int *idx) {
    return sscanf(loc, "FB%d_P%d", fb, idx) == 2;
}

/* Parses "FB4_7" -> fb=4, mc=7 (1-based macrocell#). Returns 1 on match. */
static int parse_fbm(const char *loc, int *fb, int *mc) {
    return sscanf(loc, "FB%d_%d", fb, mc) == 2;
}

/* Extract a single connection net id or constant, given a JSON array like
 * [123] or ["0"] or ["1"]. Returns NET_NONE if absent/empty. */
static int single_conn(JsonValue *conn_obj, const char *key) {
    JsonValue *arr = json_get(conn_obj, key);
    if (!arr || json_len(arr) == 0) return NET_NONE;
    JsonValue *item = json_at(arr, 0);
    if (item->type == JSON_STR) {
        if (strcmp(item->str, "0") == 0) return NET_ZERO;
        if (strcmp(item->str, "1") == 0) return NET_ONE;
        return NET_NONE;
    }
    return (int)item->num;
}

/* Fills an int array from a JSON array, skipping the non-integer entries:
 * a connection list may hold the string constants "0" and "1" alongside the
 * net ids. */
static int int_list(JsonValue *conn_obj, const char *key, int *out, int maxout) {
    JsonValue *arr = json_get(conn_obj, key);
    int n = 0;
    if (!arr) return 0;
    for (int i = 0; i < json_len(arr) && n < maxout; i++) {
        JsonValue *item = json_at(arr, i);
        if (item->type == JSON_NUM) out[n++] = (int)item->num;
    }
    return n;
}

/* A netlist file may split a net of fan-out > 1, rerouting the extra consumers
 * through buffer cells (Y = A). Resolve any net id back through them. */
static int *g_buf_redirect; /* MAX_NET, -1 = no redirect, else target id */

static int canon(int net_id) {
    if (net_id < 0) return net_id; /* constants / NONE pass through untouched */
    int seen_guard = 0;
    /* PROVENANCE: DEFINED loop guard */
    while (net_id < MAX_NET && g_buf_redirect[net_id] >= 0 && seen_guard < 1000) {
        net_id = g_buf_redirect[net_id];
        seen_guard++;
    }
    return net_id;
}

static int find_id_by_name(Model *m, const char *name) {
    for (int i = 0; i < MAX_NET; i++)
        if (m->id2name[i] && strcmp(m->id2name[i], name) == 0) return i;
    return -1;
}

Model *model_load(const char *json_path) {
    char *text = read_file(json_path);
    JsonValue *root = json_parse(text);
    JsonValue *mod = json_get(json_get(root, "modules"), "top");

    Model *m = calloc(1, sizeof(Model));
    JsonValue *attrs = json_get(mod, "attributes");
    const char *pn = json_as_str(json_get(attrs, "PART_NAME"));
    const char *pp = json_as_str(json_get(attrs, "PART_PKG"));
    const char *ps = json_as_str(json_get(attrs, "PART_SPEED"));
    if (pn) strncpy(m->part_name, pn, sizeof(m->part_name) - 1);
    if (pp) strncpy(m->part_pkg, pp, sizeof(m->part_pkg) - 1);
    if (ps) strncpy(m->part_speed, ps, sizeof(m->part_speed) - 1);

    JsonValue *cells = json_get(mod, "cells");
    JsonValue *netnames = json_get(mod, "netnames");
    JsonValue *ports = json_get(mod, "ports");

    /* id -> name from netnames */
    for (int i = 0; i < json_len(netnames); i++) {
        const char *name = netnames->keys[i];
        JsonValue *info = netnames->vals[i];
        JsonValue *bits = json_get(info, "bits");
        for (int b = 0; b < json_len(bits); b++) {
            JsonValue *bit = json_at(bits, b);
            if (bit->type == JSON_NUM) {
                int id = (int)bit->num;
                if (id >= 0 && id < MAX_NET) m->id2name[id] = xstrdup(name);
            }
        }
    }

    /* id -> port name / direction */
    for (int i = 0; i < json_len(ports); i++) {
        const char *name = ports->keys[i];
        JsonValue *info = ports->vals[i];
        JsonValue *bits = json_get(info, "bits");
        const char *dir = json_as_str(json_get(info, "direction"));
        for (int b = 0; b < json_len(bits); b++) {
            JsonValue *bit = json_at(bits, b);
            if (bit->type == JSON_NUM) {
                int id = (int)bit->num;
                if (id >= 0 && id < MAX_NET) m->portname_of_id[id] = xstrdup(name);
            }
        }
        /* PROVENANCE: DEFINED capacity */
        if (m->n_port_directions < 256) {
            m->port_direction_name[m->n_port_directions] = xstrdup(name);
            m->port_direction_val[m->n_port_directions] = xstrdup(dir ? dir : "input");
            m->n_port_directions++;
        }
    }

    /* $_BUF_ redirect table */
    g_buf_redirect = malloc(sizeof(int) * MAX_NET);
    for (int i = 0; i < MAX_NET; i++) g_buf_redirect[i] = -1;
    for (int i = 0; i < MAX_NET; i++) m->global_invert[i] = 0;
    for (int i = 0; i < MAX_NET; i++) m->global_const[i] = 0;
    for (int i = 0; i < json_len(cells); i++) {
        JsonValue *c = cells->vals[i];
        const char *ctype = json_as_str(json_get(c, "type"));
        if (ctype && strcmp(ctype, "$_BUF_") == 0) {
            JsonValue *conn = json_get(c, "connections");
            int y = single_conn(conn, "Y");
            int a = single_conn(conn, "A");
            if (y >= 0 && y < MAX_NET) g_buf_redirect[y] = a;
        }
        /* Global buffers can invert the net they drive. Recorded in the same
         * pre-pass so it is available before any macrocell is built. */
        if (ctype && (strcmp(ctype, "BUFGTS") == 0 ||
                      strcmp(ctype, "BUFGSR") == 0 ||
                      strcmp(ctype, "BUFG") == 0)) {
            JsonValue *par = json_get(c, "parameters");
            JsonValue *inv = par ? json_get(par, "INVERT") : NULL;
            int o = single_conn(json_get(c, "connections"), "O");
            if (o >= 0 && o < MAX_NET) {
                if (inv && (int)json_as_num(inv)) m->global_invert[o] = 1;
                /* Also record a tied-off input: that means the global is
                 * DISABLED, not driven from its silicon pad. */
                int gin = single_conn(json_get(c, "connections"), "I");
                if (gin == NET_ZERO)      m->global_const[o] = '0';
                else if (gin == NET_ONE)  m->global_const[o] = '1';
            }
        }
    }

    /* main cell pass */
    for (int i = 0; i < json_len(cells); i++) {
        JsonValue *c = cells->vals[i];
        const char *ctype = json_as_str(json_get(c, "type"));
        JsonValue *cattrs = json_get(c, "attributes");
        const char *loc = json_as_str(json_get(cattrs, "LOC"));
        JsonValue *conn = json_get(c, "connections");
        if (!ctype || !loc) continue;

        if (strcmp(ctype, "ANDTERM") == 0) {
            int fb, idx;
            if (!parse_fbp(loc, &fb, &idx)) continue;
            if (fb > MAX_FB || idx >= MAX_PTERM) continue;
            AndTerm *a = &m->andterms[fb][idx];
            /* PROVENANCE: DEFINED capacity */
            a->n_true = int_list(conn, "IN", a->true_in, 64);
            /* PROVENANCE: DEFINED capacity */
            a->n_comp = int_list(conn, "IN_B", a->comp_in, 64);
            for (int k = 0; k < a->n_true; k++) a->true_in[k] = canon(a->true_in[k]);
            for (int k = 0; k < a->n_comp; k++) a->comp_in[k] = canon(a->comp_in[k]);
            a->out = canon(single_conn(conn, "OUT"));
            a->valid = 1;
            if (fb > m->num_fb) m->num_fb = fb;

        } else if (strcmp(ctype, "ORTERM") == 0) {
            int fb, mc;
            if (!parse_fbm(loc, &fb, &mc)) continue;
            if (fb > MAX_FB || mc > MAX_MC) continue;
            OrTerm *o = &m->orterms[fb][mc];
            o->n_in = int_list(conn, "IN", o->in, MAX_PTERM);
            for (int k = 0; k < o->n_in; k++) o->in[k] = canon(o->in[k]);
            o->out = canon(single_conn(conn, "OUT"));
            o->valid = 1;

        } else if (strcmp(ctype, "MACROCELL_XOR") == 0) {
            int fb, mc;
            if (!parse_fbm(loc, &fb, &mc)) continue;
            if (fb > MAX_FB || mc > MAX_MC) continue;
            XorGate *x = &m->xorgates[fb][mc];
            x->orterm = canon(single_conn(conn, "IN_ORTERM"));
            x->ptc = canon(single_conn(conn, "IN_PTC"));
            JsonValue *params = json_get(c, "parameters");
            JsonValue *inv = json_get(params, "INVERT_OUT");
            x->invert = inv ? (int)json_as_num(inv) : 0;
            x->out = canon(single_conn(conn, "OUT"));
            x->valid = 1;

        } else if (strcmp(ctype, "FDCP") == 0 || strcmp(ctype, "FDCP_N") == 0 ||
                   strcmp(ctype, "FDDCP") == 0 || strcmp(ctype, "LDCP") == 0 ||
                   strcmp(ctype, "LDCP_N") == 0 || strcmp(ctype, "FTCP") == 0 ||
                   strcmp(ctype, "FTCP_N") == 0 || strcmp(ctype, "FTDCP") == 0 ||
                   strcmp(ctype, "FDCPE") == 0 || strcmp(ctype, "FDCPE_N") == 0 ||
                   strcmp(ctype, "FDDCPE") == 0) {
            int fb, mc;
            if (!parse_fbm(loc, &fb, &mc)) continue;
            if (fb > MAX_FB || mc > MAX_MC) continue;
            Reg *r = &m->regs[fb][mc];
            strncpy(r->celltype, ctype, sizeof(r->celltype) - 1);
            r->is_latch = (strncmp(ctype, "LDCP", 4) == 0);
            r->is_tff = (strncmp(ctype, "FT", 2) == 0);
            r->is_dualedge = (strstr(ctype, "DD") != NULL);
            size_t len = strlen(ctype);
            r->invert_clk = (len >= 2 && strcmp(ctype + len - 2, "_N") == 0);
            r->has_ce = (json_get(conn, "CE") != NULL);
            const char *d_key = r->is_tff ? "T" : "D";
            const char *clk_key = r->is_latch ? "G" : "C";
            r->d = canon(single_conn(conn, d_key));
            r->clk = canon(single_conn(conn, clk_key));
            r->pre = canon(single_conn(conn, "PRE"));
            r->clr = canon(single_conn(conn, "CLR"));
            r->ce = r->has_ce ? canon(single_conn(conn, "CE")) : NET_NONE;
            r->q = canon(single_conn(conn, "Q"));
            JsonValue *params = json_get(c, "parameters");
            JsonValue *initv = json_get(params, "INIT");
            r->init = initv ? (int)json_as_num(initv) : 0;
            r->valid = 1;

        } else if (strcmp(ctype, "IOBUFE") == 0) {
            int fb, mc;
            if (!parse_fbm(loc, &fb, &mc)) continue;
            if (fb > MAX_FB || mc > MAX_MC) continue;
            Iob *b = &m->iob[fb][mc];
            b->has_output = 1;
            b->drive = canon(single_conn(conn, "I"));
            b->readback = canon(single_conn(conn, "O"));
            b->pad = canon(single_conn(conn, "IO"));
            b->oe = canon(single_conn(conn, "E"));
            {   /* Full output-buffer mode, when the netlist states it. */
                JsonValue *at = json_get(c, "attributes");
                JsonValue *om = at ? json_get(at, "OBUF_MODE") : NULL;
                const char *s = om ? json_as_str(om) : NULL;
                snprintf(b->obuf_mode, sizeof(b->obuf_mode), "%s", s ? s : "");
            }
            b->valid = 1;

        } else if (strcmp(ctype, "IBUF") == 0) {
            int fb, mc;
            if (!parse_fbm(loc, &fb, &mc)) continue; /* else: XC2C32 dedicated ipad, N/A here */
            if (fb > MAX_FB || mc > MAX_MC) continue;
            Iob *b = &m->iob[fb][mc];
            b->has_output = 0;
            b->drive = NET_NONE;
            b->readback = canon(single_conn(conn, "O"));
            b->pad = canon(single_conn(conn, "I"));
            b->oe = NET_NONE;
            b->valid = 1;
        }
    }

    /* readback -> port name redirect */
    for (int fb = 1; fb <= MAX_FB; fb++) {
        for (int mc = 1; mc <= MAX_MC; mc++) {
            Iob *b = &m->iob[fb][mc];
            if (!b->valid) continue;
            if (b->pad >= 0 && b->pad < MAX_NET && m->portname_of_id[b->pad]) {
                if (b->readback >= 0 && b->readback < MAX_NET)
                    m->readback_to_port[b->readback] = xstrdup(m->portname_of_id[b->pad]);
            }
        }
    }

    m->gck[0] = find_id_by_name(m, "gck_0");
    m->gck[1] = find_id_by_name(m, "gck_1");
    m->gck[2] = find_id_by_name(m, "gck_2");
    m->gts[0] = find_id_by_name(m, "gts_0");
    m->gts[1] = find_id_by_name(m, "gts_1");
    m->gts[2] = find_id_by_name(m, "gts_2");
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    m->gts[3] = find_id_by_name(m, "gts_3");
    m->gsr = find_id_by_name(m, "gsr");

    free(text);
    return m;
}

const char *model_name_of(Model *m, int net_id) {
    static char buf[32];
    if (net_id == NET_ZERO) return "'0'";
    if (net_id == NET_ONE) return "'1'";
    if (net_id == NET_NONE || net_id < 0 || net_id >= MAX_NET) return "'0'";
    if (m->readback_to_port[net_id]) return m->readback_to_port[net_id];
    if (m->portname_of_id[net_id]) return m->portname_of_id[net_id];
    if (m->id2name[net_id]) return m->id2name[net_id];
    snprintf(buf, sizeof(buf), "net_%d", net_id);
    return buf;
}
