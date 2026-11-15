// Store / 事务 / 快照 / 索引 / 查询入口。
#include "common.h"

namespace fqn {

// ---------- 通用小工具 ----------
static bool set_big(napi_env env, napi_value obj, const char *name, uint64_t v) {
    napi_value b;
    return napi_create_bigint_uint64(env, v, &b) == napi_ok && napi_set_named_property(env, obj, name, b) == napi_ok;
}

static bool opt_u64(napi_env env, napi_value obj, const char *name, uint64_t def, uint64_t *out) {
    napi_value v;
    if (napi_get_named_property(env, obj, name, &v) != napi_ok) return false;
    return get_u64(env, v, def, out, name);
}

// 复制读取结果：优先走 get_view（只复制一次载荷）；固定预算不足时退回原复制路径。
static napi_value copy_value(napi_env env, fqdb_store *store, fqdb_slice key, const char *op) {
    fqdb_store_value *v = nullptr;
    fqdb_status st = fqdb_store_get_view(store, key, &v);
    if (st == FQDB_OK) {
        fqdb_slice d = fqdb_store_value_data(v);
        napi_value r = copy_to_buffer(env, d.data, d.size);
        fqdb_store_value_close(v);
        return r;
    }
    if (st == FQDB_NOT_FOUND) {
        napi_value n;
        FQN_TRY(napi_get_null(env, &n));
        return n;
    }
    if (st == FQDB_LIMIT) {
        fqdb_buffer b{nullptr, 0};
        st = fqdb_store_get(store, key, &b);
        if (st == FQDB_OK) {
            napi_value r = copy_to_buffer(env, b.data, b.size);
            fqdb_buffer_free(&b);
            return r;
        }
        if (st == FQDB_NOT_FOUND) {
            napi_value n;
            FQN_TRY(napi_get_null(env, &n));
            return n;
        }
    }
    return throw_status(env, st, op);
}

// ---------- Store 生命周期 ----------
static void store_ref_finalize(napi_env, void *data, void *) {
    auto *s = static_cast<StoreRef *>(data);
    if (!s->closed) box_release(s->box);  // 用户没有 close()：最后一个引用归零时才真正关闭
    delete s;
}

static napi_value wrap_store_ref(napi_env env, StoreBox *box) {
    auto *ref = new StoreRef();
    ref->box = box;
    napi_value ext;
    napi_status st = napi_create_external(env, ref, store_ref_finalize, nullptr, &ext);
    if (st != napi_ok) {
        box_release(box);
        delete ref;
        return fail(env, st);
    }
    return ext;
}

// open(path, options?) -> store handle
static napi_value store_open(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    Input path;
    if (!path.load(env, argv[0], "path")) return nullptr;
    std::string dir(static_cast<const char *>(path.slice.data), path.slice.size);
    fqdb_store_options o;
    std::memset(&o, 0, sizeof o);
    const fqdb_store_options *options = nullptr;
    napi_valuetype vt;
    FQN_TRY(napi_typeof(env, argv[1], &vt));
    if (vt == napi_object) {
        uint64_t durability, cache, delta, writer, txn, all, pins, snaps;
        if (!opt_u64(env, argv[1], "durability", 0, &durability) ||
            !opt_u64(env, argv[1], "cacheBytes", 64ull << 20, &cache) ||
            !opt_u64(env, argv[1], "deltaBytes", 4ull << 20, &delta) ||
            !opt_u64(env, argv[1], "writerBytes", 32ull << 20, &writer) ||
            !opt_u64(env, argv[1], "transactionBytes", 64ull << 20, &txn) ||
            !opt_u64(env, argv[1], "allTransactionBytes", 256ull << 20, &all) ||
            !opt_u64(env, argv[1], "pinnedBytes", 128ull << 20, &pins) ||
            !opt_u64(env, argv[1], "maxSnapshots", 1024, &snaps))
            return nullptr;
        napi_value v;
        bool create = true, segmented = false;
        FQN_TRY(napi_get_named_property(env, argv[1], "create", &v));
        create = get_bool(env, v, true);
        FQN_TRY(napi_get_named_property(env, argv[1], "segmented", &v));
        segmented = get_bool(env, v, false);
        o.struct_size = sizeof o;
        o.abi_version = 1;
        o.durability = static_cast<uint32_t>(durability);
        o.flags = (create ? static_cast<uint32_t>(FQDB_STORE_CREATE) : 0u) |
                  (segmented ? static_cast<uint32_t>(FQDB_STORE_SEGMENTED) : 0u);
        o.cache_bytes = cache;
        o.delta_bytes = delta;
        o.writer_bytes = writer;
        o.transaction_bytes = txn;
        o.all_transaction_bytes = all;
        o.pinned_bytes = pins;
        o.max_snapshots = snaps;
        options = &o;
    }
    fqdb_store *store = nullptr;
    fqdb_status st = fqdb_store_open(dir.c_str(), options, &store);
    if (st != FQDB_OK) return throw_status(env, st, "open");
    auto *box = new StoreBox();
    box->store = store;
    return wrap_store_ref(env, box);
}

static napi_value store_close(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    auto *s = static_cast<StoreRef *>(unwrap_handle(env, argv[0], MAGIC_STORE, "store"));
    if (!s) return nullptr;
    if (!s->closed) {
        s->closed = true;
        box_release(s->box);
    }
    return nullptr;
}

static napi_value store_is_closed(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    auto *s = static_cast<StoreRef *>(unwrap_handle(env, argv[0], MAGIC_STORE, "store"));
    if (!s) return nullptr;
    napi_value r;
    FQN_TRY(napi_get_boolean(env, s->closed, &r));
    return r;
}

// share(store) -> bigint token（给 worker_threads 用；token 必须被 attach 或 discard 恰好一次）
static napi_value store_share(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    if (!live_store(env, argv[0])) return nullptr;
    auto *s = static_cast<StoreRef *>(unwrap_handle(env, argv[0], MAGIC_STORE, "store"));
    s->box->refs.fetch_add(1, std::memory_order_relaxed);
    return make_bigint(env, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(s->box)));
}
static napi_value store_attach(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    bool lossless = false;
    uint64_t token = 0;
    FQN_TRY(napi_get_value_bigint_uint64(env, argv[0], &token, &lossless));
    if (!token) return throw_type(env, "invalid store token");
    return wrap_store_ref(env, reinterpret_cast<StoreBox *>(static_cast<uintptr_t>(token)));
}
static napi_value store_discard(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    bool lossless = false;
    uint64_t token = 0;
    FQN_TRY(napi_get_value_bigint_uint64(env, argv[0], &token, &lossless));
    if (token) box_release(reinterpret_cast<StoreBox *>(static_cast<uintptr_t>(token)));
    return nullptr;
}

