# Python 与 Java 的复用读取

不需要可写目标缓冲时，可使用 [共享只读读取](STORE_SHARED_READ.md) 避免载荷复制；本文介绍的目标复用接口仍保持原有生命周期与预算行为。

准备键保留键查找所需的对象；重复读取时可复用业务目标缓冲，减少分配和跨语言封送。这些接口仍复制值字节，不占用共享值视图的固定内存预算，也不会改变原来 `get`/`Get` 的所有权和错误契约。核心的完整私有二进制缓冲覆盖与减法裁剪保持原样。

## Python 固定目标

```python
with db.prepare_key(b"player/42") as key:
    target = bytearray(8192)
    with key.bind_buffer(target) as reader:
        count = reader.read()
        if count is not None:
            consume(memoryview(target)[:count])
```

`read()` 返回写入字节数；空值返回 0，缺失或到期返回 None。容量不足抛出 `BufferTooSmall(required_size)`，不部分写入；成功只更新有效前缀，尾部保留原样。不要解析尾部残留的旧数据。示例中的 consume 代表业务处理，memoryview 若被保留也会阻止 bytearray 调整大小。

绑定期间持有 bytearray 的缓冲导出，调整大小会抛 BufferError，防止指针失效。先关闭读取器再调整大小或换目标。读取器复用 ctypes 数组、容量和输出参数；只在单次调用中获取原生句柄租约，因此 key.close 不会等待一个闲置读取器析构。读取器保留准备键的 Python 包装对象；关闭读取器不关闭准备键，显式关闭准备键后 read 拒绝调用。

同一个读取器的 read 与 close 串行化；对目标缓冲的其他读写须由业务同步。多个读取线程各用自己的读取器和目标。父 Store 关闭后，未关闭的准备键仍保持底层库的生命周期；所有子句柄关闭后才能重新打开该目录。

普通 `key.get_into(bytearray)` 不变，适用于临时目标；固定读取器是显式选择，不给原路径增加额外导出和锁。

## Java direct ByteBuffer

```java
try (Store.PreparedKey key = db.prepareKey("player/42".getBytes(StandardCharsets.UTF_8))) {
    ByteBuffer target = ByteBuffer.allocateDirect(8192);
    long count = key.getIntoDirect(target);
    if (count >= 0) {
        ByteBuffer valid = target.duplicate();
        valid.limit(valid.position() + (int) count);
        consume(valid);
    }
}
```

须传入可写 direct ByteBuffer，支持 slice。写入范围为调用时 position 到 limit，返回字节数或缺失时的 -1；空值返回 0。position、limit 和有效范围之外的字节保持不变。容量不足抛 DbException(code=16)，不写入部分值；null、heap buffer、read-only buffer 在 Java 层拒绝。JNI 再次验证地址、容量和偏移范围。

JNI 调用的局部引用保持 direct 内存有效，不使用 GetPrimitiveArrayCritical 包围可能进行 I/O 的读取。读取与 key.close 通过现有句柄租约协调；目标的并发访问、position/limit 修改由业务同步。direct 内存由 JVM 管理，建议每个长期工作线程复用少量目标，不要每次读取都 allocateDirect。原 `getInto(byte[])` 保持兼容，使用独立方法名避免 `getInto(null)` 重载歧义。

验证入口为 `scripts/verify-store-buffers.py`，传入匹配的生产核心库、JNI、JDK/bin 和新的输出目录；覆盖固定导出、容量、空值/缺失、整理、父子生命周期与并发关闭。业务成本驱动器 `scripts/verify-business-protobuf.py` 另测固定 Python 参数和 Java direct 路径，结果只代表指定载荷与运行条件。
