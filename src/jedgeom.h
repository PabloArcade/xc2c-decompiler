/* jedgeom.h -- fuse-map geometry from the JED's own Note Block headers.
 * See jedgeom.c. Density-agnostic; handles both the 27-bit and 29-bit
 * macrocell layouts. */
#ifndef JEDGEOM_H
#include <stddef.h>

#define JEDGEOM_H

typedef struct JedGeom JedGeom;

/* The global-net fuse block, decoded. See jedgeom_globals(). */
typedef struct {
    int gsr_enable, gsr_invert;
    /* PROVENANCE: VENDOR 3 GCK, 4 GTS, 1 GSR on every part: ISE device files, ziaderive/chp.py (genglobals.py, §206) */
    int gts_enable[4], gts_invert[4];
    int global_pu;
} JedGlobals;

int jedgeom_globals(const JedGeom *g, JedGlobals *out);  /* 0 if unavailable */

JedGeom    *jedgeom_read(const char *path);   /* NULL if no Note Block headers */
void        jedgeom_free(JedGeom *g);

int         jedgeom_nblk(const JedGeom *g);
int         jedgeom_qf(const JedGeom *g);
const char *jedgeom_dev(const JedGeom *g);
int         jedgeom_nmc(const JedGeom *g, int b);
int         jedgeom_mcw(const JedGeom *g, int b);
const char *jedgeom_mc(const JedGeom *g, int b, int i);
int         jedgeom_zia_n(const JedGeom *g, int b);
int         jedgeom_zia_w(const JedGeom *g, int b);
int         jedgeom_and_n(const JedGeom *g, int b);
int         jedgeom_and_w(const JedGeom *g, int b);
int         jedgeom_or_n(const JedGeom *g, int b);
int         jedgeom_or_w(const JedGeom *g, int b);

int         jedgeom_is_ddr(const char *cfg);
int         jedgeom_init_state(const char *cfg);
int         jedgeom_print_config(const JedGeom *g, int fb, int mc);

#endif

/* accessors used by modelgen.c to build the Model from the fuse map. */
const char *jedgeom_zia_row(const JedGeom *g, int b, int r);
const char *jedgeom_and_row(const JedGeom *g, int b, int p);
const char *jedgeom_or_row(const JedGeom *g, int b, int p);
int         jedgeom_mc_n(const JedGeom *g, int b);
int         jedgeom_mc_w(const JedGeom *g, int b, int i);
int         jedgeom_field(const JedGeom *g, int fb, int mc, const char *name,
                          char *out, size_t n);
int         jedgeom_density(const JedGeom *g);
