#ifndef XC2C_COMPAT_H
#define XC2C_COMPAT_H

#include <stdio.h>
#include <stddef.h>

/*
 * MinGW/MSVC lack open_memstream and strndup -- both POSIX, not C99.
 *
 * Redirecting the call sites with `#define strndup(s,n) ...` does not work:
 * glibc DECLARES strndup in <string.h>, so reaching that header after this
 * one mangles the declaration and the build fails. Overriding a libc symbol
 * with a macro is fragile either way, since include order decides whether it
 * works.
 *
 * So the shims get their own names and each call site uses them explicitly.
 * On Linux and macOS they resolve straight to the system functions.
 */

#if defined(_WIN32) || defined(__MINGW32__) || defined(XC2C_FORCE_COMPAT)
/* PROVENANCE: DEFINED build switch */
#  define NEED_COMPAT_SHIMS 1
#  if defined(_WIN32) || defined(__MINGW32__)
#    include <process.h>   /* _getpid */
#  else
#    include <unistd.h>    /* XC2C_FORCE_COMPAT: exercise the shims on Linux */
#    define _getpid getpid
#  endif

FILE *xc2c_open_memstream(char **bufp, size_t *sizep);
/* Closing a memstream is where the buffer and its size are handed back, so
 * it needs its own close. Use it ONLY on a stream from xc2c_open_memstream. */
int   xc2c_close_memstream(FILE *fp);
char *xc2c_strndup(const char *s, size_t n);

#else  /* native POSIX */

#  include <string.h>
#  define xc2c_open_memstream(b, s)  open_memstream((b), (s))
#  define xc2c_close_memstream(f)    fclose((f))
#  define xc2c_strndup(s, n)         strndup((s), (n))

#endif

#endif /* XC2C_COMPAT_H */
