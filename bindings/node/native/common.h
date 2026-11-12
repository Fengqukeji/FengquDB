// FengquDB N-API addon: shared declarations.
// 设计要点：所有句柄都是 napi_external，指向以 magic 开头的原生结构；
// Store 通过引用计数的 StoreBox 在线程之间共享，事务/快照持有一个引用，
// 因此 Store.close() 不会在有未结束事务/快照时真正关闭底层库。
#pragma once
#ifndef NAPI_VERSION
#define NAPI_VERSION 8
#endif
#include <node_api.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "fengqudb_store.h"

namespace fqn {

enum : uint32_t {
    MAGIC_STORE = 0x46510001u,
    MAGIC_TXN = 0x46510002u,
    MAGIC_SNAP = 0x46510003u,
    MAGIC_KEY = 0x46510004u,
    MAGIC_READER = 0x46510005u,
    MAGIC_LEASE = 0x46510006u,
};

// JS 层的 FengquDBError 构造函数保存在每个 env 的实例数据里（worker 各自独立）。
struct EnvData {
    napi_ref error_class = nullptr;
};
EnvData *env_data(napi_env env);

// 状态码 -1 表示“句柄已关闭/已释放”这类误用（不是核心库返回的状态）。
constexpr int32_t STATUS_CLOSED = -1;

napi_value throw_status(napi_env env, fqdb_status status, const char *operation);
napi_value throw_closed(napi_env env, const char *what);
napi_value throw_type(napi_env env, const char *message);
napi_value throw_range(napi_env env, const char *message);
// 将失败的 napi_status 转成 JS 异常（若已有挂起异常则保持）。
napi_value fail(napi_env env, napi_status status);

#define FQN_TRY(expr)                                   \
    do {                                                \
        napi_status fqn_s_ = (expr);                    \
        if (fqn_s_ != napi_ok) return fail(env, fqn_s_); \
    } while (0)

#define FQN_ARGS(n)                                                         \
    napi_value argv[n];                                                     \
    size_t argc = n;                                                        \
    FQN_TRY(napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr))

// ---- 句柄结构 ----
struct StoreBox {
    std::atomic<int> refs{1};
    fqdb_store *store = nullptr;
};
void box_release(StoreBox *box);  // 引用归零时关闭底层 Store

struct StoreRef {
    uint32_t magic = MAGIC_STORE;
    StoreBox *box = nullptr;
    bool closed = false;
};
struct TxnBox {
    uint32_t magic = MAGIC_TXN;
    fqdb_store_txn *txn = nullptr;
    StoreBox *box = nullptr;
};
struct SnapBox {
    uint32_t magic = MAGIC_SNAP;
    fqdb_store_snapshot *snap = nullptr;
    StoreBox *box = nullptr;
};
struct KeyBox {
    uint32_t magic = MAGIC_KEY;
    fqdb_store_key *key = nullptr;
};
struct ReaderBox {
    uint32_t magic = MAGIC_READER;
    fqdb_store_reader *reader = nullptr;
    const fqdb_store_result_row *rows = nullptr;
    uint32_t count = 0;
    uint32_t capacity = 0;     // 构造时键的个数
    uint32_t *info = nullptr;  // JS 提供的 Uint32Array，[2*i]=found [2*i+1]=size
    napi_ref info_ref = nullptr;
    std::vector<napi_ref> views;  // 已交出的值视图（弱引用），下一次 read/release/close 时分离
};
// 共享租约/查询结果缓冲：由 external ArrayBuffer 持有（napi_wrap 关联），
// finalizer 或显式 release 归还。
struct Lease {
    uint32_t magic = MAGIC_LEASE;
    bool released = false;
    fqdb_store_value *owner = nullptr;  // 值租约
    fqdb_buffer buffer{nullptr, 0};     // 或查询结果缓冲
    uint64_t version = 0, expires_at_ms = 0;
    void close();
};

// ---- 参数辅助 ----
// 键/值输入：Uint8Array(含 Buffer) / ArrayBuffer / string(UTF-8)。短字符串使用栈缓冲。
struct Input {
    char stack[256];
    std::string heap;
    fqdb_slice slice{nullptr, 0};
    Input() = default;
    Input(const Input &) = delete;
    Input &operator=(const Input &) = delete;
    bool load(napi_env env, napi_value value, const char *what);  // 失败时已抛出 JS 异常
};

bool get_u64(napi_env env, napi_value v, uint64_t def, uint64_t *out, const char *what);
bool get_bool(napi_env env, napi_value v, bool def);
void *unwrap_handle(napi_env env, napi_value v, uint32_t magic, const char *what);

// 取得仍然打开的 Store；已关闭则抛出 status=-1 错误并返回 nullptr。
fqdb_store *live_store(napi_env env, napi_value v);
TxnBox *live_txn(napi_env env, napi_value v);

napi_value make_bigint(napi_env env, uint64_t v);
napi_value copy_to_buffer(napi_env env, const void *data, size_t size);
// 以 external ArrayBuffer 包装租约并返回 Uint8Array；失败时释放租约。
napi_value make_lease_view(napi_env env, Lease *lease, const void *data, size_t size);

// 注册函数（module.cpp 调用）。
napi_status register_store(napi_env env, napi_value exports);
napi_status register_shared(napi_env env, napi_value exports);
napi_status set_function(napi_env env, napi_value exports, const char *name, napi_callback cb);

}  // namespace fqn
