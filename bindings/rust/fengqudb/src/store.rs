//! Store、事务、快照、索引。

use std::ffi::CString;
use std::path::Path;
use std::ptr::{self, NonNull};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use fengqudb_sys as sys;

use crate::buffer::Buffer;
use crate::error::{check, Error, Result};
use crate::shared::{PreparedKey, Reader, SharedValue};

/// 持久化模式。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Durability {
    /// 每次提交同步落盘。
    Sync = 0,
    /// 缓冲写入，手动 `sync()`。
    Buffered = 1,
}

/// 打开选项；`Default` 对应头文件中 NULL 选项的默认值（SYNC，不存在则创建）。
#[derive(Clone, Debug)]
pub struct Options {
    pub durability: Durability,
    pub create: bool,
    pub segmented: bool,
    pub cache_bytes: u64,
    pub delta_bytes: u64,
    pub writer_bytes: u64,
    pub transaction_bytes: u64,
    pub all_transaction_bytes: u64,
    /// 共享读取的固定内存预算（`pinned_bytes`）。
    pub pinned_bytes: u64,
    pub max_snapshots: u64,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            durability: Durability::Sync,
            create: true,
            segmented: false,
            cache_bytes: 64 << 20,
            delta_bytes: 4 << 20,
            writer_bytes: 32 << 20,
            transaction_bytes: 64 << 20,
            all_transaction_bytes: 256 << 20,
            pinned_bytes: 128 << 20,
            max_snapshots: 1024,
        }
    }
}

impl Options {
    fn to_raw(&self) -> sys::fqdb_store_options {
        let mut flags = 0;
        if self.create {
            flags |= sys::FQDB_STORE_CREATE;
        }
        if self.segmented {
            flags |= sys::FQDB_STORE_SEGMENTED;
        }
        sys::fqdb_store_options {
            struct_size: std::mem::size_of::<sys::fqdb_store_options>() as u32,
            abi_version: 1,
            durability: self.durability as u32,
            flags,
            cache_bytes: self.cache_bytes,
            delta_bytes: self.delta_bytes,
            writer_bytes: self.writer_bytes,
            transaction_bytes: self.transaction_bytes,
            all_transaction_bytes: self.all_transaction_bytes,
            pinned_bytes: self.pinned_bytes,
            max_snapshots: self.max_snapshots,
        }
    }
}

/// `fqdb_store_get_info` 的常用字段。
#[derive(Clone, Copy, Debug, Default)]
pub struct StoreInfo {
    pub version: u64,
    pub durable_version: u64,
    pub file_bytes: u64,
    pub stored_keys: u64,
    pub stored_value_bytes: u64,
    pub cache_bytes: u64,
    pub pinned_bytes: u64,
}

/// 复合索引的字段类型（protobuf 标量类型号）。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum FieldType {
    Uint64 = 1,
    Int64 = 2,
    Double = 3,
    Bytes = 4,
    Sint64 = 5,
    Float = 6,
    Fixed64 = 7,
    Sfixed64 = 8,
    Fixed32 = 9,
    Sfixed32 = 10,
    Bool = 11,
}

/// 复合索引的一个字段。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IndexField {
    pub field: u32,
    pub ty: FieldType,
}

/// 索引标志：唯一。
pub const INDEX_UNIQUE: u32 = 1;
/// 索引标志：必需字段。
pub const INDEX_REQUIRED: u32 = 2;

/// 原生 Store 句柄的共享所有者。最后一个引用释放时调用 `fqdb_store_close`。
struct Core {
    ptr: NonNull<sys::fqdb_store>,
    closed: AtomicBool,
}

// SAFETY: 头文件约定 fqdb_store 的读写入口线程安全（多线程并发读是受支持的用法）；
// close 只在最后一个 Arc 引用消失时执行，不会与其它调用竞争。
unsafe impl Send for Core {}
// SAFETY: 同上。
unsafe impl Sync for Core {}

impl Drop for Core {
    fn drop(&mut self) {
        // SAFETY: ptr 来自成功的 fqdb_store_open，且此处是唯一的 close；
        // 所有借用它的子句柄都持有 Arc<Core>，因此不会再有并发调用。
        unsafe { sys::fqdb_store_close(self.ptr.as_ptr()) }
    }
}

