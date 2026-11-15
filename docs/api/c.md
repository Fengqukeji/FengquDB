# C API 与调用例子

[接入文档](../languages/c.md)

从前面的常用操作开始。本页每节都包含用途、调用例子和对应函数；展开“函数与参数”可查完整重载。

## 1. 读取

下面的 bytes 辅助函数和 db 句柄沿用接入文档。

```c
fqdb_buffer value = {0};
fqdb_status status = fqdb_store_get(db, bytes("name"), &value);
if (status == FQDB_OK) {
    /* value.data/value.size 是独立数据，可以在这里处理 */
    fqdb_buffer_free(&value);
} else if (status == FQDB_NOT_FOUND) {
    /* 不存在 */
} else {
    /* 处理错误 */
}
```

空值是合法数据，不能用长度0判断不存在。

<details>
<summary>函数与参数（8项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `fqdb_status fqdb_store_get(fqdb_store *store, fqdb_slice key, fqdb_buffer *out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_get_version(fqdb_store *store, fqdb_slice key, uint64_t *out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_get_view(fqdb_store *store, fqdb_slice key, fqdb_store_value **out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_reader_acquire(fqdb_store_reader *reader, fqdb_store_results **out)` | 读取对应的数据或状态 |
| `全局` | `void fqdb_store_reader_close(fqdb_store_reader *reader)` | 释放对象或借用的资源 |
| `全局` | `fqdb_status fqdb_store_reader_create(fqdb_store *store, const fqdb_slice *keys, uint32_t count, fqdb_store_reader **out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_reader_read(fqdb_store_reader *reader, const fqdb_store_result_row **rows, uint32_t *count)` | 读取对应的数据或状态 |
| `全局` | `void fqdb_store_reader_release(fqdb_store_reader *reader)` | 释放对象或借用的资源 |

</details>

## 2. 新增、保存、单个字符串

```c
fqdb_store_atomic_info info = {0};
fqdb_status status = fqdb_store_put(db, bytes("name"), bytes("Alice"), 0, &info, 0);
if (status != FQDB_OK) { /* 处理错误 */ }
```

同键 put 覆盖旧值。整数可明确编码成8字节小端格式；使用 add_i64 的键同样采用此格式。

实体写入使用工具生成的函数：

```c
struct Student student = {7, fqdb_text_from_cstr("Alice"), 95};
unsigned char key[64], encoded[256]; size_t key_size, value_size;
fqg_slice unknown = {NULL, 0};
if (!models_StudentFqCodec_key(&student,key,sizeof(key),&key_size)) return 1;
if (!models_StudentFqCodec_encode(&student,unknown,encoded,sizeof(encoded),&value_size)) return 1;
fqdb_slice k = {key,key_size}, v = {encoded,value_size};
status = fqdb_store_put(db,k,v,0,&info,0);
```

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `fqdb_status fqdb_store_put(fqdb_store *store, fqdb_slice key, fqdb_slice value, uint64_t expires_at_ms, fqdb_store_atomic_info *out, uint32_t force_reliable)` | 保存或设置数据 |

</details>

## 3. 修改与原子计数

整份修改再次 put 即可。patch 修改原始字节区间，compare_exchange 按版本修改，add_i64 对专用八字节整数原子加减。编码失败或缓冲不足时，不要把部分输出写入库。

<details>
<summary>函数与参数（6项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `fqdb_status fqdb_store_add_i64(fqdb_store *store, fqdb_slice key, int64_t delta, int64_t minimum, int64_t maximum, uint64_t timeout_ms, fqdb_store_atomic_info *out, uint32_t force_reliable)` | 条件更新或局部修改 |
| `全局` | `fqdb_status fqdb_store_add_i64_fields(fqdb_store *store, const fqdb_store_i64_field_update *items, uint32_t count, uint64_t timeout_ms, fqdb_store_atomic_info *out, uint32_t force_reliable)` | 条件更新或局部修改 |
| `全局` | `fqdb_status fqdb_store_compare_exchange(fqdb_store *store, fqdb_slice key, uint64_t expected_version, fqdb_slice value, uint64_t expires_at_ms, uint64_t timeout_ms, fqdb_store_atomic_info *out, uint32_t force_reliable)` | 条件更新或局部修改 |
| `全局` | `fqdb_status fqdb_store_delete_if_version(fqdb_store *store, fqdb_slice key, uint64_t expected_version, uint64_t timeout_ms, fqdb_store_atomic_info *out, uint32_t force_reliable)` | 删除记录或成员 |
| `全局` | `fqdb_status fqdb_store_patch(fqdb_store *store, fqdb_slice key, uint64_t offset, uint64_t remove_bytes, fqdb_slice insert, fqdb_store_atomic_info *out, uint32_t force_reliable)` | 条件更新或局部修改 |
| `全局` | `fqdb_status fqdb_store_put_if_changed(fqdb_store *store, fqdb_slice key, fqdb_slice value, uint64_t expires_at_ms, uint64_t timeout_ms, fqdb_store_atomic_info *out, uint32_t force_reliable)` | 保存或设置数据 |

</details>

## 4. 删除

```c
fqdb_store_atomic_info info = {0};
fqdb_status status = fqdb_store_delete(db, bytes("student/7"), &info, 0);
```

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `fqdb_status fqdb_store_delete(fqdb_store *store, fqdb_slice key, fqdb_store_atomic_info *out, uint32_t force_reliable)` | 删除记录或成员 |

</details>

## batch

