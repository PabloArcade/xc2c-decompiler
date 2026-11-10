#include <string.h>
#include "jed2vhdl.h"

#include "globalnets_gen.h"

/* Where each global net enters the die. The table in globalnets_gen.h covers
 * all eight part numbers, the non-A XC2C32 and XC2C64 included: the vendor
 * states those separately even though they are the same die as the A parts. */
#define Loc GenLoc
#define gck_table gen_gck
#define gts_table gen_gts
#define gsr_table gen_gsr
#define devices gen_devices

static int dev_index(const char *device) {
    for (int i = 0; i < GEN_NDEV; i++)
        if (strcmp(devices[i], device) == 0) return i;
    return -1;
}

int global_gck_loc(const char *device, int idx, int *fb, int *mc) {
    int di = dev_index(device);
    if (di < 0 || idx < 0 || idx > 2) return 0;
    *fb = gck_table[di][idx].fb;
    *mc = gck_table[di][idx].mc;
    return 1;
}
int global_gts_loc(const char *device, int idx, int *fb, int *mc) {
    int di = dev_index(device);
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    if (di < 0 || idx < 0 || idx > 3) return 0;
    *fb = gts_table[di][idx].fb;
    *mc = gts_table[di][idx].mc;
    return 1;
}
int global_gsr_loc(const char *device, int *fb, int *mc) {
    int di = dev_index(device);
    if (di < 0) return 0;
    *fb = gsr_table[di].fb;
    *mc = gsr_table[di].mc;
    return 1;
}