/// 进程内分页数据库。可克隆（共享同一原生句柄），可跨线程使用。
///
/// `close()` 之后经由该 Store（及其克隆）的操作返回 [`Error::Closed`]；
/// 已创建的 [`PreparedKey`]、[`Reader`]、[`SharedValue`] 按原生库约定继续有效，
/// 原生句柄在所有克隆与子句柄释放后才真正关闭。
#[derive(Clone)]
pub struct Store {
    core: Arc<Core>,
}

impl std::fmt::Debug for Store {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Store").field("closed", &self.is_closed()).finish()
    }
}

fn c_path(path: &Path) -> Result<CString> {
    let text = path.to_str().ok_or(Error::Invalid("path is not valid UTF-8"))?;
    CString::new(text).map_err(|_| Error::Invalid("path contains NUL"))
}

/// 缓冲结果转换：NOT_FOUND 映射为 `None`。
fn buffer_result(code: i32, op: &'static str, raw: sys::fqdb_buffer) -> Result<Option<Buffer>> {
    if code == sys::FQDB_NOT_FOUND {
        return Ok(None);
    }
    check(code, op)?;
    // SAFETY: 调用成功，raw 是新分配、尚未释放的缓冲。
    Ok(Some(unsafe { Buffer::from_raw(raw) }))
}

fn atomic_version(info: &sys::fqdb_store_atomic_info) -> u64 {
    info.version
}

impl Store {
    /// 打开（按选项可创建）目录型数据库。
    pub fn open(path: impl AsRef<Path>, options: &Options) -> Result<Store> {
        let path = c_path(path.as_ref())?;
        let raw_options = options.to_raw();
        let mut out: *mut sys::fqdb_store = ptr::null_mut();
        // SAFETY: path 是以 NUL 结尾的有效 C 字符串；raw_options 与 out 是有效的局部变量。
        let code = unsafe { sys::fqdb_store_open(path.as_ptr(), &raw_options, &mut out) };
        check(code, "fqdb_store_open")?;
        let ptr = NonNull::new(out).ok_or(Error::Native { code: sys::FQDB_INTERNAL_ERROR, op: "fqdb_store_open" })?;
        Ok(Store { core: Arc::new(Core { ptr, closed: AtomicBool::new(false) }) })
    }

    /// 逻辑关闭：此后经由该 Store 的调用返回 [`Error::Closed`]。幂等。
    pub fn close(&self) {
        self.core.closed.store(true, Ordering::Release);
    }

    /// 是否已 `close()`。
    pub fn is_closed(&self) -> bool {
        self.core.closed.load(Ordering::Acquire)
    }

    /// 取得原生指针；已关闭则报错。
    #[inline]
    pub(crate) fn raw(&self) -> Result<*mut sys::fqdb_store> {
        if self.core.closed.load(Ordering::Relaxed) {
            return Err(Error::Closed);
        }
        Ok(self.core.ptr.as_ptr())
    }

    /// 同步到磁盘。
    pub fn sync(&self) -> Result<()> {
        // SAFETY: raw() 返回的句柄在 self 存活期间有效。
        check(unsafe { sys::fqdb_store_sync(self.raw()?) }, "fqdb_store_sync")
    }

    /// 统计信息。
    pub fn info(&self) -> Result<StoreInfo> {
        let mut raw = sys::fqdb_store_info::default();
        // SAFETY: 句柄有效，raw 是有效输出。
        check(unsafe { sys::fqdb_store_get_info(self.raw()?, &mut raw) }, "fqdb_store_get_info")?;
        Ok(StoreInfo {
            version: raw.version,
            durable_version: raw.durable_version,
            file_bytes: raw.file_bytes,
            stored_keys: raw.stored_keys,
            stored_value_bytes: raw.stored_value_bytes,
            cache_bytes: raw.cache_bytes,
            pinned_bytes: raw.pinned_bytes,
        })
    }

    /// 写入，返回提交版本。
    pub fn put(&self, key: &[u8], value: &[u8]) -> Result<u64> {
        self.put_with_expiry(key, value, 0)
    }