// ---------- 单键读写 ----------
// put(store, key, value, expiresAtMs?, forceReliable?) -> bigint version
static napi_value store_put(napi_env env, napi_callback_info info) {
    FQN_ARGS(5);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    Input key, value;
    uint64_t expires;
    if (!key.load(env, argv[1], "key") || !value.load(env, argv[2], "value") ||
        !get_u64(env, argv[3], 0, &expires, "expiresAtMs"))
        return nullptr;
    fqdb_store_atomic_info ai;
    std::memset(&ai, 0, sizeof ai);
    fqdb_status st = fqdb_store_put(s, key.slice, value.slice, expires, &ai, get_bool(env, argv[4], false) ? 1u : 0u);
    if (st != FQDB_OK) return throw_status(env, st, "put");
    return make_bigint(env, ai.version);
}

// delete(store, key, forceReliable?) -> boolean（是否删除了存在的键）
static napi_value store_delete(napi_env env, napi_callback_info info) {
    FQN_ARGS(3);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    Input key;
    if (!key.load(env, argv[1], "key")) return nullptr;
    fqdb_store_atomic_info ai;
    std::memset(&ai, 0, sizeof ai);
    fqdb_status st = fqdb_store_delete(s, key.slice, &ai, get_bool(env, argv[2], false) ? 1u : 0u);
    napi_value r;
    if (st == FQDB_NOT_FOUND) {
        FQN_TRY(napi_get_boolean(env, false, &r));
        return r;
    }
    if (st != FQDB_OK) return throw_status(env, st, "delete");
    FQN_TRY(napi_get_boolean(env, ai.changed != 0, &r));
    return r;
}