<details>
<summary>函数与参数（5项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `void fqdb_store_batch_close(fqdb_store_batch *batch)` | 释放对象或借用的资源 |
| `全局` | `fqdb_status fqdb_store_batch_create(fqdb_store *store, const fqdb_store_put_item *items, uint32_t count, fqdb_store_batch **out)` | 批量处理 |
| `全局` | `void fqdb_store_change_batch_close(fqdb_store_change_batch *batch)` | 释放对象或借用的资源 |
| `全局` | `const fqdb_store_change_batch_info * fqdb_store_change_batch_data(const fqdb_store_change_batch *batch)` | 批量处理 |
| `全局` | `fqdb_status fqdb_store_get_many(fqdb_store *store, const fqdb_slice *keys, uint32_t count, fqdb_store_results **out)` | 读取对应的数据或状态 |

</details>

## 5. 事务与原子批写

```c
fqdb_store_txn *tx = NULL;
fqdb_status status = fqdb_store_txn_begin(db, 0, &tx);
if (status == FQDB_OK) {
    status = fqdb_store_txn_put(tx, bytes("a"), bytes("one"), 0);
    if (status == FQDB_OK) {
        uint64_t version = 0;
        status = fqdb_store_txn_commit(tx, &version, 0);
    }
    fqdb_store_txn_close(tx);
}
```

未提交即 close 会丢弃修改。CONFLICT 时重新创建事务并重新读取计算。put_many/delete_many 用于同一笔批写。

<details>
<summary>函数与参数（19项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `fqdb_status fqdb_store_txn_begin(fqdb_store *store, uint64_t timeout_ms, fqdb_store_txn **out)` | 事务操作 |
| `全局` | `void fqdb_store_txn_cancel(fqdb_store_txn *txn)` | 事务操作 |
| `全局` | `void fqdb_store_txn_close(fqdb_store_txn *txn)` | 释放对象或借用的资源 |
| `全局` | `fqdb_status fqdb_store_txn_commit(fqdb_store_txn *txn, uint64_t *version, uint32_t force_reliable)` | 提交事务或查看提交结果 |
| `全局` | `fqdb_status fqdb_store_txn_cursor_open(fqdb_store_txn *txn, fqdb_slice begin, fqdb_slice end, uint32_t reverse, fqdb_store_cursor **out)` | 事务操作 |
| `全局` | `fqdb_status fqdb_store_txn_delete(fqdb_store_txn *txn, fqdb_slice key)` | 删除记录或成员 |
| `全局` | `fqdb_status fqdb_store_txn_delete_many(fqdb_store_txn *txn, const fqdb_slice *keys, uint32_t count)` | 删除记录或成员 |
| `全局` | `fqdb_status fqdb_store_txn_execute(fqdb_store_txn *txn, fqdb_slice request, fqdb_buffer *out)` | 事务操作 |
| `全局` | `fqdb_status fqdb_store_txn_expire(fqdb_store_txn *txn, fqdb_slice key, uint64_t expires_at_ms)` | 事务操作 |
| `全局` | `fqdb_status fqdb_store_txn_get(fqdb_store_txn *txn, fqdb_slice key, fqdb_buffer *out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_txn_get_many(fqdb_store_txn *txn, const fqdb_slice *keys, uint32_t count, fqdb_store_results **out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_txn_patch(fqdb_store_txn *txn, fqdb_slice key, uint64_t offset, uint64_t remove_bytes, fqdb_slice insert)` | 事务操作 |
| `全局` | `fqdb_status fqdb_store_txn_put(fqdb_store_txn *txn, fqdb_slice key, fqdb_slice value, uint64_t expires_at_ms)` | 事务操作 |
| `全局` | `fqdb_status fqdb_store_txn_put_batch(fqdb_store_txn *txn, const fqdb_store_batch *batch)` | 事务操作 |
| `全局` | `fqdb_status fqdb_store_txn_put_many(fqdb_store_txn *txn, const fqdb_store_put_item *items, uint32_t count)` | 事务操作 |
| `全局` | `fqdb_status fqdb_store_txn_release(fqdb_store_txn *txn, uint64_t id)` | 释放对象或借用的资源 |
| `全局` | `fqdb_status fqdb_store_txn_rollback_to(fqdb_store_txn *txn, uint64_t id)` | 回退到保存点 |
| `全局` | `fqdb_status fqdb_store_txn_savepoint(fqdb_store_txn *txn, uint64_t *id)` | 创建或操作事务保存点 |
| `全局` | `fqdb_status fqdb_store_txn_submit(fqdb_store_txn *txn, fqdb_store_commit **out, uint32_t force_reliable)` | 事务操作 |

</details>

## 6. 条件查询

fqdb_store_select 接收二进制查询请求，返回二进制结果；用于组合筛选、排序、分组、统计与等值关联。C 接口不提供类型化查询构建器。请求和结果字段定义在本节末尾的查询格式中。

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `fqdb_status fqdb_store_select(fqdb_store *store, fqdb_slice request, fqdb_buffer *out)` | 查询构建或结果读取 |

</details>

## 7. 字段索引

index_define/index_define_compound 为常用条件建立索引，并可设置唯一、必填等约束；index_drop 删除索引。

<details>
<summary>函数与参数（3项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `fqdb_status fqdb_store_index_define(fqdb_store *store, fqdb_slice name, fqdb_slice key_prefix, uint32_t field, uint32_t type, uint32_t flags, const struct fqdb_store_maintenance_options *options, struct fqdb_store_maintenance_info *out)` | 定义或使用索引 |
| `全局` | `fqdb_status fqdb_store_index_define_compound(fqdb_store *store, fqdb_slice name, fqdb_slice key_prefix, const fqdb_store_index_field *fields, uint32_t count, uint32_t flags, const struct fqdb_store_maintenance_options *options, struct fqdb_store_maintenance_info *out)` | 定义或使用索引 |
| `全局` | `fqdb_status fqdb_store_index_drop(fqdb_store *store, fqdb_slice name, const struct fqdb_store_maintenance_options *options, struct fqdb_store_maintenance_info *out)` | 定义或使用索引 |