    /// 写入并设置绝对到期时间（Unix 毫秒，0 表示不过期）。
    pub fn put_with_expiry(&self, key: &[u8], value: &[u8], expires_at_ms: u64) -> Result<u64> {
        let mut info = sys::fqdb_store_atomic_info::default();
        // SAFETY: 句柄有效；key/value 切片在调用期间有效，库在返回前复制输入。
        let code = unsafe {
            sys::fqdb_store_put(
                self.raw()?,
                sys::fqdb_slice::from_bytes(key),
                sys::fqdb_slice::from_bytes(value),
                expires_at_ms,
                &mut info,
                0,
            )
        };
        check(code, "fqdb_store_put")?;
        Ok(atomic_version(&info))
    }

    /// 删除键；键不存在时同样成功。
    pub fn delete(&self, key: &[u8]) -> Result<()> {
        let mut info = sys::fqdb_store_atomic_info::default();
        // SAFETY: 句柄有效；key 在调用期间有效。
        let code = unsafe { sys::fqdb_store_delete(self.raw()?, sys::fqdb_slice::from_bytes(key), &mut info, 0) };
        check(code, "fqdb_store_delete")
    }

    /// 复制读取到原生缓冲（不经过 Rust 堆）。未命中返回 `None`。
    pub fn get_buffer(&self, key: &[u8]) -> Result<Option<Buffer>> {
        let mut raw = sys::fqdb_buffer::EMPTY;
        // SAFETY: 句柄有效；raw 是有效输出，成功后由 Buffer 接管。
        let code = unsafe { sys::fqdb_store_get(self.raw()?, sys::fqdb_slice::from_bytes(key), &mut raw) };
        buffer_result(code, "fqdb_store_get", raw)
    }

    /// 复制读取，调用方拥有 `Vec<u8>`。未命中返回 `None`，空值返回空 `Vec`。
    pub fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        Ok(self.get_buffer(key)?.map(|b| b.to_vec()))
    }

    /// 共享（零拷贝）读取。未命中返回 `None`。
    pub fn get_shared(&self, key: &[u8]) -> Result<Option<SharedValue>> {
        let mut out = sys::fqdb_store_shared_value::zeroed();
        // SAFETY: 句柄有效；out 是有效输出，成功后由 SharedValue 接管 owner。
        let code = unsafe { sys::fqdb_store_get_shared(self.raw()?, sys::fqdb_slice::from_bytes(key), &mut out) };
        SharedValue::from_raw(code, out, "fqdb_store_get_shared")
    }

    /// 准备键：键只封送一次，之后重复共享读取。
    pub fn prepare_key(&self, key: &[u8]) -> Result<PreparedKey> {
        PreparedKey::new(self, key)
    }

    /// 批量读取器：键只封送一次（最多 4096 个，保留顺序与重复键）。
    pub fn reader(&self, keys: &[&[u8]]) -> Result<Reader> {
        Reader::new(self, keys)
    }

    /// 开始事务；`timeout_ms` 为 0 表示无期限。
    pub fn begin(&self, timeout_ms: u64) -> Result<Transaction<'_>> {
        let mut out: *mut sys::fqdb_store_txn = ptr::null_mut();
        // SAFETY: 句柄有效；out 是有效输出。
        check(unsafe { sys::fqdb_store_txn_begin(self.raw()?, timeout_ms, &mut out) }, "fqdb_store_txn_begin")?;
        let raw = NonNull::new(out).ok_or(Error::Native { code: sys::FQDB_INTERNAL_ERROR, op: "fqdb_store_txn_begin" })?;
        Ok(Transaction { raw, _store: self })
    }

    /// 创建不可变快照。
    pub fn snapshot(&self) -> Result<Snapshot<'_>> {
        let mut out: *mut sys::fqdb_store_snapshot = ptr::null_mut();
        // SAFETY: 句柄有效；out 是有效输出。
        check(unsafe { sys::fqdb_store_snapshot_create(self.raw()?, &mut out) }, "fqdb_store_snapshot_create")?;
        let raw = NonNull::new(out).ok_or(Error::Native { code: sys::FQDB_INTERNAL_ERROR, op: "fqdb_store_snapshot_create" })?;
        Ok(Snapshot { raw, _store: self })
    }

    /// 定义复合索引（1..8 个字段）。`flags` 取 [`INDEX_UNIQUE`] / [`INDEX_REQUIRED`]。
    pub fn define_compound_index(&self, name: &str, key_prefix: &[u8], fields: &[IndexField], flags: u32) -> Result<()> {
        if fields.is_empty() || fields.len() > 8 {
            return Err(Error::Invalid("index needs 1..8 fields"));
        }
        let raw_fields: Vec<sys::fqdb_store_index_field> =
            fields.iter().map(|f| sys::fqdb_store_index_field { field: f.field, type_: f.ty as u32 }).collect();
        let mut info = sys::fqdb_store_maintenance_info::default();
        // SAFETY: 句柄有效；各指针在调用期间有效；options 传 NULL 表示默认值。
        let code = unsafe {
            sys::fqdb_store_index_define_compound(
                self.raw()?,
                sys::fqdb_slice::from_bytes(name.as_bytes()),
                sys::fqdb_slice::from_bytes(key_prefix),
                raw_fields.as_ptr(),
                raw_fields.len() as u32,
                flags,
                ptr::null(),
                &mut info,
            )
        };
        check(code, "fqdb_store_index_define_compound")
    }

    /// 删除索引定义。
    pub fn drop_index(&self, name: &str) -> Result<()> {
        let mut info = sys::fqdb_store_maintenance_info::default();
        // SAFETY: 句柄有效；name 在调用期间有效；options 传 NULL 表示默认值。
        let code = unsafe {
            sys::fqdb_store_index_drop(self.raw()?, sys::fqdb_slice::from_bytes(name.as_bytes()), ptr::null(), &mut info)
        };
        check(code, "fqdb_store_index_drop")
    }

    /// 在只读快照上执行已编码的 SELECT/EXPLAIN 请求，返回原生响应缓冲。
    pub(crate) fn select_raw(&self, request: &[u8]) -> Result<Buffer> {
        let mut raw = sys::fqdb_buffer::EMPTY;
        // SAFETY: 句柄有效；request 在调用期间有效；成功后由 Buffer 接管 raw。
        let code = unsafe { sys::fqdb_store_select(self.raw()?, sys::fqdb_slice::from_bytes(request), &mut raw) };
        check(code, "fqdb_store_select")?;
        // SAFETY: 调用成功，raw 是新分配的缓冲。
        Ok(unsafe { Buffer::from_raw(raw) })
    }
}