// get(store, key) -> Buffer | null
static napi_value store_get(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    Input key;
    if (!key.load(env, argv[1], "key")) return nullptr;
    return copy_value(env, s, key.slice, "get");
}

static napi_value store_sync(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    fqdb_status st = fqdb_store_sync(s);
    if (st != FQDB_OK) return throw_status(env, st, "sync");
    return nullptr;
}

static napi_value store_write_options(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    fqdb_store_write_options options{};
    fqdb_status st = fqdb_store_write_options_get(s, &options);
    if (st != FQDB_OK) return throw_status(env, st, "writeOptions");
    napi_value result, mode, period, threshold;
    FQN_TRY(napi_create_array_with_length(env, 3, &result));
    FQN_TRY(napi_create_uint32(env, options.mode, &mode));
    FQN_TRY(napi_create_uint32(env, options.period_ms, &period));
    FQN_TRY(napi_create_bigint_uint64(env, options.threshold_bytes, &threshold));
    FQN_TRY(napi_set_element(env, result, 0, mode));
    FQN_TRY(napi_set_element(env, result, 1, period));
    FQN_TRY(napi_set_element(env, result, 2, threshold));
    return result;
}

static napi_value store_write_configure(napi_env env, napi_callback_info info) {
    FQN_ARGS(4);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    fqdb_store_write_options options{};
    fqdb_status st = fqdb_store_write_options_get(s, &options);
    if (st != FQDB_OK) return throw_status(env, st, "writeOptions");
    uint64_t mode, period, threshold;
    if (!get_u64(env, argv[1], options.mode, &mode, "mode") ||
        !get_u64(env, argv[2], options.period_ms, &period, "periodMs") ||
        !get_u64(env, argv[3], options.threshold_bytes, &threshold, "thresholdBytes")) return nullptr;
    if (mode > UINT32_MAX || period > UINT32_MAX) return throw_type(env, "mode/periodMs exceed uint32 range");
    options.mode = static_cast<uint32_t>(mode);
    options.period_ms = static_cast<uint32_t>(period);
    options.threshold_bytes = threshold;
    st = fqdb_store_write_configure(s, &options);
    if (st != FQDB_OK) return throw_status(env, st, "configureWrite");
    return nullptr;
}

static napi_value store_write_reset(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    fqdb_status st = fqdb_store_write_configure(s, nullptr);
    if (st != FQDB_OK) return throw_status(env, st, "resetWriteOptions");
    return nullptr;
}

static napi_value store_info(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    fqdb_store_info si;
    std::memset(&si, 0, sizeof si);
    fqdb_status st = fqdb_store_get_info(s, &si);
    if (st != FQDB_OK) return throw_status(env, st, "info");
    napi_value o;
    FQN_TRY(napi_create_object(env, &o));
    if (!set_big(env, o, "version", si.version) || !set_big(env, o, "durableVersion", si.durable_version) ||
        !set_big(env, o, "fileBytes", si.file_bytes) || !set_big(env, o, "indexedKeys", si.indexed_keys) ||
        !set_big(env, o, "storedKeys", si.stored_keys) || !set_big(env, o, "storedValueBytes", si.stored_value_bytes) ||
        !set_big(env, o, "deltaBytes", si.delta_bytes) || !set_big(env, o, "cacheBytes", si.cache_bytes) ||
        !set_big(env, o, "pinnedBytes", si.pinned_bytes) || !set_big(env, o, "transactionBytes", si.transaction_bytes) ||
        !set_big(env, o, "recoveredTransactions", si.recovered_transactions) ||
        !set_big(env, o, "repairedBytes", si.repaired_bytes))
        return fail(env, napi_generic_failure);
    return o;
}

