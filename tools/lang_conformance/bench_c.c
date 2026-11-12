/* 跨语言绑定性能基线（纯 C ABI）。各语言绑定用同样的数据与操作测量，开销与此对比。
 * 用法: bench_c DIR [SECONDS]   输出每行一个 JSON: {"op":..., "ns_per_op":..., "ops":...}
 * 数据: 32768 条 256 B 记录，键 'k'+8 字节大端序号；查询数据集与 conformance.json 的 p/ 部分同形。 */
#define _POSIX_C_SOURCE 200809L
#include "fengqudb_store.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N 32768u
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static void make_key(uint8_t k[9], uint64_t id) {
    k[0] = 'k';
    for (int i = 0; i < 8; ++i) k[1 + i] = (uint8_t)(id >> (56 - 8 * i));
}
static void check(fqdb_status s, const char *what) {
    if (s != FQDB_OK) { fprintf(stderr, "%s: %s\n", what, fqdb_status_message(s)); exit(2); }
}
static void report(const char *op, uint64_t ops, double seconds, uint64_t sink) {
    printf("{\"op\":\"%s\",\"ops\":%" PRIu64 ",\"ns_per_op\":%.1f,\"sink\":%" PRIu64 "}\n", op, ops,
           seconds * 1e9 / (double)ops, sink);
    fflush(stdout);
}
/* 简单 protobuf 写入：varint */
static size_t put_varint(uint8_t *p, uint64_t v) {
    size_t n = 0;
    while (v >= 128) { p[n++] = (uint8_t)(v | 128); v >>= 7; }
    p[n++] = (uint8_t)v;
    return n;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/tmp/fqdb-bench";
    double seconds = argc > 2 ? atof(argv[2]) : 1.0;
    fqdb_store_options o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o; o.abi_version = 1; o.durability = 1; o.flags = FQDB_STORE_CREATE;
    /* 预算取值与 tools/final_benchmark/fq.cpp 相同；全 0 会被拒绝为 invalid argument。 */
    o.cache_bytes = 2ull << 30; o.delta_bytes = 16ull << 20; o.writer_bytes = 32ull << 20;
    o.transaction_bytes = 64ull << 20; o.all_transaction_bytes = 256ull << 20;
    o.pinned_bytes = 2ull << 30; o.max_snapshots = 1024;
    fqdb_store *db = NULL;
    check(fqdb_store_open(dir, &o, &db), "open");
    uint8_t value[256], key[9];
    for (uint64_t i = 0; i < N; ++i) {
        memset(value, (int)(i & 0xff), sizeof value);
        make_key(key, i);
        fqdb_slice k = {key, 9}, v = {value, sizeof value};
        fqdb_store_atomic_info info;
        check(fqdb_store_put(db, k, v, 0, &info, 0), "put");
    }
    check(fqdb_store_sync(db), "sync");

    /* 1. 准备键 + 共享读取（零拷贝视图）：热点读 */
    {
        make_key(key, 4242);
        fqdb_slice k = {key, 9};
        fqdb_store_key *pk = NULL;
        check(fqdb_store_key_prepare(db, k, &pk), "prepare");
        uint64_t sink = 0, ops = 0;
        double t0 = now(), t;
        do {
            for (int i = 0; i < 1000; ++i) {
                fqdb_store_shared_value sv;
                check(fqdb_store_key_get_shared(pk, &sv), "get_shared");
                sink += ((const uint8_t *)sv.row.value.data)[sv.row.value.size - 1];
                fqdb_store_value_close(sv.owner);
            }
            ops += 1000;
            t = now();
        } while (t - t0 < seconds);
        report("get_shared_prepared", ops, t - t0, sink);
        fqdb_store_key_close(pk);
    }
    /* 2. 按键共享读取（每次传键） */
    {
        uint64_t sink = 0, ops = 0, id = 0;
        double t0 = now(), t;
        do {
            for (int i = 0; i < 1000; ++i) {
                make_key(key, (id++ * 2654435761u) % N);
                fqdb_slice k = {key, 9};
                fqdb_store_shared_value sv;
                check(fqdb_store_get_shared(db, k, &sv), "get_shared");
                sink += ((const uint8_t *)sv.row.value.data)[0];
                fqdb_store_value_close(sv.owner);
            }
            ops += 1000;
            t = now();
        } while (t - t0 < seconds);
        report("get_shared_key", ops, t - t0, sink);
    }
    /* 3. 复制读取（调用方拥有副本） */
    {
        uint64_t sink = 0, ops = 0, id = 0;
        double t0 = now(), t;
        do {
            for (int i = 0; i < 1000; ++i) {
                make_key(key, (id++ * 2654435761u) % N);
                fqdb_slice k = {key, 9};
                fqdb_buffer out;
                check(fqdb_store_get(db, k, &out), "get");
                sink += ((const uint8_t *)out.data)[0];
                fqdb_buffer_free(&out);
            }
            ops += 1000;
            t = now();
        } while (t - t0 < seconds);
        report("get_copy", ops, t - t0, sink);
    }
    /* 4. 32 键准备读取器（批量）：每个操作 = 1 条记录 */
    {
        fqdb_slice keys[32];
        uint8_t kb[32][9];
        for (int i = 0; i < 32; ++i) { make_key(kb[i], (uint64_t)i * 997); keys[i].data = kb[i]; keys[i].size = 9; }
        fqdb_store_reader *rd = NULL;
        check(fqdb_store_reader_create(db, keys, 32, &rd), "reader_create");
        uint64_t sink = 0, ops = 0;
        double t0 = now(), t;
        do {
            for (int i = 0; i < 200; ++i) {
                const fqdb_store_result_row *rows; uint32_t count;
                check(fqdb_store_reader_read(rd, &rows, &count), "reader_read");
                for (uint32_t j = 0; j < count; ++j) sink += ((const uint8_t *)rows[j].value.data)[0];
            }
            ops += 200u * 32u;
            t = now();
        } while (t - t0 < seconds);
        report("reader32_per_record", ops, t - t0, sink);
        fqdb_store_reader_close(rd);
    }
    /* 5. 复制读取 + 逐条 get_many 的对照由各语言自选；此处略 */

    /* 6. 查询：请求字节来自 conformance.json 的 topn_mage_level（环境变量 FQDB_BENCH_SELECT_HEX）。
       数据: 64 条 p/ 文档，字段 1 uid、2 class、3 level，与该用例所需字段一致。 */
    const char *hex = getenv("FQDB_BENCH_SELECT_HEX");
    if (hex) {
        for (uint64_t i = 0; i < 64; ++i) {
            uint8_t buf[64]; size_t n = 0;
            buf[n++] = 0x08; n += put_varint(buf + n, i);
            const char *cls = (i % 2) ? "mage" : "warrior"; size_t cl = strlen(cls);
            buf[n++] = 0x12; buf[n++] = (uint8_t)cl; memcpy(buf + n, cls, cl); n += cl;
            buf[n++] = 0x18; n += put_varint(buf + n, i % 20);
            uint8_t k2[16]; int kl = snprintf((char *)k2, sizeof k2, "p/%" PRIu64, i);
            fqdb_slice k = {k2, (uint64_t)kl}, v = {buf, n};
            fqdb_store_atomic_info info;
            check(fqdb_store_put(db, k, v, 0, &info, 0), "put p");
        }
        size_t hl = strlen(hex), rn = hl / 2;
        uint8_t *req = (uint8_t *)malloc(rn);
        for (size_t i = 0; i < rn; ++i) {
            unsigned b; sscanf(hex + 2 * i, "%2x", &b); req[i] = (uint8_t)b;
        }
        uint64_t sink = 0, ops = 0;
        double t0 = now(), t;
        do {
            for (int i = 0; i < 200; ++i) {
                fqdb_slice r = {req, rn};
                fqdb_buffer out;
                check(fqdb_store_select(db, r, &out), "select");
                sink += out.size;
                fqdb_buffer_free(&out);
            }
            ops += 200;
            t = now();
        } while (t - t0 < seconds);
        report("select_topn", ops, t - t0, sink);
        free(req);
    }
    fqdb_store_close(db);
    return 0;
}