</details>

## 8. 关键写入：可靠确认

默认性能模式为50 ms /1 MiB，普通成功表示内存生效。最后的可靠标志传1：

突然断电时，尚未落盘的数据可能丢失；关键业务要等待下面的可靠确认。

```c
status = fqdb_store_put(db, bytes("payment/receipt"), bytes("paid"), 0, &info, 1);
status = fqdb_store_sync(db);
```

每个状态都要检查。COMMIT_UNKNOWN 时按业务编号核对，不要盲目重放付款。

<details>
<summary>函数与参数（7项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `fqdb_status fqdb_store_commit_poll(fqdb_store_commit *commit, fqdb_status *completion_status, uint64_t *version)` | 提交事务或查看提交结果 |
| `全局` | `fqdb_status fqdb_store_commit_wait(fqdb_store_commit *commit, uint64_t timeout_ms, fqdb_status *completion_status, uint64_t *version)` | 提交事务或查看提交结果 |
| `全局` | `fqdb_status fqdb_store_put_submit(fqdb_store *store, fqdb_slice key, fqdb_slice value, uint64_t expires_at_ms, fqdb_store_commit **out, uint32_t force_reliable)` | 保存或设置数据 |
| `全局` | `fqdb_status fqdb_store_sync(fqdb_store *store)` | 控制可靠确认或提交等待 |
| `全局` | `fqdb_status fqdb_store_write_configure(fqdb_store *store, const fqdb_store_write_options *options)` | 控制可靠确认或提交等待 |
| `全局` | `fqdb_status fqdb_store_write_options_get(fqdb_store *store, fqdb_store_write_options *out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_write_state_get(fqdb_store *store, fqdb_store_write_state *out)` | 读取对应的数据或状态 |

</details>

## 9. 到期数据

put 的 expires_at_ms 参数传绝对 Unix 毫秒；0 表示不过期。到期后读取不再可见。

<details>
<summary>函数与参数（2项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `fqdb_status fqdb_store_purge_expired(fqdb_store *store, const fqdb_store_expiry_options *options, fqdb_store_expiry_info *out)` | 到期数据操作 |
| `全局` | `uint64_t fqdb_store_value_expires_at_ms(const fqdb_store_value *value)` | 到期数据操作 |

</details>

## 10. 共享读取与复用缓冲

get_shared 返回只读租约，用完 value_close；key_prepare 复用键，reader_create 复用批读。读取器的行在下一次 acquire/release/close 后失效。

<details>
<summary>函数与参数（3项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `fqdb_status fqdb_store_get_shared(fqdb_store *store, fqdb_slice key, fqdb_store_shared_value *out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_key_get_shared(fqdb_store_key *key, fqdb_store_shared_value *out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_snapshot_get_shared(const fqdb_store_snapshot *snapshot, fqdb_slice key, fqdb_store_shared_value *out)` | 读取对应的数据或状态 |

</details>

## 11. 一致快照与遍历

snapshot 创建一致视图，cursor 按前缀或范围遍历。分别使用 snapshot_close、cursor_close 释放。

<details>
<summary>函数与参数（9项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `void fqdb_store_cursor_close(fqdb_store_cursor *cursor)` | 释放对象或借用的资源 |
| `全局` | `fqdb_status fqdb_store_cursor_next(fqdb_store_cursor *cursor, fqdb_store_row *out)` | 一致读取或范围遍历 |
| `全局` | `fqdb_status fqdb_store_cursor_open(const fqdb_store_snapshot *snapshot, fqdb_slice begin, fqdb_slice end, uint32_t reverse, fqdb_store_cursor **out)` | 一致读取或范围遍历 |
| `全局` | `void fqdb_store_snapshot_close(fqdb_store_snapshot *snapshot)` | 释放对象或借用的资源 |
| `全局` | `fqdb_status fqdb_store_snapshot_create(fqdb_store *store, fqdb_store_snapshot **out)` | 一致读取或范围遍历 |
| `全局` | `fqdb_status fqdb_store_snapshot_get(const fqdb_store_snapshot *snapshot, fqdb_slice key, fqdb_buffer *out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_snapshot_get_many(const fqdb_store_snapshot *snapshot, const fqdb_slice *keys, uint32_t count, fqdb_store_results **out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_snapshot_get_prepared_view(const fqdb_store_snapshot *snapshot, fqdb_store_key *key, fqdb_store_value **out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_snapshot_get_view(const fqdb_store_snapshot *snapshot, fqdb_slice key, fqdb_store_value **out)` | 读取对应的数据或状态 |

</details>

## 12. 备份和恢复

backup 写入新的备份目录；restore/recover_chain 恢复完整或增量备份。先关闭要替换的数据库，不复制正在写入的文件。