// ---------- 索引 ----------
static napi_value maintenance_result(napi_env env, const fqdb_store_maintenance_info &m) {
    napi_value o;
    FQN_TRY(napi_create_object(env, &o));
    if (!set_big(env, o, "snapshotVersion", m.snapshot_version) || !set_big(env, o, "finalVersion", m.final_version) ||
        !set_big(env, o, "sourceFileBytes", m.source_file_bytes) ||
        !set_big(env, o, "outputFileBytes", m.output_file_bytes) || !set_big(env, o, "storedKeys", m.stored_keys) ||
        !set_big(env, o, "storedValueBytes", m.stored_value_bytes) || !set_big(env, o, "copiedKeys", m.copied_keys) ||
        !set_big(env, o, "catchupRounds", m.catchup_rounds))
        return fail(env, napi_generic_failure);
    return o;
}

// defineCompoundIndex(store, name, prefix, fieldsFlat:number[] [field,type,...], flags)
static napi_value store_define_index(napi_env env, napi_callback_info info) {
    FQN_ARGS(5);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    Input name, prefix;
    if (!name.load(env, argv[1], "index name") || !prefix.load(env, argv[2], "key prefix")) return nullptr;
    bool is_array = false;
    FQN_TRY(napi_is_array(env, argv[3], &is_array));
    if (!is_array) return throw_type(env, "fields must be an array");
    uint32_t n = 0;
    FQN_TRY(napi_get_array_length(env, argv[3], &n));
    if (n == 0 || n % 2 != 0 || n > 16) return throw_range(env, "index needs 1..8 [field, type] pairs");
    fqdb_store_index_field fields[8];
    for (uint32_t i = 0; i < n; ++i) {
        napi_value e;
        uint32_t x = 0;
        FQN_TRY(napi_get_element(env, argv[3], i, &e));
        FQN_TRY(napi_get_value_uint32(env, e, &x));
        (i % 2 == 0 ? fields[i / 2].field : fields[i / 2].type) = x;
    }
    uint32_t flags = 0;
    FQN_TRY(napi_get_value_uint32(env, argv[4], &flags));
    fqdb_store_maintenance_info mi;
    std::memset(&mi, 0, sizeof mi);
    fqdb_status st = fqdb_store_index_define_compound(s, name.slice, prefix.slice, fields, n / 2, flags, nullptr, &mi);
    if (st != FQDB_OK) return throw_status(env, st, "defineCompoundIndex");
    return maintenance_result(env, mi);
}

static napi_value store_drop_index(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    Input name;
    if (!name.load(env, argv[1], "index name")) return nullptr;
    fqdb_store_maintenance_info mi;
    std::memset(&mi, 0, sizeof mi);
    fqdb_status st = fqdb_store_index_drop(s, name.slice, nullptr, &mi);
    if (st != FQDB_OK) return throw_status(env, st, "dropIndex");
    return maintenance_result(env, mi);
}

// ---------- 事务 ----------
static void txn_finalize(napi_env, void *data, void *) {
    auto *t = static_cast<TxnBox *>(data);
    if (t->txn) fqdb_store_txn_close(t->txn);
    if (t->box) box_release(t->box);
    delete t;
}

// txnBegin(store, timeoutMs?) -> txn handle
static napi_value txn_begin(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    uint64_t timeout;
    if (!get_u64(env, argv[1], 0, &timeout, "timeoutMs")) return nullptr;
    fqdb_store_txn *txn = nullptr;
    fqdb_status st = fqdb_store_txn_begin(s, timeout, &txn);
    if (st != FQDB_OK) return throw_status(env, st, "transaction");
    auto *sr = static_cast<StoreRef *>(unwrap_handle(env, argv[0], MAGIC_STORE, "store"));
    auto *t = new TxnBox();
    t->txn = txn;
    t->box = sr->box;
    sr->box->refs.fetch_add(1, std::memory_order_relaxed);  // 事务期间保持底层 Store 存活
    napi_value ext;
    napi_status ns = napi_create_external(env, t, txn_finalize, nullptr, &ext);
    if (ns != napi_ok) {
        txn_finalize(env, t, nullptr);
        return fail(env, ns);
    }
    return ext;
}

static napi_value txn_close(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    auto *t = static_cast<TxnBox *>(unwrap_handle(env, argv[0], MAGIC_TXN, "transaction"));
    if (!t) return nullptr;
    if (t->txn) {
        fqdb_store_txn_close(t->txn);
        t->txn = nullptr;
    }
    if (t->box) {
        box_release(t->box);
        t->box = nullptr;
    }
    return nullptr;
}