/// 事务。单线程使用（可移动到其它线程，但不可共享）。
///
/// `Drop` 时关闭句柄；未提交的写入随之丢弃。生命周期绑定到所属 Store。
pub struct Transaction<'s> {
    raw: NonNull<sys::fqdb_store_txn>,
    _store: &'s Store,
}

// SAFETY: 事务句柄没有线程亲和性，头文件只要求同一时刻单线程使用；
// 这里不实现 Sync，因此不会被并发访问。
unsafe impl Send for Transaction<'_> {}

impl Transaction<'_> {
    /// 读取（可见本事务未提交的写入）。
    pub fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        let mut raw = sys::fqdb_buffer::EMPTY;
        // SAFETY: 事务句柄在 self 存活期间有效；key 在调用期间有效。
        let code = unsafe { sys::fqdb_store_txn_get(self.raw.as_ptr(), sys::fqdb_slice::from_bytes(key), &mut raw) };
        Ok(buffer_result(code, "fqdb_store_txn_get", raw)?.map(|b| b.to_vec()))
    }

    /// 写入。
    pub fn put(&mut self, key: &[u8], value: &[u8]) -> Result<()> {
        self.put_with_expiry(key, value, 0)
    }

    /// 写入并设置绝对到期时间（Unix 毫秒，0 表示不过期）。
    pub fn put_with_expiry(&mut self, key: &[u8], value: &[u8], expires_at_ms: u64) -> Result<()> {
        // SAFETY: 句柄有效；输入在调用期间有效，库会复制。
        let code = unsafe {
            sys::fqdb_store_txn_put(
                self.raw.as_ptr(),
                sys::fqdb_slice::from_bytes(key),
                sys::fqdb_slice::from_bytes(value),
                expires_at_ms,
            )
        };
        check(code, "fqdb_store_txn_put")
    }

    /// 删除。
    pub fn delete(&mut self, key: &[u8]) -> Result<()> {
        // SAFETY: 句柄有效；key 在调用期间有效。
        check(unsafe { sys::fqdb_store_txn_delete(self.raw.as_ptr(), sys::fqdb_slice::from_bytes(key)) }, "fqdb_store_txn_delete")
    }

    /// 批量写入（不过期）；同一键按输入顺序，最后一个生效。
    pub fn put_many(&mut self, items: &[(&[u8], &[u8])]) -> Result<()> {
        let count = u32::try_from(items.len()).map_err(|_| Error::Invalid("too many items"))?;
        let raw_items: Vec<sys::fqdb_store_put_item> = items
            .iter()
            .map(|(k, v)| sys::fqdb_store_put_item {
                key: sys::fqdb_slice::from_bytes(k),
                value: sys::fqdb_slice::from_bytes(v),
                expires_at_ms: 0,
            })
            .collect();
        // SAFETY: 句柄有效；raw_items 及其引用的切片在调用期间有效。
        check(unsafe { sys::fqdb_store_txn_put_many(self.raw.as_ptr(), raw_items.as_ptr(), count) }, "fqdb_store_txn_put_many")
    }

    /// 提交并关闭事务，返回提交版本。
    pub fn commit(self) -> Result<u64> {
        let mut version = 0u64;
        // SAFETY: 句柄有效；提交后仍须 close，由 Drop 完成。
        let code = unsafe { sys::fqdb_store_txn_commit(self.raw.as_ptr(), &mut version, 0) };
        check(code, "fqdb_store_txn_commit")?;
        Ok(version)
    }

    /// 在事务内执行已编码的 SELECT/EXPLAIN 请求（可见私有写入）。
    pub(crate) fn execute_raw(&self, request: &[u8]) -> Result<Buffer> {
        let mut raw = sys::fqdb_buffer::EMPTY;
        // SAFETY: 句柄有效；request 在调用期间有效；成功后由 Buffer 接管。
        let code = unsafe { sys::fqdb_store_txn_execute(self.raw.as_ptr(), sys::fqdb_slice::from_bytes(request), &mut raw) };
        check(code, "fqdb_store_txn_execute")?;
        // SAFETY: 调用成功，raw 是新分配的缓冲。
        Ok(unsafe { Buffer::from_raw(raw) })
    }
}

