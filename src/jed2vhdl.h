#ifndef JED2VHDL_H
#define JED2VHDL_H

#include "json.h"
#include <stdio.h>
#include "compat.h"

char *xasprintf(const char *fmt, ...);
char *vhdl_ident(const char *name);


/* PROVENANCE: VENDOR largest part: 32 function blocks of 16 macrocells and 56 product terms, ISE device files and fitter reports, ziaderive/chp.py (§206) */
#define MAX_FB 32
/* PROVENANCE: VENDOR largest part: 32 function blocks of 16 macrocells and 56 product terms, ISE device files and fitter reports, ziaderive/chp.py (§206) */
#define MAX_MC 16
/* PROVENANCE: VENDOR largest part: 32 function blocks of 16 macrocells and 56 product terms, ISE device files and fitter reports, ziaderive/chp.py (§206) */
#define MAX_PTERM 56
/* PROVENANCE: DEFINED net table size */
#define MAX_NET 65536

/* Sentinel net references. A real net is >= 0, its id; these are the
 * specials, kept out of that range. */
/* PROVENANCE: DEFINED sentinel */
#define NET_NONE 0x7FFFFFF0
/* PROVENANCE: DEFINED sentinel */
#define NET_ZERO 0x7FFFFFF1
/* PROVENANCE: DEFINED sentinel */
#define NET_ONE  0x7FFFFFF2

typedef enum { REV_NONE, REV_AND, REV_OR, REV_XOR, REV_REG } RevKind;

typedef struct {
    /* PROVENANCE: DEFINED capacity, above the 40 ZIA inputs */
    int true_in[64], n_true;
    /* PROVENANCE: DEFINED capacity, above the 40 ZIA inputs */
    int comp_in[64], n_comp;
    int out;
    int valid;
} AndTerm;

typedef struct {
    int in[MAX_PTERM], n_in;
    int out;
    int valid;
} OrTerm;

typedef struct {
    int orterm, ptc, invert, out;
    int valid;
} XorGate;

typedef struct {
    char celltype[16];
    int init, is_latch, is_tff, is_dualedge, invert_clk, has_ce;
    int d, clk, pre, clr, ce, q;
    int valid;
} Reg;

typedef struct {
    int has_output;
    int drive, readback, pad, oe;
    int valid;
    /* Full output-buffer mode string ("open-drain", "push-pull", ...). */
    char obuf_mode[40];
} Iob;

typedef struct {
    RevKind kind;
    int fb, idx;
} RevEntry;

typedef struct {
    char *name;   /* e.g. "PAL09_I5" or "TP" for a vector base */
    int has_index;
    int index;
    char *loc;    /* the LOC value as written, upper-cased: "P12", "12", "D1" */
} UcfEntry;

typedef struct {
    char *base;
    int max_index; /* -1 if scalar */
    int min_index; /* lowest index seen; INT_MAX until one is */
} VectorDecl;

typedef struct {
    char *name;
    char *expr;
} ControlSig;

typedef struct {
    char *name;      /* final VHDL port name, e.g. "PAL09_I5" or "TP(3)" or "PIN_27" or "PAD_FB1_1" */
    char *direction; /* "in" / "out" / "inout" */
    int is_vector;
    int hi, lo;      /* vector bounds, as "(hi downto lo)" */
} PortDecl;

/* ---- Model: the decoded design. Built from the fuse map by
 * model_from_jed(), or from a netlist file by model_load(). ---- */
typedef struct {
    char part_name[32], part_pkg[32], part_speed[8];

    char *id2name[MAX_NET];        /* net id -> netnames-derived name */
    char *portname_of_id[MAX_NET]; /* net id -> top-level port name */
    char *port_direction_name[256];
    char *port_direction_val[256];
    int n_port_directions;
    char *readback_to_port[MAX_NET];

    AndTerm andterms[MAX_FB + 1][MAX_PTERM];
    OrTerm orterms[MAX_FB + 1][MAX_MC + 1];
    XorGate xorgates[MAX_FB + 1][MAX_MC + 1];
    Reg regs[MAX_FB + 1][MAX_MC + 1];
    Iob iob[MAX_FB + 1][MAX_MC + 1];
    int num_fb;

    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    int gck[3], gts[4], gsr;
    /* A global buffer can INVERT, and most do: leaving this unread puts the
     * wrong polarity on every output enable driven from one. */
    int global_invert[MAX_NET];
    /* A global buffer whose I input is a CONSTANT is a DISABLED global: the
     * net resolves to a constant and no pad is involved. '0'/'1' when so,
     * 0 when genuinely pad-driven. */
    char global_const[MAX_NET];
} Model;

