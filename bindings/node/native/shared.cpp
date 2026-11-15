// 共享读（零拷贝视图）、准备键、批量读取器。
#include "common.h"

namespace fqn {

static napi_value null_value(napi_env env) {
    napi_value n = nullptr;
    napi_get_null(env, &n);
    return n;
}

static napi_value lease_from(napi_env env, fqdb_store_shared_value &sv) {
    auto *l = new Lease();
    l->owner = sv.owner;
    l->version = sv.row.version;
    l->expires_at_ms = sv.row.expires_at_ms;
    return make_lease_view(env, l, sv.row.value.data, sv.row.value.size);
}

// 同步回调：视图只在回调期间有效，返回后立即 detach 并归还租约。
// 不为 ArrayBuffer 注册 finalizer/wrap，省去 GC 跟踪成本。
static napi_value call_with_view(napi_env env, napi_value fn, fqdb_store_shared_value *sv) {
    napi_value ab = nullptr, view = nullptr, result = nullptr, undefined = nullptr;
    const void *data = sv->row.value.data;
    size_t size = static_cast<size_t>(sv->row.value.size);
    napi_status st;
    if (size == 0) {
        void *tmp;
        st = napi_create_arraybuffer(env, 0, &tmp, &ab);
    } else {
        st = napi_create_external_arraybuffer(env, const_cast<void *>(data), size, nullptr, nullptr, &ab);
    }
    if (st == napi_ok) st = napi_create_typedarray(env, napi_uint8_array, size, ab, 0, &view);
    if (st != napi_ok) {
        fqdb_store_value_close(sv->owner);
        return fail(env, st);
    }
    napi_get_undefined(env, &undefined);
    st = napi_call_function(env, undefined, fn, 1, &view, &result);
    napi_value exception = nullptr;
    bool threw = false;
    if (st == napi_pending_exception) {
        threw = true;
        napi_get_and_clear_last_exception(env, &exception);
    }
    napi_detach_arraybuffer(env, ab);  // 回调保留的视图此后长度为 0，不会悬垂
    fqdb_store_value_close(sv->owner);
    if (threw) {
        napi_throw(env, exception);
        return nullptr;
    }
    if (st != napi_ok) return fail(env, st);
    return result;
}

static napi_value call_with_null(napi_env env, napi_value fn) {
    napi_value n = null_value(env), undefined, result = nullptr;
    napi_get_undefined(env, &undefined);
    napi_status st = napi_call_function(env, undefined, fn, 1, &n, &result);
    if (st != napi_ok) return st == napi_pending_exception ? nullptr : fail(env, st);
    return result;
}

static bool is_function(napi_env env, napi_value v) {
    napi_valuetype t;
    return napi_typeof(env, v, &t) == napi_ok && t == napi_function;
}

// ---------- Store 共享读 ----------
static napi_value store_get_shared(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    Input key;
    if (!key.load(env, argv[1], "key")) return nullptr;
    fqdb_store_shared_value sv;
    std::memset(&sv, 0, sizeof sv);
    fqdb_status st = fqdb_store_get_shared(s, key.slice, &sv);
    if (st == FQDB_NOT_FOUND) return null_value(env);
    if (st != FQDB_OK) return throw_status(env, st, "getShared");
    return lease_from(env, sv);
}

static napi_value store_with_shared(napi_env env, napi_callback_info info) {
    FQN_ARGS(3);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    if (!is_function(env, argv[2])) return throw_type(env, "callback must be a function");
    Input key;
    if (!key.load(env, argv[1], "key")) return nullptr;
    fqdb_store_shared_value sv;
    std::memset(&sv, 0, sizeof sv);
    fqdb_status st = fqdb_store_get_shared(s, key.slice, &sv);
    if (st == FQDB_NOT_FOUND) return call_with_null(env, argv[2]);
    if (st != FQDB_OK) return throw_status(env, st, "withShared");
    return call_with_view(env, argv[2], &sv);
}

// 找出视图背后的 Lease（经 ArrayBuffer 的 napi_wrap）。
static Lease *lease_of(napi_env env, napi_value view) {
    napi_typedarray_type type;
    size_t len;
    void *data;
    napi_value ab;
    size_t off;
    if (napi_get_typedarray_info(env, view, &type, &len, &data, &ab, &off) != napi_ok) {
        throw_type(env, "expected a Uint8Array view returned by the binding");
        return nullptr;
    }
    void *p = nullptr;
    if (napi_unwrap(env, ab, &p) != napi_ok || !p || static_cast<Lease *>(p)->magic != MAGIC_LEASE) {
        throw_type(env, "not a native shared view");
        return nullptr;
    }
    return static_cast<Lease *>(p);
}

// sharedRelease(view) -> boolean（本次调用是否真正释放）。
// 释放后 ArrayBuffer 被 detach：持有的视图及其 subarray 长度变为 0，不会悬垂。
static napi_value shared_release(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    Lease *l = lease_of(env, argv[0]);
    if (!l) return nullptr;
    bool did = !l->released;
    if (did) {
        l->close();
        napi_typedarray_type type;
        size_t len;
        void *data;
        napi_value ab;
        size_t off;
        napi_get_typedarray_info(env, argv[0], &type, &len, &data, &ab, &off);
        napi_detach_arraybuffer(env, ab);
    }
    napi_value r;
    FQN_TRY(napi_get_boolean(env, did, &r));
    return r;
}

// sharedInfo(view) -> [version, expiresAtMs, released]
static napi_value shared_info(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    Lease *l = lease_of(env, argv[0]);
    if (!l) return nullptr;
    napi_value arr, v, e, r;
    FQN_TRY(napi_create_array_with_length(env, 3, &arr));
    FQN_TRY(napi_create_bigint_uint64(env, l->version, &v));
    FQN_TRY(napi_create_bigint_uint64(env, l->expires_at_ms, &e));
    FQN_TRY(napi_get_boolean(env, l->released, &r));
    FQN_TRY(napi_set_element(env, arr, 0, v));
    FQN_TRY(napi_set_element(env, arr, 1, e));
    FQN_TRY(napi_set_element(env, arr, 2, r));
    return arr;
}

// ---------- 准备键 ----------
static void key_finalize(napi_env, void *data, void *) {
    auto *k = static_cast<KeyBox *>(data);
    if (k->key) fqdb_store_key_close(k->key);
    delete k;
}

static napi_value key_prepare(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    Input key;
    if (!key.load(env, argv[1], "key")) return nullptr;
    fqdb_store_key *pk = nullptr;
    fqdb_status st = fqdb_store_key_prepare(s, key.slice, &pk);
    if (st != FQDB_OK) return throw_status(env, st, "prepareKey");
    auto *k = new KeyBox();
    k->key = pk;
    napi_value ext = nullptr;
    napi_status ns = napi_create_external(env, k, key_finalize, nullptr, &ext);
    if (ns != napi_ok) {
        key_finalize(env, k, nullptr);
        return fail(env, ns);
    }
    return ext;
}

static KeyBox *live_key(napi_env env, napi_value v) {
    auto *k = static_cast<KeyBox *>(unwrap_handle(env, v, MAGIC_KEY, "prepared key"));
    if (!k) return nullptr;
    if (!k->key) {
        throw_closed(env, "prepared key");
        return nullptr;
    }
    return k;
}

static napi_value key_close(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    auto *k = static_cast<KeyBox *>(unwrap_handle(env, argv[0], MAGIC_KEY, "prepared key"));
    if (!k) return nullptr;
    if (k->key) {
        fqdb_store_key_close(k->key);
        k->key = nullptr;
    }
    return nullptr;
}

static napi_value key_get_shared(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    KeyBox *k = live_key(env, argv[0]);
    if (!k) return nullptr;
    fqdb_store_shared_value sv;
    std::memset(&sv, 0, sizeof sv);
    fqdb_status st = fqdb_store_key_get_shared(k->key, &sv);
    if (st == FQDB_NOT_FOUND) return null_value(env);
    if (st != FQDB_OK) return throw_status(env, st, "getShared");
    return lease_from(env, sv);
}

static napi_value key_with_shared(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    KeyBox *k = live_key(env, argv[0]);
    if (!k) return nullptr;
    if (!is_function(env, argv[1])) return throw_type(env, "callback must be a function");
    fqdb_store_shared_value sv;
    std::memset(&sv, 0, sizeof sv);
    fqdb_status st = fqdb_store_key_get_shared(k->key, &sv);
    if (st == FQDB_NOT_FOUND) return call_with_null(env, argv[1]);
    if (st != FQDB_OK) return throw_status(env, st, "withShared");
    return call_with_view(env, argv[1], &sv);
}

// keyGet(key) -> Buffer | null（复制一次）
static napi_value key_get(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    KeyBox *k = live_key(env, argv[0]);
    if (!k) return nullptr;
    fqdb_store_value *v = nullptr;
    fqdb_status st = fqdb_store_key_get_view(k->key, &v);
    if (st == FQDB_NOT_FOUND) return null_value(env);
    if (st != FQDB_OK) return throw_status(env, st, "get");
    fqdb_slice d = fqdb_store_value_data(v);
    napi_value r = copy_to_buffer(env, d.data, d.size);
    fqdb_store_value_close(v);
    return r;
}

// keyGetInto(key, target) -> 写入字节数；未命中 -1；容量不足抛 BUFFER_TOO_SMALL
static napi_value key_get_into(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    KeyBox *k = live_key(env, argv[0]);
    if (!k) return nullptr;
    napi_typedarray_type type;
    size_t len;
    void *data;
    if (napi_get_typedarray_info(env, argv[1], &type, &len, &data, nullptr, nullptr) != napi_ok ||
        (type != napi_uint8_array && type != napi_int8_array && type != napi_uint8_clamped_array))
        return throw_type(env, "target must be a Uint8Array");
    uint64_t required = 0;
    fqdb_status st = fqdb_store_key_get_into(k->key, data, len, &required);
    napi_value r;
    if (st == FQDB_NOT_FOUND) {
        FQN_TRY(napi_create_int32(env, -1, &r));
        return r;
    }
    if (st != FQDB_OK) return throw_status(env, st, "getInto");
    FQN_TRY(napi_create_double(env, static_cast<double>(required), &r));
    return r;
}

// keyPeekByte(key, index) -> 0..255，未命中或越界 -1；index<0 从末尾算。零拷贝、无 JS 对象分配。
static napi_value key_peek_byte(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    KeyBox *k = live_key(env, argv[0]);
    if (!k) return nullptr;
    int64_t index = 0;
    FQN_TRY(napi_get_value_int64(env, argv[1], &index));
    fqdb_store_shared_value sv;
    fqdb_status st = fqdb_store_key_get_shared(k->key, &sv);
    int32_t out = -1;
    if (st == FQDB_OK) {
        int64_t n = static_cast<int64_t>(sv.row.value.size);
        if (index < 0) index += n;
        if (index >= 0 && index < n) out = static_cast<const uint8_t *>(sv.row.value.data)[index];
        fqdb_store_value_close(sv.owner);
    } else if (st != FQDB_NOT_FOUND) {
        return throw_status(env, st, "peekByte");
    }
    napi_value r;
    FQN_TRY(napi_create_int32(env, out, &r));
    return r;
}

// ---------- 批量读取器 ----------
static void reader_invalidate(napi_env env, ReaderBox *r) {
    for (napi_ref ref : r->views) {
        napi_value ab = nullptr;
        if (napi_get_reference_value(env, ref, &ab) == napi_ok && ab) napi_detach_arraybuffer(env, ab);
        napi_delete_reference(env, ref);
    }
    r->views.clear();
}

static void reader_destroy(napi_env env, ReaderBox *r) {
    reader_invalidate(env, r);
    if (r->reader) fqdb_store_reader_close(r->reader);
    r->reader = nullptr;
    r->rows = nullptr;
    r->count = 0;
    if (r->info_ref) napi_delete_reference(env, r->info_ref);
    r->info_ref = nullptr;
    r->info = nullptr;
}

static void reader_finalize(napi_env env, void *data, void *) {
    auto *r = static_cast<ReaderBox *>(data);
    reader_destroy(env, r);
    delete r;
}

static ReaderBox *live_reader(napi_env env, napi_value v) {
    auto *r = static_cast<ReaderBox *>(unwrap_handle(env, v, MAGIC_READER, "reader"));
    if (!r) return nullptr;
    if (!r->reader) {
        throw_closed(env, "reader");
        return nullptr;
    }
    return r;
}

// readerCreate(store, keys: (Uint8Array|string)[], info: Uint32Array(2*n)) -> handle
static napi_value reader_create(napi_env env, napi_callback_info info) {
    FQN_ARGS(3);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    bool is_array = false;
    FQN_TRY(napi_is_array(env, argv[1], &is_array));
    if (!is_array) return throw_type(env, "keys must be an array");
    uint32_t n = 0;
    FQN_TRY(napi_get_array_length(env, argv[1], &n));
    if (n == 0 || n > 4096) return throw_range(env, "a reader needs 1..4096 keys");
    napi_typedarray_type type;
    size_t len;
    void *data;
    if (napi_get_typedarray_info(env, argv[2], &type, &len, &data, nullptr, nullptr) != napi_ok ||
        type != napi_uint32_array || len < 2ull * n)
        return throw_type(env, "info must be a Uint32Array with 2 entries per key");
    std::deque<Input> inputs;
    std::vector<fqdb_slice> slices(n);
    for (uint32_t i = 0; i < n; ++i) {
        napi_value k;
        FQN_TRY(napi_get_element(env, argv[1], i, &k));
        inputs.emplace_back();
        if (!inputs.back().load(env, k, "key")) return nullptr;
        slices[i] = inputs.back().slice;
    }
    fqdb_store_reader *rd = nullptr;
    fqdb_status st = fqdb_store_reader_create(s, slices.data(), n, &rd);
    if (st != FQDB_OK) return throw_status(env, st, "prepareReader");
    auto *box = new ReaderBox();
    box->reader = rd;
    box->capacity = n;
    box->info = static_cast<uint32_t *>(data);
    napi_status ns = napi_create_reference(env, argv[2], 1, &box->info_ref);  // 保持 info 数组存活
    napi_value ext = nullptr;
    if (ns == napi_ok) ns = napi_create_external(env, box, reader_finalize, nullptr, &ext);
    if (ns != napi_ok) {
        reader_finalize(env, box, nullptr);
        return fail(env, ns);
    }
    return ext;
}

// readerRead(reader) -> 行数；同时填充 info[2i]=found, info[2i+1]=size
static napi_value reader_read(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    ReaderBox *r = live_reader(env, argv[0]);
    if (!r) return nullptr;
    reader_invalidate(env, r);
    r->rows = nullptr;
    r->count = 0;
    const fqdb_store_result_row *rows = nullptr;
    uint32_t count = 0;
    fqdb_status st = fqdb_store_reader_read(r->reader, &rows, &count);
    if (st != FQDB_OK) return throw_status(env, st, "read");
    if (count > r->capacity) count = r->capacity;
    r->rows = rows;
    r->count = count;
    uint32_t *out = r->info;
    for (uint32_t i = 0; i < count; ++i) {
        out[2 * i] = rows[i].found ? 1u : 0u;
        out[2 * i + 1] = rows[i].found ? static_cast<uint32_t>(rows[i].value.size) : 0u;
    }
    napi_value res;
    FQN_TRY(napi_create_uint32(env, count, &res));
    return res;
}

static const fqdb_store_result_row *reader_row(napi_env env, ReaderBox *r, napi_value idx) {
    uint32_t i = 0;
    if (napi_get_value_uint32(env, idx, &i) != napi_ok) {
        throw_type(env, "row index must be a non-negative integer");
        return nullptr;
    }
    if (!r->rows || i >= r->count) {
        throw_range(env, "row index out of range (call read() first)");
        return nullptr;
    }
    return &r->rows[i];
}

// readerValue(reader, i) -> Uint8Array | null；视图有效到下一次 read/release/close
static napi_value reader_value(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    ReaderBox *r = live_reader(env, argv[0]);
    if (!r) return nullptr;
    const fqdb_store_result_row *row = reader_row(env, r, argv[1]);
    if (!row) return nullptr;
    if (!row->found) return null_value(env);
    napi_value ab, view;
    size_t size = static_cast<size_t>(row->value.size);
    if (size == 0) {
        void *tmp;
        FQN_TRY(napi_create_arraybuffer(env, 0, &tmp, &ab));
    } else {
        FQN_TRY(napi_create_external_arraybuffer(env, const_cast<void *>(row->value.data), size, nullptr, nullptr, &ab));
        napi_ref ref;
        FQN_TRY(napi_create_reference(env, ab, 0, &ref));  // 弱引用：用于下一次 read 时 detach
        r->views.push_back(ref);
    }
    FQN_TRY(napi_create_typedarray(env, napi_uint8_array, size, ab, 0, &view));
    return view;
}

// readerCopy(reader, i, target) -> 复制的字节数；未命中 -1
static napi_value reader_copy(napi_env env, napi_callback_info info) {
    FQN_ARGS(3);
    ReaderBox *r = live_reader(env, argv[0]);
    if (!r) return nullptr;
    const fqdb_store_result_row *row = reader_row(env, r, argv[1]);
    if (!row) return nullptr;
    napi_typedarray_type type;
    size_t len;
    void *data;
    if (napi_get_typedarray_info(env, argv[2], &type, &len, &data, nullptr, nullptr) != napi_ok ||
        (type != napi_uint8_array && type != napi_int8_array && type != napi_uint8_clamped_array))
        return throw_type(env, "target must be a Uint8Array");
    napi_value res;
    if (!row->found) {
        FQN_TRY(napi_create_int32(env, -1, &res));
        return res;
    }
    if (row->value.size > len) return throw_range(env, "target is too small for the value");
    if (row->value.size) std::memcpy(data, row->value.data, static_cast<size_t>(row->value.size));
    FQN_TRY(napi_create_double(env, static_cast<double>(row->value.size), &res));
    return res;
}

// readerMeta(reader, i) -> [version, expiresAtMs]
static napi_value reader_meta(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    ReaderBox *r = live_reader(env, argv[0]);
    if (!r) return nullptr;
    const fqdb_store_result_row *row = reader_row(env, r, argv[1]);
    if (!row) return nullptr;
    napi_value arr, v, e;
    FQN_TRY(napi_create_array_with_length(env, 2, &arr));
    FQN_TRY(napi_create_bigint_uint64(env, row->version, &v));
    FQN_TRY(napi_create_bigint_uint64(env, row->expires_at_ms, &e));
    FQN_TRY(napi_set_element(env, arr, 0, v));
    FQN_TRY(napi_set_element(env, arr, 1, e));
    return arr;
}

static napi_value reader_release(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    ReaderBox *r = live_reader(env, argv[0]);
    if (!r) return nullptr;
    reader_invalidate(env, r);
    r->rows = nullptr;
    r->count = 0;
    fqdb_store_reader_release(r->reader);
    return nullptr;
}

static napi_value reader_close(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    auto *r = static_cast<ReaderBox *>(unwrap_handle(env, argv[0], MAGIC_READER, "reader"));
    if (!r) return nullptr;
    reader_destroy(env, r);
    return nullptr;
}

napi_status register_shared(napi_env env, napi_value exports) {
    struct Entry {
        const char *name;
        napi_callback cb;
    } table[] = {
        {"storeGetShared", store_get_shared},
        {"storeWithShared", store_with_shared},
        {"sharedRelease", shared_release},
        {"sharedInfo", shared_info},
        {"keyPrepare", key_prepare},
        {"keyClose", key_close},
        {"keyGetShared", key_get_shared},
        {"keyWithShared", key_with_shared},
        {"keyGet", key_get},
        {"keyGetInto", key_get_into},
        {"keyPeekByte", key_peek_byte},
        {"readerCreate", reader_create},
        {"readerRead", reader_read},
        {"readerValue", reader_value},
        {"readerCopy", reader_copy},
        {"readerMeta", reader_meta},
        {"readerRelease", reader_release},
        {"readerClose", reader_close},
    };
    for (const auto &e : table) {
        napi_status st = set_function(env, exports, e.name, e.cb);
        if (st != napi_ok) return st;
    }
    return napi_ok;
}

}  // namespace fqn
