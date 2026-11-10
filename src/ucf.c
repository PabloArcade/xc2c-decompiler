#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "jed2vhdl.h"

static char *xstrdup(const char *s) { return s ? strdup(s) : NULL; }

/* Parses lines like:
 *   NET "SIGNAL_NAME" LOC = "37";
 *   NET "VECTOR_NAME(3)" LOC = "12";
 * '#' starts a comment (rest of line ignored). A pin assignment is a NET
 * line -- the keyword NET, in any case, first on the line -- naming a quoted
 * net and carrying a LOC constraint anywhere after the name: the token LOC on
 * its own (not part of a longer word such as CLOCK_DEDICATED_ROUTE or BLOC),
 * then '=', then a quoted value. It may share the line with other
 * constraints:  NET "x" IOSTANDARD = "LVCMOS33" | LOC = "P1";  */
UcfFile *ucf_parse(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open UCF %s\n", path); return NULL; }

    UcfFile *u = calloc(1, sizeof(UcfFile));
    /* PROVENANCE: DEFINED UCF syntax */
    int cap = 64;
    u->entries = malloc(sizeof(UcfEntry) * cap);
    u->count = 0;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;

        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (toupper((unsigned char)s[0]) != 'N' || toupper((unsigned char)s[1]) != 'E' ||
            /* PROVENANCE: DEFINED UCF syntax: 'NET ' */
            toupper((unsigned char)s[2]) != 'T' || (s[3] != ' ' && s[3] != '\t')) continue;
        /* PROVENANCE: DEFINED UCF syntax */
        s += 3;
        while (*s == ' ' || *s == '\t') s++;
        if (*s != '"') continue;
        char *q1 = s;
        char *q2 = strchr(q1 + 1, '"');
        if (!q2) continue;

        /* the value of the LOC token, wherever it sits after the name */
        char *q3 = NULL, *q4 = NULL;
        for (char *c = q2 + 1; *c && !q3; c++) {
            if (toupper((unsigned char)c[0]) != 'L' || toupper((unsigned char)c[1]) != 'O' ||
                toupper((unsigned char)c[2]) != 'C') continue;
            if (isalnum((unsigned char)c[-1]) || c[-1] == '_') continue;
            /* PROVENANCE: DEFINED UCF syntax: 'LOC' */
            char *p = c + 3;
            if (isalnum((unsigned char)*p) || *p == '_') continue;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '=') continue;
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '"') continue;
            char *end = strchr(p + 1, '"');
            if (end) { q3 = p; q4 = end; }
        }
        if (!q3) continue;

        char raw_name[256];
        size_t nlen = q2 - (q1 + 1);
        if (nlen >= sizeof(raw_name)) nlen = sizeof(raw_name) - 1;
        strncpy(raw_name, q1 + 1, nlen);
        raw_name[nlen] = 0;

        char pin_str[32];
        size_t plen = q4 - (q3 + 1);
        if (plen >= sizeof(pin_str)) plen = sizeof(pin_str) - 1;
        strncpy(pin_str, q3 + 1, plen);
        pin_str[plen] = 0;

        char base[256];
        int has_index = 0, index = 0;
        char *paren = strchr(raw_name, '(');
        char *angle = strchr(raw_name, '<');
        if (paren && raw_name[strlen(raw_name) - 1] == ')') {
            size_t blen = paren - raw_name;
            strncpy(base, raw_name, blen);
            base[blen] = 0;
            index = atoi(paren + 1);
            has_index = 1;
        } else if (angle && raw_name[strlen(raw_name) - 1] == '>') {
            size_t blen = angle - raw_name;
            strncpy(base, raw_name, blen);
            base[blen] = 0;
            index = atoi(angle + 1);
            has_index = 1;
        } else {
            strncpy(base, raw_name, sizeof(base) - 1);
            base[sizeof(base) - 1] = 0;
        }

        if (u->count >= cap) { cap *= 2; u->entries = realloc(u->entries, sizeof(UcfEntry) * cap); }
        u->entries[u->count].name = xstrdup(base);
        u->entries[u->count].has_index = has_index;
        u->entries[u->count].index = index;
        for (char *c = pin_str; *c; c++) *c = (char)toupper((unsigned char)*c);
        u->entries[u->count].loc = xstrdup(pin_str);
        u->count++;
    }
    fclose(f);
    return u;
}
