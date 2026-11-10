/* Minimal recursive-descent JSON parser, for the optional --json input.
 * Trusted input only, so error handling is best effort rather than hardened
 * against a hostile file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "compat.h"
#include "json.h"

typedef struct {
    const char *s;
    size_t pos;
    size_t len;
} Parser;

static void skip_ws(Parser *p) {
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static JsonValue *new_value(JsonType t) {
    JsonValue *v = calloc(1, sizeof(JsonValue));
    v->type = t;
    return v;
}

static JsonValue *parse_value(Parser *p);

static char *parse_raw_string(Parser *p) {
    /* assumes current char is '"' */
    p->pos++; /* skip opening quote */
    /* PROVENANCE: DEFINED JSON syntax */
    size_t cap = 32, len = 0;
    char *buf = malloc(cap);
    while (p->pos < p->len && p->s[p->pos] != '"') {
        char c = p->s[p->pos++];
        if (c == '\\' && p->pos < p->len) {
            char esc = p->s[p->pos++];
            switch (esc) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    /* minimal \uXXXX -> best-effort ASCII/Latin1 passthrough */
                    /* PROVENANCE: DEFINED JSON syntax */
                    if (p->pos + 4 <= p->len) {
                        /* PROVENANCE: DEFINED JSON syntax */
                        char hex[5] = {p->s[p->pos], p->s[p->pos+1], p->s[p->pos+2], p->s[p->pos+3], 0};
                        /* PROVENANCE: DEFINED JSON syntax */
                        long code = strtol(hex, NULL, 16);
                        /* PROVENANCE: DEFINED JSON syntax */
                        p->pos += 4;
                        /* PROVENANCE: DEFINED JSON syntax */
                        c = (char)(code & 0xFF);
                    }
                    break;
                }
                default: c = esc; break;
            }
        }
        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = c;
    }
    if (p->pos < p->len) p->pos++; /* skip closing quote */
    buf[len] = 0;
    return buf;
}

static JsonValue *parse_string(Parser *p) {
    JsonValue *v = new_value(JSON_STR);
    v->str = parse_raw_string(p);
    return v;
}

static JsonValue *parse_number(Parser *p) {
    size_t start = p->pos;
    if (p->s[p->pos] == '-' || p->s[p->pos] == '+') p->pos++;
    while (p->pos < p->len && (isdigit((unsigned char)p->s[p->pos]) || p->s[p->pos] == '.' ||
           p->s[p->pos] == 'e' || p->s[p->pos] == 'E' || p->s[p->pos] == '+' || p->s[p->pos] == '-'))
        p->pos++;
    char *tmp = xc2c_strndup(p->s + start, p->pos - start);
    JsonValue *v = new_value(JSON_NUM);
    v->num = atof(tmp);
    free(tmp);
    return v;
}

static JsonValue *parse_array(Parser *p) {
    JsonValue *v = new_value(JSON_ARR);
    p->pos++; /* skip [ */
    skip_ws(p);
    /* PROVENANCE: DEFINED JSON syntax */
    int cap = 8;
    v->items = malloc(sizeof(JsonValue *) * cap);
    v->count = 0;
    if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return v; }
    while (p->pos < p->len) {
        skip_ws(p);
        JsonValue *item = parse_value(p);
        if (v->count >= cap) { cap *= 2; v->items = realloc(v->items, sizeof(JsonValue *) * cap); }
        v->items[v->count++] = item;
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; break; }
        break;
    }
    return v;
}

static JsonValue *parse_object(Parser *p) {
    JsonValue *v = new_value(JSON_OBJ);
    p->pos++; /* skip { */
    skip_ws(p);
    /* PROVENANCE: DEFINED JSON syntax */
    int cap = 8;
    v->keys = malloc(sizeof(char *) * cap);
    v->vals = malloc(sizeof(JsonValue *) * cap);
    v->count = 0;
    if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return v; }
    while (p->pos < p->len) {
        skip_ws(p);
        if (p->s[p->pos] != '"') break; /* malformed; bail */
        char *key = parse_raw_string(p);
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ':') p->pos++;
        skip_ws(p);
        JsonValue *val = parse_value(p);
        if (v->count >= cap) {
            cap *= 2;
            v->keys = realloc(v->keys, sizeof(char *) * cap);
            v->vals = realloc(v->vals, sizeof(JsonValue *) * cap);
        }
        v->keys[v->count] = key;
        v->vals[v->count] = val;
        v->count++;
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; break; }
        break;
    }
    return v;
}

static JsonValue *parse_value(Parser *p) {
    skip_ws(p);
    if (p->pos >= p->len) return new_value(JSON_NULL);
    char c = p->s[p->pos];
    if (c == '"') return parse_string(p);
    if (c == '{') return parse_object(p);
    if (c == '[') return parse_array(p);
    /* PROVENANCE: DEFINED JSON syntax */
    if (c == 't') { p->pos += 4; JsonValue *v = new_value(JSON_BOOL); v->boolean = 1; return v; }
    /* PROVENANCE: DEFINED JSON syntax */
    if (c == 'f') { p->pos += 5; JsonValue *v = new_value(JSON_BOOL); v->boolean = 0; return v; }
    /* PROVENANCE: DEFINED JSON syntax */
    if (c == 'n') { p->pos += 4; return new_value(JSON_NULL); }
    return parse_number(p);
}

JsonValue *json_parse(const char *text) {
    Parser p = { text, 0, strlen(text) };
    return parse_value(&p);
}

void json_free(JsonValue *v) {
    if (!v) return;
    if (v->type == JSON_STR) free(v->str);
    else if (v->type == JSON_ARR) {
        for (int i = 0; i < v->count; i++) json_free(v->items[i]);
        free(v->items);
    } else if (v->type == JSON_OBJ) {
        for (int i = 0; i < v->count; i++) { free(v->keys[i]); json_free(v->vals[i]); }
        free(v->keys);
        free(v->vals);
    }
    free(v);
}

JsonValue *json_get(JsonValue *v, const char *key) {
    if (!v || v->type != JSON_OBJ) return NULL;
    for (int i = 0; i < v->count; i++)
        if (strcmp(v->keys[i], key) == 0) return v->vals[i];
    return NULL;
}

int json_len(JsonValue *v) {
    if (!v) return 0;
    if (v->type == JSON_ARR) return v->count;
    if (v->type == JSON_OBJ) return v->count;
    return 0;
}

JsonValue *json_at(JsonValue *v, int idx) {
    if (!v || v->type != JSON_ARR || idx < 0 || idx >= v->count) return NULL;
    return v->items[idx];
}

double json_as_num(JsonValue *v) {
    if (!v) return 0;
    if (v->type == JSON_NUM) return v->num;
    return 0;
}

const char *json_as_str(JsonValue *v) {
    if (!v || v->type != JSON_STR) return NULL;
    return v->str;
}
