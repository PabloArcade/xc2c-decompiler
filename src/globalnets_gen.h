/* Generated data -- do not edit.
 *
 * Where the global clock, output-enable and set/reset nets enter the die, per
 * device: the function block and macrocell of the pad the vendor's own device
 * tables mark GCK0..2, GTS0..3 and GSR. FB and MC are 1-indexed. */

typedef struct { int fb, mc; } GenLoc;

/* PROVENANCE: VENDOR global-net pads per part: ISE device files via genglobals.py (§206) */
static const char *const gen_devices[] = {
    "XC2C32A",
    "XC2C64A",
    "XC2C128",
    "XC2C256",
    "XC2C384",
    "XC2C512",
    "XC2C32",
    "XC2C64",
};
/* PROVENANCE: VENDOR global-net pads per part: ISE device files via genglobals.py (§206) */
#define GEN_NDEV 8

/* PROVENANCE: VENDOR global-net pads per part: ISE device files via genglobals.py (§206) */
static const GenLoc gen_gck[GEN_NDEV][3] = {
    /* XC2C32A  */ {{2,5},{2,6},{2,7}},
    /* XC2C64A  */ {{2,7},{2,8},{2,10}},
    /* XC2C128  */ {{2,13},{2,14},{2,16}},
    /* XC2C256  */ {{5,6},{5,4},{6,4}},
    /* XC2C384  */ {{7,15},{7,12},{8,2}},
    /* XC2C512  */ {{11,3},{9,16},{10,3}},
    /* XC2C32   */ {{2,5},{2,6},{2,7}},
    /* XC2C64   */ {{2,7},{2,8},{2,10}},
};

/* PROVENANCE: VENDOR global-net pads per part: ISE device files via genglobals.py (§206) */
static const GenLoc gen_gts[GEN_NDEV][4] = {
    /* XC2C32A  */ {{1,5},{1,4},{1,7},{1,6}},
    /* XC2C64A  */ {{1,10},{1,9},{1,12},{1,11}},
    /* XC2C128  */ {{1,16},{1,15},{3,3},{3,2}},
    /* XC2C256  */ {{2,5},{2,12},{2,1},{2,3}},
    /* XC2C384  */ {{2,5},{2,15},{2,1},{2,3}},
    /* XC2C512  */ {{1,1},{2,14},{1,13},{1,3}},
    /* XC2C32   */ {{1,5},{1,4},{1,7},{1,6}},
    /* XC2C64   */ {{1,10},{1,9},{1,12},{1,11}},
};

/* PROVENANCE: VENDOR global-net pads per part: ISE device files via genglobals.py (§206) */
static const GenLoc gen_gsr[GEN_NDEV] = {
    /* XC2C32A  */ {1,8},
    /* XC2C64A  */ {1,13},
    /* XC2C128  */ {3,4},
    /* XC2C256  */ {1,3},
    /* XC2C384  */ {1,3},
    /* XC2C512  */ {1,16},
    /* XC2C32   */ {1,8},
    /* XC2C64   */ {1,13},
};