static napi_value txn_get(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    TxnBox *t = live_txn(env, argv[0]);
    if (!t) return nullptr;
    Input key;
    if (!key.load(env, argv[1], "key")) return nullptr;
    fqdb_buffer b{nullptr, 0};
    fqdb_status st = fqdb_store_txn_get(t->txn, key.slice, &b);
    napi_value r;
    if (st == FQDB_NOT_FOUND) {
        FQN_TRY(napi_get_null(env, &r));
        return r;
    }
    if (st != FQDB_OK) return throw_status(env, st, "transaction get");
    r = copy_to_buffer(env, b.data, b.size);
    fqdb_buffer_free(&b);
    return r;
}

static napi_value txn_put(napi_env env, napi_callback_info info) {
    FQN_ARGS(4);
    TxnBox *t = live_txn(env, argv[0]);
    if (!t) return nullptr;
    Input key, value;
    uint64_t expires;
    if (!key.load(env, argv[1], "key") || !value.load(env, argv[2], "value") ||
        !get_u64(env, argv[3], 0, &expires, "expiresAtMs"))
        return nullptr;
    fqdb_status st = fqdb_store_txn_put(t->txn, key.slice, value.slice, expires);
    if (st != FQDB_OK) return throw_status(env, st, "transaction put");
    return nullptr;
}

static napi_value txn_delete(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    TxnBox *t = live_txn(env, argv[0]);
    if (!t) return nullptr;
    Input key;
    if (!key.load(env, argv[1], "key")) return nullptr;
    fqdb_status st = fqdb_store_txn_delete(t->txn, key.slice);
    if (st != FQDB_OK && st != FQDB_NOT_FOUND) return throw_status(env, st, "transaction delete");
    return nullptr;
}

// txnPutMany(txn, items: [key, value, expiresAtMs?][])
static napi_value txn_put_many(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    TxnBox *t = live_txn(env, argv[0]);
    if (!t) return nullptr;
    bool is_array = false;
    FQN_TRY(napi_is_array(env, argv[1], &is_array));
    if (!is_array) return throw_type(env, "items must be an array");
    uint32_t n = 0;
    FQN_TRY(napi_get_array_length(env, argv[1], &n));
    std::deque<Input> inputs;
    std::vector<fqdb_store_put_item> items(n);
    for (uint32_t i = 0; i < n; ++i) {
        napi_value row, k, v, e;
        FQN_TRY(napi_get_element(env, argv[1], i, &row));
        FQN_TRY(napi_get_element(env, row, 0, &k));
        FQN_TRY(napi_get_element(env, row, 1, &v));
        FQN_TRY(napi_get_element(env, row, 2, &e));
        inputs.emplace_back();
        Input &ik = inputs.back();
        inputs.emplace_back();
        Input &iv = inputs.back();
        uint64_t expires;
        if (!ik.load(env, k, "key") || !iv.load(env, v, "value") || !get_u64(env, e, 0, &expires, "expiresAtMs"))
            return nullptr;
        items[i] = fqdb_store_put_item{ik.slice, iv.slice, expires};
    }
    fqdb_status st = fqdb_store_txn_put_many(t->txn, items.data(), n);
    if (st != FQDB_OK) return throw_status(env, st, "transaction putMany");
    return nullptr;
}

static napi_value txn_delete_many(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    TxnBox *t = live_txn(env, argv[0]);
    if (!t) return nullptr;
    bool is_array = false;
    FQN_TRY(napi_is_array(env, argv[1], &is_array));
    if (!is_array) return throw_type(env, "keys must be an array");
    uint32_t n = 0;
    FQN_TRY(napi_get_array_length(env, argv[1], &n));
    std::deque<Input> inputs;
    std::vector<fqdb_slice> keys(n);
    for (uint32_t i = 0; i < n; ++i) {
        napi_value k;
        FQN_TRY(napi_get_element(env, argv[1], i, &k));
        inputs.emplace_back();
        if (!inputs.back().load(env, k, "key")) return nullptr;
        keys[i] = inputs.back().slice;
    }
    fqdb_status st = fqdb_store_txn_delete_many(t->txn, keys.data(), n);
    if (st != FQDB_OK) return throw_status(env, st, "transaction deleteMany");
    return nullptr;
}

