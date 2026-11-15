<!-- api:read -->
## 1. 读取

```js
const data = db.get('student/7');
const student = data === null ? null : Codec.decode(data).value;
```

缺失返回 null，空 Uint8Array 是有效数据。Codec 是接入文档生成的 StudentFqCodec。

<!-- api:save -->
## 2. 新增、保存、单个字符串和整数

```js
db.put(Codec.key(student), Codec.encode(student));
db.put('name', '张三');
const number = Buffer.alloc(8); number.writeBigInt64LE(95n);
db.put('score', number);
const score = Buffer.from(db.get('score')).readBigInt64LE();
```

同键 put 覆盖原值。64位整数使用 bigint。

<!-- api:modify -->
## 3. 修改

```js
const bytes = db.get('student/7');
if (bytes !== null) {
  const row = Codec.decode(bytes);
  row.value.score = 96;
  db.put(row.fqKey(), row.fqEncode());
}
```

多键修改放在事务中；当前 Node 包没有原子计数和二进制 patch 的便捷方法。

<!-- api:delete -->
## 4. 删除

```js
const removed = db.delete('student/7');
```

<!-- api:batch -->
## 5. 批量修改

```js
const tx = db.transaction();
try { tx.putMany([['a','one'],['b','two']]); tx.commit(); }
finally { tx.close(); }
```

整批修改作为同一笔事务提交。

<!-- api:transaction -->
## 6. 事务与局部回退

```js
const tx = db.transaction();
try {
  tx.put('a','one');
  const point = tx.savepoint();
  tx.put('temporary','discard');
  tx.rollbackTo(point);
  tx.commit();
} finally { tx.close(); }
```

commit 会关闭事务；未提交即 close 会丢弃。CONFLICT 时重新创建事务并重新读取计算。

<!-- api:query -->
## 7. 条件筛选、排序、统计

```js
const {from, s64, text} = require('./FengquDB/node');
const score = s64(Codec.fields.score), name = text(Codec.fields.name);
const rows = from('student/').where(score.ge(90n))
  .select(name,score).orderBy(score.desc()).limit(10).run(db);
try { for(const row of rows) console.log(row.get(name),row.get(score)); }
finally { rows.close(); }
```

and/or/not 组合条件，groupBy/having 分组统计，join 做等值关联；prepare 复用查询，explain 查看是否走索引。查询列类型应匹配编码，普通 number 字段默认用 f64。

<!-- api:index -->
## 8. 索引

```js
db.defineCompoundIndex('student-score', 'student/', [[Codec.fields.score,5]], 0);
```

5 表示有符号 ZigZag 整数。

<!-- api:durability -->
## 9. 关键写入：等待落盘

默认性能模式为 50 ms / 1 MiB，普通成功表示内存已生效。

突然断电时，尚未落盘的数据可能丢失；关键业务要等待下面的可靠确认。

```js
db.put('payment/receipt', 'paid', {forceReliable:true});
db.sync();
```

事务使用 commit(true)。整库切换用 `db.configureWrite({mode:'reliable'})`，性能模式用 `db.configureWrite({mode:'performance', periodMs:50, thresholdBytes:1024*1024})`；省略的参数保留当前值。`db.writeOptions()` 读取配置，`db.resetWriteOptions()` 恢复默认性能模式。配置会保存，重开后保留。

接口是同步的，可靠写与大查询应放到 worker，避免阻塞事件循环。COMMIT_UNKNOWN 时先按业务编号核对，不要重复扣款。

<!-- api:expiry -->
## 10. 到期数据

```js
db.put('verification', '1234', {expiresAtMs:Date.now()+300_000});
```

到期时间为绝对 Unix 毫秒，0 表示不过期。

<!-- api:fastread -->
## 11. 热点读取与共享字节

```js
db.withShared('student/7', bytes => { if(bytes) consume(bytes); });
const reader = db.prepareReader(['student/7','student/8']);
try { reader.read(); if(reader.found(0)) consume(reader.value(0)); }
finally { reader.close(); }
```

共享字节只读；回调结束或释放后不能继续用。读取器视图只保留到下一次 read/release/close，需要跨次保存就复制。

<!-- api:snapshot -->
## 12. 一致快照

```js
const snapshot = db.snapshot();
try { const value = snapshot.get('student/7'); }
finally { snapshot.close(); }
```

<!-- api:serialization -->
## 13. 实体编解码

Codec.encode/decode 操作原 Student。读改写时用记录的 fqEncode 保留未知字段；encode(value, destination) 可复用 Uint8Array，返回实际编码区间。自动生成由 npm prebuild 执行，不需要额外安装生成器。