<details>
<summary>函数与参数（7项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `fqdb_status fqdb_store_backup(fqdb_store *store, const char *destination, const fqdb_store_maintenance_options *options, fqdb_store_maintenance_info *out)` | 备份、恢复或历史保留 |
| `全局` | `fqdb_status fqdb_store_backup_incremental(fqdb_store *store, const char *destination, uint64_t from_version, const fqdb_store_maintenance_options *options, fqdb_store_maintenance_info *out)` | 备份、恢复或历史保留 |
| `全局` | `fqdb_status fqdb_store_history_hold(fqdb_store *store, fqdb_slice name, uint64_t from_version, uint64_t timeout_ms, fqdb_store_atomic_info *out)` | 备份、恢复或历史保留 |
| `全局` | `fqdb_status fqdb_store_history_info_get(fqdb_store *store, fqdb_store_history_info *out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_history_release(fqdb_store *store, fqdb_slice name, uint64_t timeout_ms, fqdb_store_atomic_info *out)` | 释放对象或借用的资源 |
| `全局` | `fqdb_status fqdb_store_recover_chain(const char *base, const char *const *archives, uint32_t archive_count, const char *destination, uint64_t target_version, uint64_t target_unix_ms, const fqdb_store_maintenance_options *options, fqdb_store_maintenance_info *out)` | 备份、恢复或历史保留 |
| `全局` | `fqdb_status fqdb_store_restore(const char *source, const char *destination, const fqdb_store_maintenance_options *options, fqdb_store_maintenance_info *out)` | 备份、恢复或历史保留 |

</details>

## 13. 原生集合

execute 处理 Hash/Set/List/有序集合命令，hash_get 读取 Hash 字段。适合原子成员更新、队列和排名。

<details>
<summary>函数与参数（1项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `fqdb_status fqdb_store_hash_get_view(fqdb_store *store, fqdb_slice scope, fqdb_slice key, fqdb_store_value **out)` | 读取对应的数据或状态 |

</details>

## 14. 整理、清理与检查

checkpoint 创建检查点，compact 整理空间，purge_expired 分批清理到期记录，verify 校验文件。options 可限制一次维护工作量。

<details>
<summary>函数与参数（7项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `fqdb_status fqdb_log_get_info(fqdb_log_handle *handle, fqdb_log_info *out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_checkpoint(fqdb_store *store)` | 查看状态或维护数据库 |
| `全局` | `fqdb_status fqdb_store_compact(fqdb_store *store, const fqdb_store_maintenance_options *options, fqdb_store_maintenance_info *out)` | 查看状态或维护数据库 |
| `全局` | `fqdb_status fqdb_store_configure(fqdb_store *store, const fqdb_store_runtime_options *options)` | 查看状态或维护数据库 |
| `全局` | `fqdb_status fqdb_store_get_info(fqdb_store *store, fqdb_store_info *out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_runtime_info_get(fqdb_store *store, fqdb_store_runtime_info *out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_verify(fqdb_store *store, const fqdb_store_maintenance_options *options, fqdb_store_maintenance_info *out)` | 查看状态或维护数据库 |

</details>

## 15. 提交变化和日志

watch 消费提交变化，ack 确认处理水位，watch_close 释放消费者。fqdb_log_* 设置日志回调，用于问题定位。

<details>
<summary>函数与参数（5项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `fqdb_status fqdb_store_watch_ack(fqdb_store_watch *watch, uint64_t version)` | 消费变更或定位问题 |
| `全局` | `void fqdb_store_watch_cancel(fqdb_store_watch *watch)` | 消费变更或定位问题 |
| `全局` | `void fqdb_store_watch_close(fqdb_store_watch *watch)` | 释放对象或借用的资源 |
| `全局` | `fqdb_status fqdb_store_watch_next(fqdb_store_watch *watch, uint64_t timeout_ms, fqdb_store_change_batch **out)` | 消费变更或定位问题 |
| `全局` | `fqdb_status fqdb_store_watch_open(fqdb_store *store, uint64_t from_version, fqdb_slice key_prefix, const fqdb_store_watch_options *options, fqdb_store_watch **out)` | 消费变更或定位问题 |

</details>

## 16. 编解码与资源释放

生成的 C 解码记录借用原输入，输入必须保持存活且不可修改。再次编码传 row.unknown_source 保留未知字段。编码输出不能与输入重叠。

普通读取的 buffer 用 fqdb_buffer_free；共享值用 value_close；不要混用释放函数。键最长64 KiB，值最长16 MiB。

## 其他辅助接口

<details>
<summary>函数与参数（27项）</summary>

