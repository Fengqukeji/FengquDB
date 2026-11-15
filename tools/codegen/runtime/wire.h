#ifndef FQDB_GENERATED_WIRE_V1_H
#define FQDB_GENERATED_WIRE_V1_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>
#ifdef __cplusplus
#define FQG_TYPE(name) ::name
#else
#define FQG_TYPE(name) name
#endif
/* Borrowed byte/string view. Decode and key views live as long as their input. */
typedef struct { const uint8_t *data; size_t size; } fqg_slice;
typedef struct { uint8_t *data; size_t size, capacity; int ok; } fqg_writer;
typedef struct { const uint8_t *data; size_t size, pos; int ok; } fqg_reader;
static inline int fqg_utf8(fqg_slice s) {
    size_t p = 0;
    if (s.size && !s.data) return 0;
    while (p < s.size) {
        uint32_t c = s.data[p++], min = 0; unsigned n = 0;
        if (c < 128) continue;
        if (c >= 0xc2 && c <= 0xdf) { c &= 31; n = 1; min = 128; }
        else if (c >= 0xe0 && c <= 0xef) { c &= 15; n = 2; min = 2048; }
        else if (c >= 0xf0 && c <= 0xf4) { c &= 7; n = 3; min = 65536; }
        else return 0;
        if (n > s.size - p) return 0;
        while (n--) { uint8_t b = s.data[p++]; if ((b & 0xc0) != 0x80) return 0; c = (c << 6) | (b & 63); }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0;
    }
    return 1;
}
static inline void fqg_raw(fqg_writer *w, const void *data, size_t n) {
    if (!w->ok || n > w->capacity - w->size || (n && !data)) { w->ok = 0; return; }
    if (w->data && n) memcpy(w->data + w->size, data, n);
    w->size += n;
}
static inline void fqg_u(fqg_writer *w, uint64_t v) {
    uint8_t buf[10]; size_t n = 0;
    while (v >= 128) { buf[n++] = (uint8_t)((v & 127) | 128); v >>= 7; }
    buf[n++] = (uint8_t)v; fqg_raw(w, buf, n);
}
static inline void fqg_fixed(fqg_writer *w, uint64_t v, unsigned n) {
    uint8_t buf[8]; unsigned i;
    for (i = 0; i < n; ++i) buf[i] = (uint8_t)(v >> (i * 8));
    fqg_raw(w, buf, n);
}
static inline uint64_t fqg_z(int64_t v) { return ((uint64_t)v << 1) ^ (v < 0 ? UINT64_MAX : 0); }
static inline int64_t fqg_unz(uint64_t v) { return (v & 1) ? -(int64_t)(v >> 1) - 1 : (int64_t)(v >> 1); }
static inline uint64_t fqg_read_u(fqg_reader *r) {
    uint64_t v = 0; unsigned i;
    for (i = 0; i < 10 && r->ok; ++i) {
        uint8_t b;
        if (r->pos >= r->size) break;
        b = r->data[r->pos++];
        if (i == 9 && b > 1) break;
        v |= (uint64_t)(b & 127) << (i * 7);
        if (b < 128) return v;
    }
    r->ok = 0; return 0;
}
static inline uint32_t fqg_tag(fqg_reader *r) {
    uint64_t t = fqg_read_u(r);
    if (t > UINT32_MAX || !(t >> 3) || (t & 7) > 5) r->ok = 0;
    return (uint32_t)t;
}
static inline fqg_slice fqg_take(fqg_reader *r, uint64_t n) {
    fqg_slice s = {NULL, 0};
    if (!r->ok || n > r->size - r->pos) { r->ok = 0; return s; }
    s.data = r->data ? r->data + r->pos : NULL; s.size = (size_t)n; r->pos += (size_t)n; return s;
}
static inline fqg_slice fqg_read_b(fqg_reader *r) { uint64_t n = fqg_read_u(r); return fqg_take(r, n); }
static inline uint64_t fqg_read_fixed(fqg_reader *r, unsigned n) {
    fqg_slice s = fqg_take(r, n); uint64_t v = 0; unsigned i;
    if (r->ok) for (i = 0; i < n; ++i) v |= (uint64_t)s.data[i] << (8 * i);
    return v;
}
static inline void fqg_skip(fqg_reader *r, uint32_t tag, unsigned depth) {
    switch (tag & 7) {
    case 0: (void)fqg_read_u(r); return;
    case 1: (void)fqg_take(r, 8); return;
    case 2: (void)fqg_read_b(r); return;
    case 5: (void)fqg_take(r, 4); return;
    case 3:
        if (depth >= 64) break;
        while (r->ok && r->pos < r->size) {
            uint32_t inner = fqg_tag(r);
            if ((inner & 7) == 4) { if ((inner >> 3) != (tag >> 3)) r->ok = 0; return; }
            fqg_skip(r, inner, depth + 1);
        }
        break;
    default: break;
    }
    r->ok = 0;
}
/* Only used for unknown fields: known-only messages never rescan input. */
static inline void fqg_unknown(fqg_writer *w, fqg_slice source, int (*known)(uint32_t)) {
    fqg_reader r = {source.data, source.size, 0, source.size == 0 || source.data != NULL};
    while (r.ok && r.pos < r.size) {
        size_t begin = r.pos; uint32_t t = fqg_tag(&r); fqg_skip(&r, t, 0);
        if (r.ok && !known(t)) fqg_raw(w, r.data + begin, r.pos - begin);
    }
    if (!r.ok) w->ok = 0;
}
static inline void fqg_key_number(fqg_writer *w, uint64_t v, int negative) {
    uint8_t buf[21]; size_t n = sizeof(buf);
    do { buf[--n] = (uint8_t)('0' + v % 10); v /= 10; } while (v);
    if (negative) buf[--n] = '-';
    fqg_raw(w, buf + n, sizeof(buf) - n);
}
#endif
