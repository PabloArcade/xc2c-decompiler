#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

/* Portable "asprintf": returns a freshly malloc'd formatted string. */
char *xasprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *buf = malloc(n + 1);
    vsnprintf(buf, n + 1, fmt, ap2);
    va_end(ap2);
    return buf;
}

/* Sanitizes a string into a safe VHDL identifier. Always returns a freshly
 * malloc'd string. */
char *vhdl_ident(const char *name) {
    size_t len = strlen(name);
    /* PROVENANCE: DEFINED room for an escape */
    char *out = malloc(len + 6);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char ch = name[i];
        out[j++] = (isalnum((unsigned char)ch) || ch == '_') ? ch : '_';
    }
    out[j] = 0;
    if (j == 0 || !isalpha((unsigned char)out[0])) {
        char *prefixed = xasprintf("sig_%s", out);
        free(out);
        return prefixed;
    }
    return out;
}