// txnCommit(txn, forceReliable?) -> bigint version
static napi_value txn_commit(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    TxnBox *t = live_txn(env, argv[0]);
    if (!t) return nullptr;
    uint64_t version = 0;
    fqdb_status st = fqdb_store_txn_commit(t->txn, &version, get_bool(env, argv[1], false) ? 1u : 0u);
    if (st != FQDB_OK) return throw_status(env, st, "commit");
    return make_bigint(env, version);
}

static napi_value txn_savepoint(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    TxnBox *t = live_txn(env, argv[0]);
    if (!t) return nullptr;
    uint64_t id = 0;
    fqdb_status st = fqdb_store_txn_savepoint(t->txn, &id);
    if (st != FQDB_OK) return throw_status(env, st, "savepoint");
    return make_bigint(env, id);
}
static napi_value txn_rollback_to(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    TxnBox *t = live_txn(env, argv[0]);
    uint64_t id;
    if (!t || !get_u64(env, argv[1], 0, &id, "savepoint")) return nullptr;
    fqdb_status st = fqdb_store_txn_rollback_to(t->txn, id);
    if (st != FQDB_OK) return throw_status(env, st, "rollbackTo");
    return nullptr;
}
static napi_value txn_release_savepoint(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    TxnBox *t = live_txn(env, argv[0]);
    uint64_t id;
    if (!t || !get_u64(env, argv[1], 0, &id, "savepoint")) return nullptr;
    fqdb_status st = fqdb_store_txn_release(t->txn, id);
    if (st != FQDB_OK) return throw_status(env, st, "releaseSavepoint");
    return nullptr;
}

// ---------- 快照 ----------
static void snap_finalize(napi_env, void *data, void *) {
    auto *s = static_cast<SnapBox *>(data);
    if (s->snap) fqdb_store_snapshot_close(s->snap);
    if (s->box) box_release(s->box);
    delete s;
}

static napi_value snapshot_create(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    fqdb_store_snapshot *snap = nullptr;
    fqdb_status st = fqdb_store_snapshot_create(s, &snap);
    if (st != FQDB_OK) return throw_status(env, st, "snapshot");
    auto *sr = static_cast<StoreRef *>(unwrap_handle(env, argv[0], MAGIC_STORE, "store"));
    auto *b = new SnapBox();
    b->snap = snap;
    b->box = sr->box;
    sr->box->refs.fetch_add(1, std::memory_order_relaxed);
    napi_value ext;
    napi_status ns = napi_create_external(env, b, snap_finalize, nullptr, &ext);
    if (ns != napi_ok) {
        snap_finalize(env, b, nullptr);
        return fail(env, ns);
    }
    return ext;
}

static SnapBox *live_snap(napi_env env, napi_value v) {
    auto *s = static_cast<SnapBox *>(unwrap_handle(env, v, MAGIC_SNAP, "snapshot"));
    if (!s) return nullptr;
    if (!s->snap) {
        throw_closed(env, "snapshot");
        return nullptr;
    }
    return s;
}

static napi_value snapshot_close(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    auto *s = static_cast<SnapBox *>(unwrap_handle(env, argv[0], MAGIC_SNAP, "snapshot"));
    if (!s) return nullptr;
    if (s->snap) {
        fqdb_store_snapshot_close(s->snap);
        s->snap = nullptr;
    }
    if (s->box) {
        box_release(s->box);
        s->box = nullptr;
    }
    return nullptr;
}

static napi_value snapshot_get(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    SnapBox *s = live_snap(env, argv[0]);
    if (!s) return nullptr;
    Input key;
    if (!key.load(env, argv[1], "key")) return nullptr;
    fqdb_buffer b{nullptr, 0};
    fqdb_status st = fqdb_store_snapshot_get(s->snap, key.slice, &b);
    napi_value r;
    if (st == FQDB_NOT_FOUND) {
        FQN_TRY(napi_get_null(env, &r));
        return r;
    }
    if (st != FQDB_OK) return throw_status(env, st, "snapshot get");
    r = copy_to_buffer(env, b.data, b.size);
    fqdb_buffer_free(&b);
    return r;
}

