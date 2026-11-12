/* cgo 辅助函数。每个函数把"多个 C 调用 + 输出参数"合成一次 cgo 往返，
 * 并以返回值传出结果，使 Go 侧不必传入指向 Go 内存的输出指针（避免逃逸分配）。 */
#ifndef FQDB_GO_H
#define FQDB_GO_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fengqudb_store.h"

typedef struct {
    fqdb_status st;
    fqdb_store_value *owner;
    const void *data;
    uint64_t size, version, expires;
} fqg_shared;

typedef struct {
    fqdb_status st;
    void *data;
    uint64_t size;
} fqg_buf;

typedef struct {
    fqdb_status st;
    void *ptr;
} fqg_handle;

typedef struct {
    fqdb_status st;
    uint64_t value;
} fqg_u64;

typedef struct {
    fqdb_status st;
    const fqdb_store_result_row *rows;
    uint32_t count;
} fqg_rows;

static inline fqg_shared fqg_finish_shared(fqdb_status st, const fqdb_store_shared_value *sv) {
    fqg_shared r;
    r.st = st;
    r.owner = NULL;
    r.data = NULL;
    r.size = r.version = r.expires = 0;
    if (st != FQDB_OK) return r;
    if (!sv->row.found) {
        if (sv->owner) fqdb_store_value_close(sv->owner);
        r.st = FQDB_NOT_FOUND;
        return r;
    }
    r.owner = sv->owner;
    r.data = sv->row.value.data;
    r.size = sv->row.value.size;
    r.version = sv->row.version;
    r.expires = sv->row.expires_at_ms;
    return r;
}

static inline fqg_shared fqg_store_get_shared(fqdb_store *s, const void *k, uint64_t kn) {
    fqdb_slice key = {k, kn};
    fqdb_store_shared_value sv;
    fqdb_status st = fqdb_store_get_shared(s, key, &sv);
    return fqg_finish_shared(st, &sv);
}
static inline fqg_shared fqg_key_get_shared(fqdb_store_key *k) {
    fqdb_store_shared_value sv;
    fqdb_status st = fqdb_store_key_get_shared(k, &sv);
    return fqg_finish_shared(st, &sv);
}
static inline fqg_shared fqg_snapshot_get_shared(const fqdb_store_snapshot *snap, const void *k, uint64_t kn) {
    fqdb_slice key = {k, kn};
    fqdb_store_shared_value sv;
    fqdb_status st = fqdb_store_snapshot_get_shared(snap, key, &sv);
    return fqg_finish_shared(st, &sv);
}

/* 复制读到调用方缓冲：一次往返完成"取视图 + memcpy + 归还租约"。
 * size > cap 时不复制，调用方扩容后重试。 */
static inline fqg_u64 fqg_get_into(fqdb_store *s, const void *k, uint64_t kn, void *dst, uint64_t cap) {
    fqg_u64 r;
    fqdb_slice key = {k, kn};
    fqdb_store_value *v = NULL;
    r.value = 0;
    r.st = fqdb_store_get_view(s, key, &v);
    if (r.st != FQDB_OK) return r;
    fqdb_slice d = fqdb_store_value_data(v);
    r.value = d.size;
    if (d.size <= cap && d.size > 0) memcpy(dst, d.data, d.size);
    fqdb_store_value_close(v);
    return r;
}

static inline fqg_buf fqg_buf_ret(fqdb_status st, fqdb_buffer *b) {
    fqg_buf r;
    r.st = st;
    r.data = st == FQDB_OK ? b->data : NULL;
    r.size = st == FQDB_OK ? b->size : 0;
    return r;
}
static inline void fqg_buffer_free(void *data, uint64_t size) {
    fqdb_buffer b = {data, size};
    fqdb_buffer_free(&b);
}
static inline fqg_buf fqg_txn_get(fqdb_store_txn *t, const void *k, uint64_t kn) {
    fqdb_slice key = {k, kn};
    fqdb_buffer b = {NULL, 0};
    fqdb_status st = fqdb_store_txn_get(t, key, &b);
    return fqg_buf_ret(st, &b);
}
static inline fqg_buf fqg_snapshot_get(const fqdb_store_snapshot *s, const void *k, uint64_t kn) {
    fqdb_slice key = {k, kn};
    fqdb_buffer b = {NULL, 0};
    fqdb_status st = fqdb_store_snapshot_get(s, key, &b);
    return fqg_buf_ret(st, &b);
}
static inline fqg_buf fqg_select(fqdb_store *s, const void *req, uint64_t n) {
    fqdb_slice r = {req, n};
    fqdb_buffer b = {NULL, 0};
    fqdb_status st = fqdb_store_select(s, r, &b);
    return fqg_buf_ret(st, &b);
}
static inline fqg_buf fqg_txn_execute(fqdb_store_txn *t, const void *req, uint64_t n) {
    fqdb_slice r = {req, n};
    fqdb_buffer b = {NULL, 0};
    fqdb_status st = fqdb_store_txn_execute(t, r, &b);
    return fqg_buf_ret(st, &b);
}

