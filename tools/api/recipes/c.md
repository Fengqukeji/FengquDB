<!-- api:read -->
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

<!-- api:save -->
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

<!-- api:modify -->
## 3. 修改与原子计数

整份修改再次 put 即可。patch 修改原始字节区间，compare_exchange 按版本修改，add_i64 对专用八字节整数原子加减。编码失败或缓冲不足时，不要把部分输出写入库。

<!-- api:delete -->
## 4. 删除

```c
fqdb_store_atomic_info info = {0};
fqdb_status status = fqdb_store_delete(db, bytes("student/7"), &info, 0);
```

<!-- api:transaction -->
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

<!-- api:query -->
## 6. 条件查询

fqdb_store_select 接收二进制查询请求，返回二进制结果；用于组合筛选、排序、分组、统计与等值关联。C 接口不提供类型化查询构建器。请求和结果字段定义在本节末尾的查询格式中。

<!-- api:index -->
## 7. 字段索引

index_define/index_define_compound 为常用条件建立索引，并可设置唯一、必填等约束；index_drop 删除索引。

<!-- api:durability -->
## 8. 关键写入：可靠确认

默认性能模式为50 ms /1 MiB，普通成功表示内存生效。最后的可靠标志传1：

突然断电时，尚未落盘的数据可能丢失；关键业务要等待下面的可靠确认。

```c
status = fqdb_store_put(db, bytes("payment/receipt"), bytes("paid"), 0, &info, 1);
status = fqdb_store_sync(db);
```

每个状态都要检查。COMMIT_UNKNOWN 时按业务编号核对，不要盲目重放付款。

<!-- api:expiry -->
## 9. 到期数据

put 的 expires_at_ms 参数传绝对 Unix 毫秒；0 表示不过期。到期后读取不再可见。

<!-- api:fastread -->
## 10. 共享读取与复用缓冲

get_shared 返回只读租约，用完 value_close；key_prepare 复用键，reader_create 复用批读。读取器的行在下一次 acquire/release/close 后失效。

<!-- api:snapshot -->
## 11. 一致快照与遍历

snapshot 创建一致视图，cursor 按前缀或范围遍历。分别使用 snapshot_close、cursor_close 释放。

<!-- api:backup -->
## 12. 备份和恢复

backup 写入新的备份目录；restore/recover_chain 恢复完整或增量备份。先关闭要替换的数据库，不复制正在写入的文件。

<!-- api:collections -->
## 13. 原生集合

execute 处理 Hash/Set/List/有序集合命令，hash_get 读取 Hash 字段。适合原子成员更新、队列和排名。

<!-- api:maintenance -->
## 14. 整理、清理与检查

checkpoint 创建检查点，compact 整理空间，purge_expired 分批清理到期记录，verify 校验文件。options 可限制一次维护工作量。

<!-- api:watch -->
## 15. 提交变化和日志

watch 消费提交变化，ack 确认处理水位，watch_close 释放消费者。fqdb_log_* 设置日志回调，用于问题定位。

<!-- api:serialization -->
## 16. 编解码与资源释放

生成的 C 解码记录借用原输入，输入必须保持存活且不可修改。再次编码传 row.unknown_source 保留未知字段。编码输出不能与输入重叠。

普通读取的 buffer 用 fqdb_buffer_free；共享值用 value_close；不要混用释放函数。键最长64 KiB，值最长16 MiB。
