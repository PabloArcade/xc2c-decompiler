/*
 * compat.c / compat.h -- POSIX functions MinGW does not provide.
 *
 * `open_memstream` and `strndup` are POSIX, not C99, and are absent from the
 * MinGW/MSVC runtimes. The Linux build uses the real ones; on Windows these
 * stand in. Behaviour that matters here:
 *
 *   open_memstream  - the buffer must be valid AND null-terminated after
 *                     close, and *sizep must hold the byte count. The
 *                     emitter builds the whole VHDL body through one of
 *                     these, so a wrong size silently truncates output.
 *   strndup         - copy at most n bytes and always null-terminate.
 */

#include "compat.h"

#ifdef NEED_COMPAT_SHIMS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *xc2c_strndup(const char *s, size_t n) {
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len);
    out[len] = 0;
    return out;
}

/* A memstream backed by a temp file. Simpler and more obviously correct than
 * hooking the CRT's stream internals, and this is a build-time path that runs
 * a handful of times per invocation, so the I/O cost is irrelevant. */
typedef struct MemStream {
    FILE   *fp;
    char  **bufp;
    size_t *sizep;
    char    path[512];
    struct MemStream *next;
} MemStream;

static MemStream *g_streams = NULL;

FILE *xc2c_open_memstream(char **bufp, size_t *sizep) {
    MemStream *ms = (MemStream *)calloc(1, sizeof(MemStream));
    if (!ms) return NULL;

    const char *tdir = getenv("TEMP");
    if (!tdir) tdir = getenv("TMP");
    if (!tdir) tdir = ".";

    static unsigned counter = 0;
    snprintf(ms->path, sizeof(ms->path), "%s/xc2c_ms_%u_%u.tmp",
             tdir, (unsigned)_getpid(), counter++);

    ms->fp = fopen(ms->path, "w+b");
    if (!ms->fp) { free(ms); return NULL; }

    ms->bufp = bufp;
    ms->sizep = sizep;
    *bufp = NULL;
    *sizep = 0;

    ms->next = g_streams;
    g_streams = ms;
    return ms->fp;
}

int xc2c_close_memstream(FILE *fp) {
    MemStream **pp = &g_streams;
    while (*pp && (*pp)->fp != fp) pp = &(*pp)->next;

    if (!*pp) return fclose(fp);   /* defensive: not one of ours */

    MemStream *ms = *pp;
    *pp = ms->next;

    fflush(fp);
    long sz = ftell(fp);
    if (sz < 0) sz = 0;
    fseek(fp, 0, SEEK_SET);

    char *buf = (char *)malloc((size_t)sz + 1);
    size_t got = 0;
    if (buf) {
        got = fread(buf, 1, (size_t)sz, fp);
        buf[got] = 0;              /* callers treat it as a C string */
    }
    fclose(fp);
    remove(ms->path);

    *ms->bufp = buf;
    *ms->sizep = got;
    free(ms);
    return 0;
}

#endif /* NEED_COMPAT_SHIMS */