| 所属类型 | 函数与参数 | 用途 |
| --- | --- | --- |
| `全局` | `uint32_t fqdb_abi_version()` | 辅助操作 |
| `全局` | `fqdb_status fqdb_buffer_copy(fqdb_slice source, fqdb_buffer *out)` | 辅助操作 |
| `全局` | `void fqdb_buffer_free(fqdb_buffer *buffer)` | 辅助操作 |
| `全局` | `fqdb_status fqdb_log_close(fqdb_log_handle *handle)` | 释放对象或借用的资源 |
| `全局` | `fqdb_status fqdb_log_open(const fqdb_log_options *options, fqdb_log_handle **out)` | 辅助操作 |
| `全局` | `const char * fqdb_status_message(fqdb_status status)` | 辅助操作 |
| `全局` | `void fqdb_store_cancel_maintenance(fqdb_store *store)` | 辅助操作 |
| `全局` | `void fqdb_store_close(fqdb_store *store)` | 释放对象或借用的资源 |
| `全局` | `void fqdb_store_commit_cancel(fqdb_store_commit *commit)` | 提交事务或查看提交结果 |
| `全局` | `void fqdb_store_commit_close(fqdb_store_commit *commit)` | 释放对象或借用的资源 |
| `全局` | `void fqdb_store_key_close(fqdb_store_key *key)` | 释放对象或借用的资源 |
| `全局` | `fqdb_status fqdb_store_key_get_into(fqdb_store_key *key, void *destination, uint64_t capacity, uint64_t *required)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_key_get_view(fqdb_store_key *key, fqdb_store_value **out)` | 读取对应的数据或状态 |
| `全局` | `fqdb_status fqdb_store_key_prepare(fqdb_store *store, fqdb_slice key, fqdb_store_key **out)` | 辅助操作 |
| `全局` | `fqdb_status fqdb_store_open(const char *directory, const fqdb_store_options *options, fqdb_store **out)` | 辅助操作 |
| `全局` | `fqdb_status fqdb_store_open_at(const char *root, const char *name, const fqdb_store_options *options, fqdb_store **out)` | 辅助操作 |
| `全局` | `fqdb_status fqdb_store_path_at(const char *root, const char *name, fqdb_buffer *out)` | 辅助操作 |
| `全局` | `fqdb_status fqdb_store_project(fqdb_store *store, fqdb_slice key, const uint32_t *fields, uint32_t field_count, fqdb_buffer *out)` | 辅助操作 |
| `全局` | `fqdb_status fqdb_store_project_reader_create(fqdb_store *store, const fqdb_slice *keys, uint32_t count, const uint32_t *fields, uint32_t field_count, fqdb_store_reader **out)` | 辅助操作 |
| `全局` | `void fqdb_store_results_close(fqdb_store_results *results)` | 释放对象或借用的资源 |
| `全局` | `uint32_t fqdb_store_results_count(const fqdb_store_results *results)` | 辅助操作 |
| `全局` | `const fqdb_store_result_row * fqdb_store_results_data(const fqdb_store_results *results)` | 辅助操作 |
| `全局` | `fqdb_status fqdb_store_results_get(const fqdb_store_results *results, uint32_t index, fqdb_store_result_row *out)` | 读取对应的数据或状态 |
| `全局` | `void fqdb_store_value_close(fqdb_store_value *value)` | 释放对象或借用的资源 |
| `全局` | `fqdb_slice fqdb_store_value_data(const fqdb_store_value *value)` | 辅助操作 |
| `全局` | `uint64_t fqdb_store_value_version(const fqdb_store_value *value)` | 辅助操作 |
| `全局` | `fqdb_text fqdb_text_from_cstr(const char *value)` | 辅助操作 |

</details>

## 参数类型、返回值和常量

需要设置 options 或检查返回状态时在这里查字段。普通增删改查不需要逐项阅读。

<details>
<summary>fqdb_buffer</summary>

| 声明 |
| --- |
| `void * data` |
| `uint64_t size` |

</details>

<details>
<summary>fqdb_log_event</summary>

| 声明 |
| --- |
| `uint32_t abi_version` |
| `uint64_t base_version` |
| `uint64_t database_id` |
| `uint32_t durability` |
| `uint64_t duration_us` |
| `uint64_t file_offset` |
| `uint32_t level` |
| `uint32_t operation` |
| `uint64_t operation_count` |
| `uint32_t os_error` |
| `uint64_t payload_bytes` |
| `uint64_t repaired_bytes` |
| `uint32_t stage` |
| `fqdb_status status` |
| `uint32_t struct_size` |
| `uint64_t version` |

</details>

<details>
<summary>fqdb_log_info</summary>

| 声明 |
| --- |
| `uint64_t active` |
| `uint64_t callback_errors` |
| `uint64_t delivered` |

</details>

<details>
<summary>fqdb_log_options</summary>

| 声明 |
| --- |
| `uint32_t abi_version` |
| `fqdb_log_callback callback` |
| `uint32_t min_level` |
| `uint32_t reserved` |
| `uint64_t slow_commit_us` |
| `uint32_t struct_size` |
| `void * user` |

</details>

<details>
<summary>fqdb_slice</summary>

| 声明 |
| --- |
| `const void * data` |
| `uint64_t size` |

</details>

<details>
<summary>fqdb_store_atomic_info</summary>

| 声明 |
| --- |
| `uint32_t changed` |
| `uint32_t matched` |
| `uint64_t observed_version` |
| `int64_t value` |
| `uint64_t version` |

</details>

<details>
<summary>fqdb_store_change_batch_info</summary>

| 声明 |
| --- |
| `uint32_t count` |
| `uint64_t durable_version` |
| `uint32_t flags` |
| `const fqdb_store_change_row * rows` |
| `uint64_t unix_ms` |
| `uint64_t version` |

</details>

<details>
<summary>fqdb_store_change_row</summary>

| 声明 |
| --- |
| `uint64_t expires_at_ms` |
| `fqdb_slice key` |
| `uint32_t kind` |
| `uint32_t reserved` |
| `fqdb_slice value` |

</details>

<details>
<summary>fqdb_store_expiry_info</summary>

| 声明 |
| --- |
| `uint64_t cutoff_ms` |
| `uint64_t more` |
| `uint64_t removed_keys` |
| `uint64_t removed_value_bytes` |
| `uint64_t version` |
| `uint64_t visited_pages` |

</details>

<details>
<summary>fqdb_store_expiry_options</summary>

| 声明 |
| --- |
| `uint32_t abi_version` |
| `uint64_t max_batch_bytes` |
| `uint32_t max_keys` |
| `uint32_t reserved` |
| `uint32_t struct_size` |
| `uint64_t timeout_ms` |

</details>

<details>
<summary>fqdb_store_history_info</summary>

| 声明 |
| --- |
| `uint64_t active_pins` |
| `uint64_t floor` |
| `uint64_t oldest_pin` |
| `uint64_t retained_file_bytes` |
| `uint64_t retained_generations` |
| `uint64_t total_pins` |
| `uint64_t version` |

