#ifndef FENGQUDB_ENTITY_TYPES_H
#define FENGQUDB_ENTITY_TYPES_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
/* Optional bounded C entity fields; no database or protobuf runtime dependency. */
typedef struct { const char *data; size_t size; } fqdb_text;
typedef struct { const uint8_t *data; size_t size; } fqdb_bytes;
static inline fqdb_text fqdb_text_from_cstr(const char *value) {
    fqdb_text result = {value, value ? strlen(value) : 0}; return result;
}
#endif