static inline fqg_handle fqg_open(const char *dir, const fqdb_store_options *o) {
    fqg_handle h;
    fqdb_store *s = NULL;
    h.st = fqdb_store_open(dir, o, &s);
    h.ptr = h.st == FQDB_OK ? s : NULL;
    return h;
}
static inline fqg_handle fqg_key_prepare(fqdb_store *s, const void *k, uint64_t kn) {
    fqg_handle h;
    fqdb_slice key = {k, kn};
    fqdb_store_key *pk = NULL;
    h.st = fqdb_store_key_prepare(s, key, &pk);
    h.ptr = h.st == FQDB_OK ? pk : NULL;
    return h;
}
static inline fqg_handle fqg_reader_create(fqdb_store *s, const fqdb_slice *keys, uint32_t n) {
    fqg_handle h;
    fqdb_store_reader *r = NULL;
    h.st = fqdb_store_reader_create(s, keys, n, &r);
    h.ptr = h.st == FQDB_OK ? r : NULL;
    return h;
}
static inline fqg_rows fqg_reader_read(fqdb_store_reader *r) {
    fqg_rows o;
    o.rows = NULL;
    o.count = 0;
    o.st = fqdb_store_reader_read(r, &o.rows, &o.count);
    if (o.st != FQDB_OK) { o.rows = NULL; o.count = 0; }
    return o;
}
static inline fqg_handle fqg_reader_acquire(fqdb_store_reader *r) {
    fqg_handle h;
    fqdb_store_results *res = NULL;
    h.st = fqdb_store_reader_acquire(r, &res);
    h.ptr = h.st == FQDB_OK ? res : NULL;
    return h;
}
static inline fqg_rows fqg_results_rows(fqdb_store_results *res) {
    fqg_rows o;
    o.st = FQDB_OK;
    o.rows = fqdb_store_results_data(res);
    o.count = fqdb_store_results_count(res);
    return o;
}
static inline fqg_handle fqg_snapshot_create(fqdb_store *s) {
    fqg_handle h;
    fqdb_store_snapshot *sn = NULL;
    h.st = fqdb_store_snapshot_create(s, &sn);
    h.ptr = h.st == FQDB_OK ? sn : NULL;
    return h;
}
static inline fqg_handle fqg_txn_begin(fqdb_store *s, uint64_t timeout_ms) {
    fqg_handle h;
    fqdb_store_txn *t = NULL;
    h.st = fqdb_store_txn_begin(s, timeout_ms, &t);
    h.ptr = h.st == FQDB_OK ? t : NULL;
    return h;
}
static inline fqg_u64 fqg_txn_commit(fqdb_store_txn *t) {
    fqg_u64 r;
    r.value = 0;
    r.st = fqdb_store_txn_commit(t, &r.value, 0);
    return r;
}
static inline fqg_u64 fqg_put(fqdb_store *s, const void *k, uint64_t kn, const void *v, uint64_t vn, uint64_t exp) {
    fqg_u64 r;
    fqdb_slice key = {k, kn}, val = {v, vn};
    fqdb_store_atomic_info info;
    memset(&info, 0, sizeof info);
    r.st = fqdb_store_put(s, key, val, exp, &info, 0);
    r.value = info.version;
    return r;
}
static inline fqg_u64 fqg_delete(fqdb_store *s, const void *k, uint64_t kn) {
    fqg_u64 r;
    fqdb_slice key = {k, kn};
    fqdb_store_atomic_info info;
    memset(&info, 0, sizeof info);
    r.st = fqdb_store_delete(s, key, &info, 0);
    r.value = info.version;
    return r;
}
static inline fqdb_status fqg_txn_put(fqdb_store_txn *t, const void *k, uint64_t kn, const void *v, uint64_t vn, uint64_t exp) {
    fqdb_slice key = {k, kn}, val = {v, vn};
    return fqdb_store_txn_put(t, key, val, exp);
}
static inline fqdb_status fqg_txn_delete(fqdb_store_txn *t, const void *k, uint64_t kn) {
    fqdb_slice key = {k, kn};
    return fqdb_store_txn_delete(t, key);
}
static inline fqdb_status fqg_index_define_compound(fqdb_store *s, const void *name, uint64_t nn,
                                                     const void *prefix, uint64_t pn,
                                                     const fqdb_store_index_field *f, uint32_t count,
                                                     uint32_t flags) {
    fqdb_slice n = {name, nn}, p = {prefix, pn};
    return fqdb_store_index_define_compound(s, n, p, f, count, flags, NULL, NULL);
}
static inline fqdb_status fqg_index_drop(fqdb_store *s, const void *name, uint64_t nn) {
    fqdb_slice n = {name, nn};
    return fqdb_store_index_drop(s, n, NULL, NULL);
}
#endif
