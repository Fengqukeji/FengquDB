// 模块入口、错误构造、参数辅助、租约包装。
#include "common.h"

namespace fqn {

static void env_data_finalize(napi_env env, void *data, void *) {
    auto *d = static_cast<EnvData *>(data);
    if (d->error_class) napi_delete_reference(env, d->error_class);
    delete d;
}

EnvData *env_data(napi_env env) {
    void *data = nullptr;
    napi_get_instance_data(env, &data);
    return static_cast<EnvData *>(data);
}

static bool pending(napi_env env) {
    bool p = false;
    napi_is_exception_pending(env, &p);
    return p;
}

// 构造 JS 侧注册的 FengquDBError(status, message)；未注册时退回普通 Error。
static napi_value throw_error(napi_env env, int32_t status, const std::string &message) {
    if (pending(env)) return nullptr;
    EnvData *d = env_data(env);
    napi_value msg;
    if (napi_create_string_utf8(env, message.c_str(), message.size(), &msg) != napi_ok) return nullptr;
    if (d && d->error_class) {
        napi_value cls, args[2], err;
        if (napi_get_reference_value(env, d->error_class, &cls) == napi_ok && cls &&
            napi_create_int32(env, status, &args[0]) == napi_ok) {
            args[1] = msg;
            if (napi_new_instance(env, cls, 2, args, &err) == napi_ok) {
                napi_throw(env, err);
                return nullptr;
            }
            if (pending(env)) return nullptr;
        }
    }
    napi_throw_error(env, nullptr, message.c_str());
    return nullptr;
}

napi_value throw_status(napi_env env, fqdb_status status, const char *operation) {
    std::string m = std::string(operation) + ": " + fqdb_status_message(status);
    return throw_error(env, status, m);
}
napi_value throw_closed(napi_env env, const char *what) {
    return throw_error(env, STATUS_CLOSED, std::string(what) + " is closed or released");
}
napi_value throw_type(napi_env env, const char *message) {
    if (!pending(env)) napi_throw_type_error(env, nullptr, message);
    return nullptr;
}
napi_value throw_range(napi_env env, const char *message) {
    if (!pending(env)) napi_throw_range_error(env, nullptr, message);
    return nullptr;
}
napi_value fail(napi_env env, napi_status status) {
    if (pending(env)) return nullptr;
    const napi_extended_error_info *e = nullptr;
    napi_get_last_error_info(env, &e);
    std::string m = std::string("N-API call failed (") + std::to_string(static_cast<int>(status)) + ")";
    if (e && e->error_message) m += std::string(": ") + e->error_message;
    napi_throw_error(env, nullptr, m.c_str());
    return nullptr;
}

void box_release(StoreBox *box) {
    if (box->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (box->store) fqdb_store_close(box->store);
        delete box;
    }
}

void Lease::close() {
    if (released) return;
    released = true;
    if (owner) fqdb_store_value_close(owner);
    owner = nullptr;
    if (buffer.data) fqdb_buffer_free(&buffer);
    buffer = fqdb_buffer{nullptr, 0};
}

static size_t element_size(napi_typedarray_type t) {
    switch (t) {
        case napi_int8_array:
        case napi_uint8_array:
        case napi_uint8_clamped_array: return 1;
        default: return 0;
    }
}

bool Input::load(napi_env env, napi_value value, const char *what) {
    static const char zero = 0;
    napi_typedarray_type type;
    size_t length = 0;
    void *data = nullptr;
    if (napi_get_typedarray_info(env, value, &type, &length, &data, nullptr, nullptr) == napi_ok) {
        if (!element_size(type)) {
            throw_type(env, "byte input must be a Uint8Array (or ArrayBuffer / string)");
            return false;
        }
        slice.data = data ? data : &zero;
        slice.size = length;
        return true;
    }
    napi_valuetype vt;
    if (napi_typeof(env, value, &vt) != napi_ok) return false;
    if (vt == napi_string) {
        size_t n = 0;
        if (napi_get_value_string_utf8(env, value, stack, sizeof stack, &n) != napi_ok) return false;
        if (n < sizeof stack - 1) {
            slice.data = n ? stack : &zero;
            slice.size = n;
            return true;
        }
        if (napi_get_value_string_utf8(env, value, nullptr, 0, &n) != napi_ok) return false;
        heap.resize(n + 1);
        if (napi_get_value_string_utf8(env, value, &heap[0], n + 1, &n) != napi_ok) return false;
        slice.data = heap.data();
        slice.size = n;
        return true;
    }
    bool is_ab = false;
    if (napi_is_arraybuffer(env, value, &is_ab) == napi_ok && is_ab) {
        if (napi_get_arraybuffer_info(env, value, &data, &length) != napi_ok) return false;
        slice.data = data ? data : &zero;
        slice.size = length;
        return true;
    }
    std::string m = std::string(what) + " must be a Uint8Array, ArrayBuffer or string";
    throw_type(env, m.c_str());
    return false;
}

bool get_u64(napi_env env, napi_value v, uint64_t def, uint64_t *out, const char *what) {
    napi_valuetype vt;
    if (napi_typeof(env, v, &vt) != napi_ok) return false;
    if (vt == napi_undefined || vt == napi_null) {
        *out = def;
        return true;
    }
    if (vt == napi_number) {
        double d;
        if (napi_get_value_double(env, v, &d) != napi_ok) return false;
        if (!(d >= 0) || d >= 18446744073709551616.0 || d != static_cast<double>(static_cast<uint64_t>(d))) {
            std::string m = std::string(what) + " must be a non-negative integer";
            throw_range(env, m.c_str());
            return false;
        }
        *out = static_cast<uint64_t>(d);
        return true;
    }
    if (vt == napi_bigint) {
        bool lossless = false;
        if (napi_get_value_bigint_uint64(env, v, out, &lossless) != napi_ok) return false;
        if (!lossless) {
            std::string m = std::string(what) + " does not fit in uint64";
            throw_range(env, m.c_str());
            return false;
        }
        return true;
    }
    std::string m = std::string(what) + " must be a number or bigint";
    throw_type(env, m.c_str());
    return false;
}

bool get_bool(napi_env env, napi_value v, bool def) {
    bool b = def;
    napi_valuetype vt;
    if (napi_typeof(env, v, &vt) == napi_ok && vt == napi_boolean) napi_get_value_bool(env, v, &b);
    return b;
}

void *unwrap_handle(napi_env env, napi_value v, uint32_t magic, const char *what) {
    void *p = nullptr;
    if (napi_get_value_external(env, v, &p) != napi_ok || !p || *static_cast<uint32_t *>(p) != magic) {
        std::string m = std::string("invalid ") + what + " handle";
        throw_type(env, m.c_str());
        return nullptr;
    }
    return p;
}

fqdb_store *live_store(napi_env env, napi_value v) {
    auto *s = static_cast<StoreRef *>(unwrap_handle(env, v, MAGIC_STORE, "store"));
    if (!s) return nullptr;
    if (s->closed) {
        throw_closed(env, "store");
        return nullptr;
    }
    return s->box->store;
}

TxnBox *live_txn(napi_env env, napi_value v) {
    auto *t = static_cast<TxnBox *>(unwrap_handle(env, v, MAGIC_TXN, "transaction"));
    if (!t) return nullptr;
    if (!t->txn) {
        throw_closed(env, "transaction");
        return nullptr;
    }
    return t;
}

napi_value make_bigint(napi_env env, uint64_t v) {
    napi_value r = nullptr;
    FQN_TRY(napi_create_bigint_uint64(env, v, &r));
    return r;
}

napi_value copy_to_buffer(napi_env env, const void *data, size_t size) {
    napi_value r = nullptr;
    void *out = nullptr;
    FQN_TRY(napi_create_buffer_copy(env, size, size ? data : "", &out, &r));
    return r;
}

static void lease_finalize(napi_env, void *, void *hint) {
    auto *l = static_cast<Lease *>(hint);
    l->close();
    delete l;
}

napi_value make_lease_view(napi_env env, Lease *lease, const void *data, size_t size) {
    napi_value ab = nullptr, view = nullptr;
    napi_status st;
    if (size == 0) {
        // 空值：不需要外部内存；租约立即归还，仅保留元数据。
        lease->close();
        void *tmp = nullptr;
        st = napi_create_arraybuffer(env, 0, &tmp, &ab);
        if (st == napi_ok) st = napi_wrap(env, ab, lease, lease_finalize, lease, nullptr);
        if (st != napi_ok) {
            delete lease;
            return fail(env, st);
        }
    } else {
        st = napi_create_external_arraybuffer(env, const_cast<void *>(data), size, lease_finalize, lease, &ab);
        if (st != napi_ok) {
            lease->close();
            delete lease;
            return fail(env, st);
        }
        // 与 ArrayBuffer 同寿；wrap 的 finalizer 为空，释放统一由 lease_finalize 完成。
        st = napi_wrap(env, ab, lease, nullptr, nullptr, nullptr);
        if (st != napi_ok) return fail(env, st);
    }
    st = napi_create_typedarray(env, napi_uint8_array, size, ab, 0, &view);
    if (st != napi_ok) return fail(env, st);
    return view;
}

napi_status set_function(napi_env env, napi_value exports, const char *name, napi_callback cb) {
    napi_value fn;
    napi_status st = napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn);
    if (st != napi_ok) return st;
    return napi_set_named_property(env, exports, name, fn);
}

static napi_value set_error_class(napi_env env, napi_callback_info info) {
    FQN_ARGS(1);
    EnvData *d = env_data(env);
    if (d->error_class) napi_delete_reference(env, d->error_class), d->error_class = nullptr;
    FQN_TRY(napi_create_reference(env, argv[0], 1, &d->error_class));
    return nullptr;
}

static napi_value abi_version(napi_env env, napi_callback_info) {
    napi_value r;
    FQN_TRY(napi_create_uint32(env, fqdb_abi_version(), &r));
    return r;
}

}  // namespace fqn

NAPI_MODULE_INIT() {
    using namespace fqn;
    auto *d = new EnvData();
    if (napi_set_instance_data(env, d, env_data_finalize, nullptr) != napi_ok) {
        delete d;
        return nullptr;
    }
    if (set_function(env, exports, "setErrorClass", set_error_class) != napi_ok ||
        set_function(env, exports, "abiVersion", abi_version) != napi_ok ||
        register_store(env, exports) != napi_ok || register_shared(env, exports) != napi_ok)
        return nullptr;
    return exports;
}
