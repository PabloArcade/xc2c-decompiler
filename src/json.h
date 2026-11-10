#ifndef JSON_H
#define JSON_H

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ } JsonType;

typedef struct JsonValue JsonValue;

struct JsonValue {
    JsonType type;
    int boolean;
    double num;
    char *str;
    /* array */
    JsonValue **items;
    int count;
    /* object (parallel arrays) */
    char **keys;
    JsonValue **vals;
};

/* Parses a whole JSON document from a NUL-terminated string.
 * Returns NULL on parse error. */
JsonValue *json_parse(const char *text);

void json_free(JsonValue *v);

/* Object field lookup; returns NULL if key not present or v is not an object. */
JsonValue *json_get(JsonValue *v, const char *key);

/* Array helpers */
int json_len(JsonValue *v);
JsonValue *json_at(JsonValue *v, int idx);

/* Scalar accessors (caller must know the expected type) */
double json_as_num(JsonValue *v);
const char *json_as_str(JsonValue *v); /* NULL if not a string */

#endif