impl Drop for Transaction<'_> {
    fn drop(&mut self) {
        // SAFETY: 句柄由 begin 创建，只在此处关闭一次（commit 之后同样需要 close）。
        unsafe { sys::fqdb_store_txn_close(self.raw.as_ptr()) }
    }
}

/// 不可变快照；生命周期绑定到所属 Store，读取到的共享值不依赖快照存活。
pub struct Snapshot<'s> {
    raw: NonNull<sys::fqdb_store_snapshot>,
    _store: &'s Store,
}

// SAFETY: 快照句柄没有线程亲和性；只实现 Send，不声明 Sync。
unsafe impl Send for Snapshot<'_> {}

impl Snapshot<'_> {
    /// 复制读取。
    pub fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        let mut raw = sys::fqdb_buffer::EMPTY;
        // SAFETY: 句柄有效；key 在调用期间有效。
        let code = unsafe { sys::fqdb_store_snapshot_get(self.raw.as_ptr(), sys::fqdb_slice::from_bytes(key), &mut raw) };
        Ok(buffer_result(code, "fqdb_store_snapshot_get", raw)?.map(|b| b.to_vec()))
    }

    /// 共享读取。
    pub fn get_shared(&self, key: &[u8]) -> Result<Option<SharedValue>> {
        let mut out = sys::fqdb_store_shared_value::zeroed();
        // SAFETY: 句柄有效；key 在调用期间有效；成功后由 SharedValue 接管 owner。
        let code = unsafe { sys::fqdb_store_snapshot_get_shared(self.raw.as_ptr(), sys::fqdb_slice::from_bytes(key), &mut out) };
        SharedValue::from_raw(code, out, "fqdb_store_snapshot_get_shared")
    }
}

impl Drop for Snapshot<'_> {
    fn drop(&mut self) {
        // SAFETY: 句柄由 snapshot_create 创建，只关闭一次。
        unsafe { sys::fqdb_store_snapshot_close(self.raw.as_ptr()) }
    }
}