</details>

<details>
<summary>fqdb_store_i64_field_update</summary>

| 声明 |
| --- |
| `int64_t delta` |
| `fqdb_slice key` |
| `int64_t maximum` |
| `int64_t minimum` |
| `uint64_t offset` |

</details>

<details>
<summary>fqdb_store_index_field</summary>

| 声明 |
| --- |
| `uint32_t field` |
| `uint32_t type` |

</details>

<details>
<summary>fqdb_store_info</summary>

| 声明 |
| --- |
| `uint64_t cache_bytes` |
| `uint64_t delta_bytes` |
| `uint64_t durable_version` |
| `uint64_t file_bytes` |
| `uint64_t indexed_keys` |
| `uint64_t pinned_bytes` |
| `uint64_t recovered_transactions` |
| `uint64_t repaired_bytes` |
| `uint64_t stored_keys` |
| `uint64_t stored_value_bytes` |
| `uint64_t transaction_bytes` |
| `uint64_t version` |

</details>

<details>
<summary>fqdb_store_maintenance_info</summary>

| 声明 |
| --- |
| `uint64_t catchup_rounds` |
| `uint64_t copied_keys` |
| `uint64_t final_version` |
| `uint64_t output_file_bytes` |
| `uint64_t snapshot_version` |
| `uint64_t source_file_bytes` |
| `uint64_t stored_keys` |
| `uint64_t stored_value_bytes` |

</details>

<details>
<summary>fqdb_store_maintenance_io</summary>

| 声明 |
| --- |
| `uint64_t read_bytes` |
| `uint64_t sync_calls` |
| `uint64_t write_bytes` |

</details>

<details>
<summary>fqdb_store_maintenance_options</summary>

| 声明 |
| --- |
| `uint32_t abi_version` |
| `fqdb_store_maintenance_io * io_stats` |
| `uint64_t max_barrier_bytes` |
| `uint32_t max_catchup_rounds` |
| `uint64_t max_read_bytes` |
| `uint64_t max_write_bytes` |
| `uint32_t reserved` |
| `uint32_t struct_size` |
| `uint64_t timeout_ms` |

</details>

<details>
<summary>fqdb_store_options</summary>

| 声明 |
| --- |
| `uint32_t abi_version` |
| `uint64_t all_transaction_bytes` |
| `uint64_t cache_bytes` |
| `uint64_t delta_bytes` |
| `uint32_t durability` |
| `uint32_t flags` |
| `uint64_t max_snapshots` |
| `uint64_t pinned_bytes` |
| `uint32_t struct_size` |
| `uint64_t transaction_bytes` |
| `uint64_t writer_bytes` |

</details>

<details>
<summary>fqdb_store_put_item</summary>

| 声明 |
| --- |
| `uint64_t expires_at_ms` |
| `fqdb_slice key` |
| `fqdb_slice value` |

</details>

<details>
<summary>fqdb_store_result_row</summary>

| 声明 |
| --- |
| `uint64_t expires_at_ms` |
| `uint32_t found` |
| `uint32_t reserved` |
| `fqdb_slice value` |
| `uint64_t version` |

</details>

<details>
<summary>fqdb_store_row</summary>

| 声明 |
| --- |
| `uint64_t expires_at_ms` |
| `fqdb_slice key` |
| `fqdb_slice value` |
| `uint64_t version` |

</details>

<details>
<summary>fqdb_store_runtime_info</summary>

| 声明 |
| --- |
| `uint64_t compactions` |
| `uint64_t expiry_batches` |
| `uint64_t group_flushes` |
| `uint64_t group_transactions` |
| `uint64_t last_status` |
| `uint64_t running` |

</details>

<details>
<summary>fqdb_store_runtime_options</summary>

| 声明 |
| --- |
| `uint32_t abi_version` |
| `uint32_t background` |
| `uint64_t compact_growth_bytes` |
| `uint64_t compact_min_bytes` |
| `uint32_t expiry_keys` |
| `uint32_t garbage_percent` |
| `uint32_t group_delay_us` |
| `uint32_t group_max` |
| `uint32_t interval_ms` |
| `uint64_t job_max_read_bytes` |
| `uint64_t job_max_write_bytes` |
| `uint64_t job_timeout_ms` |
| `uint32_t struct_size` |

</details>

<details>
<summary>fqdb_store_shared_value</summary>

| 声明 |
| --- |
| `fqdb_store_value * owner` |
| `fqdb_store_result_row row` |

</details>

<details>
<summary>fqdb_store_watch_options</summary>

| 声明 |
| --- |
| `uint32_t abi_version` |
| `uint32_t flags` |
| `uint64_t max_batch_bytes` |
| `uint32_t max_scan_commits` |
| `uint32_t struct_size` |

</details>

<details>
<summary>fqdb_store_write_options</summary>

| 声明 |
| --- |
| `uint32_t abi_version` |
| `uint32_t mode` |
| `uint32_t period_ms` |
| `uint32_t struct_size` |
| `uint64_t threshold_bytes` |

</details>

<details>
<summary>fqdb_store_write_state</summary>

| 声明 |
| --- |
| `uint64_t durable_version` |
| `uint64_t last_status` |
| `uint64_t pending_bytes` |
| `uint64_t version` |

</details>

<details>
<summary>全局</summary>