static napi_value snapshot_get_shared(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    SnapBox *s = live_snap(env, argv[0]);
    if (!s) return nullptr;
    Input key;
    if (!key.load(env, argv[1], "key")) return nullptr;
    fqdb_store_shared_value sv;
    std::memset(&sv, 0, sizeof sv);
    fqdb_status st = fqdb_store_snapshot_get_shared(s->snap, key.slice, &sv);
    napi_value r;
    if (st == FQDB_NOT_FOUND) {
        FQN_TRY(napi_get_null(env, &r));
        return r;
    }
    if (st != FQDB_OK) return throw_status(env, st, "snapshot getShared");
    auto *l = new Lease();
    l->owner = sv.owner;
    l->version = sv.row.version;
    l->expires_at_ms = sv.row.expires_at_ms;
    return make_lease_view(env, l, sv.row.value.data, sv.row.value.size);
}

// ---------- 查询 ----------
// select(store, request) -> Uint8Array（原生响应缓冲上的视图，release 或 GC 时 buffer_free）
static napi_value store_select(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    fqdb_store *s = live_store(env, argv[0]);
    if (!s) return nullptr;
    Input req;
    if (!req.load(env, argv[1], "request")) return nullptr;
    fqdb_buffer out{nullptr, 0};
    fqdb_status st = fqdb_store_select(s, req.slice, &out);
    if (st != FQDB_OK) return throw_status(env, st, "select");
    auto *l = new Lease();
    l->buffer = out;
    return make_lease_view(env, l, out.data, out.size);
}

static napi_value txn_select(napi_env env, napi_callback_info info) {
    FQN_ARGS(2);
    TxnBox *t = live_txn(env, argv[0]);
    if (!t) return nullptr;
    Input req;
    if (!req.load(env, argv[1], "request")) return nullptr;
    fqdb_buffer out{nullptr, 0};
    fqdb_status st = fqdb_store_txn_execute(t->txn, req.slice, &out);
    if (st != FQDB_OK) return throw_status(env, st, "select");
    auto *l = new Lease();
    l->buffer = out;
    return make_lease_view(env, l, out.data, out.size);
}

napi_status register_store(napi_env env, napi_value exports) {
    struct Entry {
        const char *name;
        napi_callback cb;
    } table[] = {
        {"open", store_open},
        {"storeClose", store_close},
        {"storeIsClosed", store_is_closed},
        {"storeShare", store_share},
        {"storeAttach", store_attach},
        {"storeDiscard", store_discard},
        {"storePut", store_put},
        {"storeDelete", store_delete},
        {"storeGet", store_get},
        {"storeSync", store_sync},
        {"storeWriteOptions", store_write_options},
        {"storeWriteConfigure", store_write_configure},
        {"storeWriteReset", store_write_reset},
        {"storeInfo", store_info},
        {"storeDefineIndex", store_define_index},
        {"storeDropIndex", store_drop_index},
        {"storeSelect", store_select},
        {"txnBegin", txn_begin},
        {"txnClose", txn_close},
        {"txnGet", txn_get},
        {"txnPut", txn_put},
        {"txnDelete", txn_delete},
        {"txnPutMany", txn_put_many},
        {"txnDeleteMany", txn_delete_many},
        {"txnCommit", txn_commit},
        {"txnSavepoint", txn_savepoint},
        {"txnRollbackTo", txn_rollback_to},
        {"txnReleaseSavepoint", txn_release_savepoint},
        {"txnSelect", txn_select},
        {"snapshotCreate", snapshot_create},
        {"snapshotClose", snapshot_close},
        {"snapshotGet", snapshot_get},
        {"snapshotGetShared", snapshot_get_shared},
    };
    for (const auto &e : table) {
        napi_status st = set_function(env, exports, e.name, e.cb);
        if (st != napi_ok) return st;
    }
    return napi_ok;
}

}  // namespace fqn