Model *model_load(const char *json_path);
/* build the same Model from the fuse map, with no external decoder. */
struct JedGeom;
Model *model_from_jed(const struct JedGeom *g);
int    zia_check(const struct JedGeom *g);   /* 0 = an unrecognised ZIA pattern, refused */
const char *model_name_of(Model *m, int net_id);

/* ---- Pin tables ---- */
/* 1 when every pin of the part is a number (wire-bond), 0 for a BGA or no table */
int  pin_table_wire_bond(const char *device, const char *pkg);
/* one spelling of a pin for comparison: P stripped only on a wire-bond part */
void pin_canon(const char *loc, int wire_bond, char *out, size_t n);
/* returns 1 and points *pin at the pin's name ("40", "D1") if known, else 0 */
int pin_table_lookup_str(const char *device, const char *pkg, int fb, int mc, const char **pin);

/* ---- Global net FB/MC tables (per device) ---- */
int global_gck_loc(const char *device, int idx, int *fb, int *mc);
int global_gts_loc(const char *device, int idx, int *fb, int *mc);
int global_gsr_loc(const char *device, int *fb, int *mc);

/* ---- UCF parsing ---- */
typedef struct {
    UcfEntry *entries;
    int count;
} UcfFile;

UcfFile *ucf_parse(const char *path);

/* ---- Generator ---- */
typedef struct {
    Model *m;
    UcfFile *ucf;
    int have_pin_table;
    char entity_name[64];

    RevEntry rev[MAX_NET];

    int needed_and[MAX_FB + 1][MAX_PTERM];
    int needed_or[MAX_FB + 1][MAX_MC + 1];
    int needed_comb[MAX_FB + 1][MAX_MC + 1];
    int comb_is_const[MAX_FB + 1][MAX_MC + 1];  /* folds to '0' or '1' */
    int needed_reg[MAX_FB + 1][MAX_MC + 1];
    int used_input_pad[MAX_FB + 1][MAX_MC + 1];

    char *pad_name[MAX_FB + 1][MAX_MC + 1];
    int named_from_ucf[MAX_FB + 1][MAX_MC + 1];
    /* PROVENANCE: DEFINED capacity */
    VectorDecl vectors[64];
    int n_vectors;

    /* PROVENANCE: DEFINED capacity */
    ControlSig control_sigs[512];
    int n_control_sigs;
    char *control_seen[MAX_NET]; /* net id -> already-assigned control signal name */

    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    int globals_used[8]; /* 0-2 gck0-2, 3-6 gts0-3, 7 gsr */
} Generator;

Generator *generator_new(Model *m, UcfFile *ucf, int use_pin_table, const char *entity_name);
void generator_mark_needed(Generator *g);
char *gen_expr(Generator *g, int net_id);
char *gen_leaf_name(Generator *g, int net_id);
char *gen_control_sig(Generator *g, const char *prefix, int fb, int mc, int net_id);
char *gen_ident(const char *raw);
void gen_write_renames(FILE *out);
char *gen_comb_sig(int fb, int mc);
char *gen_reg_sig(int fb, int mc);
PortDecl *generator_build_ports(Generator *g, int *out_count);
const char *gen_global_driver_pad(Generator *g, const char *global_name);
int gen_global_driver_loc(Generator *g, const char *global_name, int *out_fb, int *out_mc);

/* Exposed so the UCF emitter reuses the exact same pad-selection and
 * direction logic as the VHDL entity builder - the .vhd and .ucf must
 * never disagree about which pads exist or which way they point. */
int generator_pad_is_used(Generator *g, int fb, int mc, int *vector_used_flags);
const char *generator_pin_dir(Generator *g, int fb, int mc);


/* platform null device, for building VHDL into a sink to populate
 * generator state without writing a file */
#ifdef _WIN32
#  define NULLDEV "NUL"
#else
#  define NULLDEV "/dev/null"
#endif

/* ---- decode / verify / device list (xc9500dump-compatible commands) ---- */
void emit_used(Generator *g, FILE *out);
void emit_decode(Generator *g, const char *jed_filename, FILE *out);
int  emit_verify(Generator *g, const char *ucf_path, FILE *out);
void pintable_list(void);

/* ---- UCF emission (round-trip support) ---- */
void emit_ucf(Generator *g, const char *jed_filename, int lock_macrocells,
              FILE *out, int *out_npinned, int *out_nunpinned);

/* ---- VHDL emission ---- */
/* 0 on success; -1, with a message, if a working buffer could not be opened */
int  emit_vhdl(Generator *g, const char *jed_filename, FILE *out);

#endif