| 声明 |
| --- |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_ALREADY_EXISTS` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_ASYNC_REQUIRED` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_BUFFER_TOO_SMALL` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_CANCELLED` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_COLLECTION_CLEAR` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_COLLECTION_SIZE` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_COMMIT_UNKNOWN` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_CONFLICT` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_CONSTRAINT` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_CORRUPT` |
| `(anonymous enum at include/fengqudb.h:54:1) FQDB_DELETE` |
| `(anonymous enum at include/fengqudb.h:53:1) FQDB_DURABILITY_BUFFERED` |
| `(anonymous enum at include/fengqudb.h:53:1) FQDB_DURABILITY_SYNC` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_END` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_HASH_DELETE` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_HASH_GET` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_HASH_SCAN` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_HASH_SET` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_HISTORY_LOST` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_INDEX_LOOKUP` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_INDEX_QUERY` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_INTERNAL_ERROR` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_INVALID_ARGUMENT` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_IO_ERROR` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_LIMIT` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_LIST_GET` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_LIST_POP_LEFT` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_LIST_POP_RIGHT` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_LIST_PUSH_LEFT` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_LIST_PUSH_RIGHT` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_LIST_RANGE` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_LIST_SET` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_LOCKED` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_BACKUP` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_BACKUP_INCREMENTAL` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_CHANGES` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_CHECKPOINT` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_COMMIT` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_COMPACT` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_CONFIGURE` |
| `(anonymous enum at include/fengqudb_log.h:7:1) FQDB_LOG_ERROR` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_INDEX_DEFINE` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_INDEX_DROP` |
| `(anonymous enum at include/fengqudb_log.h:7:1) FQDB_LOG_INFO` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_OPEN` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_PURGE_EXPIRED` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_RECOVER_CHAIN` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_RESTORE` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_SYNC` |
| `(anonymous enum at include/fengqudb_log.h:8:1) FQDB_LOG_VERIFY` |
| `(anonymous enum at include/fengqudb_log.h:7:1) FQDB_LOG_WARNING` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_NOT_FOUND` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_NOT_MODIFIED` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_NO_MEMORY` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_OK` |
| `(anonymous enum at include/fengqudb.h:54:1) FQDB_PUT` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_RECORD_PROJECT` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_RECOVERY_REQUIRED` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SELECT` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SELECT_EXPLAIN` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SET_ADD` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SET_CONTAINS` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SET_REMOVE` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SET_SCAN` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SORTED_ADD` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SORTED_RANGE` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SORTED_REMOVE` |
| `(anonymous enum at include\fengqudb_store.h:80:1) FQDB_SORTED_SCORE` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_ARGUMENTS` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_CHECKSUM` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_COMPLETE` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_FILE_HEADER` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_FLUSH` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_INDEX` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_OPEN` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_PAYLOAD` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_PUBLISH` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_RECORD_HEADER` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_REPAIR` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_TRAILER` |
| `(anonymous enum at include/fengqudb_log.h:25:1) FQDB_STAGE_WRITE` |
| `(anonymous enum at include\fengqudb_store.h:149:1) FQDB_STORE_CREATE = 1` |
| `(anonymous enum at include\fengqudb_store.h:149:1) FQDB_STORE_SEGMENTED = 2` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_TIMEOUT` |
| `(anonymous enum at include/fengqudb.h:29:1) FQDB_UNSUPPORTED` |
| `(anonymous enum at include\fengqudb_store.h:373:1) FQDB_WATCH_INCLUDE_SYSTEM = 2` |
| `(anonymous enum at include\fengqudb_store.h:373:1) FQDB_WATCH_METADATA_ONLY = 1` |
| `(anonymous enum at include\fengqudb_store.h:24:1) FQDB_WRITE_PERFORMANCE` |
| `(anonymous enum at include\fengqudb_store.h:24:1) FQDB_WRITE_RELIABLE` |
| `struct fqdb_buffer` |
| `struct fqdb_buffer fqdb_buffer` |
| `struct fqdb_bytes fqdb_bytes` |
| `void (*)(void *, const fqdb_log_event *) fqdb_log_callback` |
| `struct fqdb_log_event` |
| `struct fqdb_log_event fqdb_log_event` |
| `struct fqdb_log_handle` |
| `struct fqdb_log_handle fqdb_log_handle` |
| `struct fqdb_log_info` |
| `struct fqdb_log_info fqdb_log_info` |
| `struct fqdb_log_options` |
| `struct fqdb_log_options fqdb_log_options` |
| `struct fqdb_slice` |
| `struct fqdb_slice fqdb_slice` |
| `int32_t fqdb_status` |
| `struct fqdb_store` |
| `struct fqdb_store fqdb_store` |
| `struct fqdb_store_atomic_info` |
| `struct fqdb_store_atomic_info fqdb_store_atomic_info` |
| `struct fqdb_store_batch` |
| `struct fqdb_store_batch fqdb_store_batch` |
| `struct fqdb_store_change_batch` |
| `struct fqdb_store_change_batch fqdb_store_change_batch` |
| `struct fqdb_store_change_batch_info` |
| `struct fqdb_store_change_batch_info fqdb_store_change_batch_info` |
| `struct fqdb_store_change_row` |
| `struct fqdb_store_change_row fqdb_store_change_row` |
| `struct fqdb_store_commit` |
| `struct fqdb_store_commit fqdb_store_commit` |
| `struct fqdb_store_cursor` |
| `struct fqdb_store_cursor fqdb_store_cursor` |
| `struct fqdb_store_expiry_info` |
| `struct fqdb_store_expiry_info fqdb_store_expiry_info` |
| `struct fqdb_store_expiry_options` |
| `struct fqdb_store_expiry_options fqdb_store_expiry_options` |
| `struct fqdb_store_history_info` |
| `struct fqdb_store_history_info fqdb_store_history_info` |
| `struct fqdb_store_i64_field_update` |
| `struct fqdb_store_i64_field_update fqdb_store_i64_field_update` |
| `struct fqdb_store_index_field` |
| `struct fqdb_store_index_field fqdb_store_index_field` |
| `struct fqdb_store_info` |
| `struct fqdb_store_info fqdb_store_info` |
| `struct fqdb_store_key` |
| `struct fqdb_store_key fqdb_store_key` |
| `struct fqdb_store_maintenance_info` |
| `struct fqdb_store_maintenance_info fqdb_store_maintenance_info` |
| `struct fqdb_store_maintenance_io` |
| `struct fqdb_store_maintenance_io fqdb_store_maintenance_io` |
| `struct fqdb_store_maintenance_options` |
| `struct fqdb_store_maintenance_options fqdb_store_maintenance_options` |
| `struct fqdb_store_options` |
| `struct fqdb_store_options fqdb_store_options` |
| `struct fqdb_store_put_item` |
| `struct fqdb_store_put_item fqdb_store_put_item` |
| `struct fqdb_store_reader` |
| `struct fqdb_store_reader fqdb_store_reader` |
| `struct fqdb_store_result_row` |
| `struct fqdb_store_result_row fqdb_store_result_row` |
| `struct fqdb_store_results` |
| `struct fqdb_store_results fqdb_store_results` |
| `struct fqdb_store_row` |
| `struct fqdb_store_row fqdb_store_row` |
| `struct fqdb_store_runtime_info` |
| `struct fqdb_store_runtime_info fqdb_store_runtime_info` |
| `struct fqdb_store_runtime_options` |
| `struct fqdb_store_runtime_options fqdb_store_runtime_options` |
| `struct fqdb_store_shared_value` |
| `struct fqdb_store_shared_value fqdb_store_shared_value` |
| `struct fqdb_store_snapshot` |
| `struct fqdb_store_snapshot fqdb_store_snapshot` |
| `struct fqdb_store_txn` |
| `struct fqdb_store_txn fqdb_store_txn` |
| `struct fqdb_store_value` |
| `struct fqdb_store_value fqdb_store_value` |
| `struct fqdb_store_watch` |
| `struct fqdb_store_watch fqdb_store_watch` |
| `struct fqdb_store_watch_options` |
| `struct fqdb_store_watch_options fqdb_store_watch_options` |
| `struct fqdb_store_write_options` |
| `struct fqdb_store_write_options fqdb_store_write_options` |
| `struct fqdb_store_write_state` |
| `struct fqdb_store_write_state fqdb_store_write_state` |
| `struct fqdb_text fqdb_text` |

</details>

<details>
<summary>查询格式：store_select</summary>

```protobuf
syntax = "proto3";
package fengqudb.api;
// C ABI: txn_execute operation 53 executes, 54 explains. The request is in
// FeatureRequest.value; FeatureResult.value contains SelectResult.
message QueryValue { uint32 kind=1; uint64 uint_value=2; sint64 int_value=3; double real_value=4; bytes bytes_value=5; }
message QueryField { uint32 source=1; uint32 type=2; repeated uint32 path=3; }
message QueryExpr { uint32 op=1; QueryField field=2; repeated QueryValue values=3; repeated QueryExpr children=4; optional QueryField right=5; }
message QueryColumn { QueryField field=1; uint32 aggregate=2; string name=3; bool distinct=4; }
message QueryOrder { QueryField field=1; bool descending=2; bool nulls_first=3; }
message QueryJoin { bytes prefix=1; repeated QueryField left=2; repeated QueryField right=3; uint32 kind=4; QueryExpr on=5; }
// SDK encoders explicitly emit defaults; limits cannot be zero.
message Select {
  bytes prefix=1; QueryExpr where=2; repeated QueryColumn columns=3;
  repeated QueryOrder order=4; repeated QueryField group=5; QueryExpr having=6;
  repeated QueryJoin joins=7; uint64 limit=8; uint64 offset=9;
  uint64 memory_bytes=10; uint64 max_candidates=11; uint64 max_work=12;
  uint64 timeout_ms=13; bool allow_scan=14; bool count_total=15;
  bool distinct=16; bytes index_hint=17; bytes after=18;
}
message QueryRow { bytes key=1; repeated QueryValue values=2; }
message QueryAccess { uint32 source=1; string strategy=2; string index=3; string reason=4; bool covering=5; bool ordered=6; bool needs_scan=7; uint64 sampled_candidates=8; }
message QueryStats { uint64 candidates=1; uint64 matched=2; uint64 result_total=3; uint64 work=4; uint64 peak_memory=5; uint64 payload_reads=6; uint64 join_probes=7; bool complete=8; }
message SelectResult { repeated QueryRow rows=1; repeated QueryAccess access=2; QueryStats stats=3; bytes cursor=4; }
```

</details>

<details>
<summary>查询格式：store_query</summary>

```protobuf
syntax = "proto3";
package fengqudb.api;

// The inner messages are encoded using the application's own protobuf schema.
// Only a contiguous prefix of index fields belongs in equal. Lower/upper each
// contain exactly the next indexed field. Presence distinguishes no bound.
message IndexQuery {
  bytes equal = 1;
  optional bytes lower = 2;
  optional bytes upper = 3;
  uint32 flags = 4; // 1 lower exclusive, 2 upper inclusive, 4 reverse, 8 values
  repeated uint32 fields = 5; // top-level projection; packed/unpacked accepted
}
```

</details>
