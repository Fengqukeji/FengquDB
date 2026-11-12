//! 共享（零拷贝）读取：SharedValue、PreparedKey、Reader、Rows。
//!
//! 所有借用原生内存的视图都用生命周期绑定到所有者：
//! `SharedValue::as_bytes(&self)` 借用值；`Reader::read(&mut self)` 返回的 `Rows<'_>`
//! 独占借用读取器，因此下一次 `read/release` 之前借用检查器保证行已失效。

use std::ptr::{self, NonNull};

use fengqudb_sys as sys;

use crate::error::{check, Error, Result};
use crate::store::Store;

/// 拥有原生租约的只读值视图（`fqdb_store_value*`）。
///
/// 字节不复制；`Drop` 时 `fqdb_store_value_close` 归还固定内存预算。
/// 按头文件约定，视图与元数据在后续写入、压缩甚至 Store 关闭之后仍然有效。
pub struct SharedValue {
    owner: NonNull<sys::fqdb_store_value>,
    data: *const u8,
    len: usize,
    version: u64,
    expires_at_ms: u64,
}

// SAFETY: 值是不可变字节，租约只通过 &self 读取；关闭租约可在任意线程执行。
unsafe impl Send for SharedValue {}
// SAFETY: 只有只读访问，没有内部可变性。
unsafe impl Sync for SharedValue {}

impl SharedValue {
    /// 把 `get_shared` 系列调用的结果转换为 `Option<SharedValue>`；NOT_FOUND 为 `None`。
    #[inline]
    pub(crate) fn from_raw(code: i32, out: sys::fqdb_store_shared_value, op: &'static str) -> Result<Option<SharedValue>> {
        if code != sys::FQDB_OK {
            if code == sys::FQDB_NOT_FOUND {
                return Ok(None);
            }
            return Err(Error::Native { code, op });
        }
        let Some(owner) = NonNull::new(out.owner) else {
            return Ok(None);
        };
        Ok(Some(SharedValue {
            owner,
            data: out.row.value.data.cast::<u8>(),
            len: out.row.value.size as usize,
            version: out.row.version,
            expires_at_ms: out.row.expires_at_ms,
        }))
    }

    /// 值字节（零拷贝），生命周期绑定到本租约。空值返回空切片。
    #[inline]
    pub fn as_bytes(&self) -> &[u8] {
        if self.len == 0 || self.data.is_null() {
            return &[];
        }
        // SAFETY: 租约未关闭期间 data 指向 len 个不可变字节（头文件：value_data 有效到
        // value_close）；返回引用的生命周期受 &self 约束，Drop 之前不会释放。
        unsafe { std::slice::from_raw_parts(self.data, self.len) }
    }

    /// 字节数。
    pub fn len(&self) -> usize {
        self.len
    }

    /// 是否为空值。
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// 该值的提交版本。
    pub fn version(&self) -> u64 {
        self.version
    }

    /// 到期时间（Unix 毫秒，0 表示不过期）。
    pub fn expires_at_ms(&self) -> u64 {
        self.expires_at_ms
    }
}

impl AsRef<[u8]> for SharedValue {
    fn as_ref(&self) -> &[u8] {
        self.as_bytes()
    }
}

impl std::fmt::Debug for SharedValue {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("SharedValue").field("len", &self.len).field("version", &self.version).finish()
    }
}

impl Drop for SharedValue {
    fn drop(&mut self) {
        // SAFETY: owner 来自成功的 get_shared 调用，只在此处关闭一次。
        unsafe { sys::fqdb_store_value_close(self.owner.as_ptr()) }
    }
}

/// 准备键：键复制一次，之后每次读取不再封送、不再分配。
///
/// 持有底层 Store 的引用，Store 逻辑关闭或被丢弃后仍可读取。
/// 支持多线程并发读取。
pub struct PreparedKey {
    raw: NonNull<sys::fqdb_store_key>,
    _store: Store,
}

// SAFETY: 头文件：“Concurrent reads are supported”，关闭不与调用竞争（Drop 需要 &mut self）。
unsafe impl Send for PreparedKey {}
// SAFETY: 同上，并发读取受支持。
unsafe impl Sync for PreparedKey {}

impl PreparedKey {
    pub(crate) fn new(store: &Store, key: &[u8]) -> Result<PreparedKey> {
        let mut out: *mut sys::fqdb_store_key = ptr::null_mut();
        // SAFETY: 句柄有效；key 在调用期间有效，库会复制；out 是有效输出。
        let code = unsafe { sys::fqdb_store_key_prepare(store.raw()?, sys::fqdb_slice::from_bytes(key), &mut out) };
        check(code, "fqdb_store_key_prepare")?;
        let raw = NonNull::new(out).ok_or(Error::Native { code: sys::FQDB_INTERNAL_ERROR, op: "fqdb_store_key_prepare" })?;
        Ok(PreparedKey { raw, _store: store.clone() })
    }

    /// 共享读取；热路径不分配。未命中返回 `None`。
    #[inline]
    pub fn get_shared(&self) -> Result<Option<SharedValue>> {
        let mut out = sys::fqdb_store_shared_value::zeroed();
        // SAFETY: 准备键句柄在 self 存活期间有效，并发读取受支持；out 是有效输出。
        let code = unsafe { sys::fqdb_store_key_get_shared(self.raw.as_ptr(), &mut out) };
        SharedValue::from_raw(code, out, "fqdb_store_key_get_shared")
    }
}

impl Drop for PreparedKey {
    fn drop(&mut self) {
        // SAFETY: 句柄由 key_prepare 创建，只关闭一次；_store 字段在此之后才释放。
        unsafe { sys::fqdb_store_key_close(self.raw.as_ptr()) }
    }
}

/// 批量读取器：键只准备一次，每次 `read` 看到同一个发布视图。
///
/// 由调用方串行使用（`Send` 但不是 `Sync`），多个读取器可并发。
/// 持有底层 Store 的引用，Store 关闭后仍可读取。
pub struct Reader {
    raw: NonNull<sys::fqdb_store_reader>,
    count: usize,
    _store: Store,
}

// SAFETY: 读取器没有线程亲和性，头文件只要求调用串行；所有方法需要 &mut self，
// 且没有实现 Sync，所以不会出现并发访问。
unsafe impl Send for Reader {}

impl Reader {
    pub(crate) fn new(store: &Store, keys: &[&[u8]]) -> Result<Reader> {
        let count = u32::try_from(keys.len()).map_err(|_| Error::Invalid("too many keys"))?;
        let slices: Vec<sys::fqdb_slice> = keys.iter().map(|k| sys::fqdb_slice::from_bytes(k)).collect();
        let mut out: *mut sys::fqdb_store_reader = ptr::null_mut();
        // SAFETY: 句柄有效；slices 及其引用的键在调用期间有效，库会复制键；out 是有效输出。
        let code = unsafe { sys::fqdb_store_reader_create(store.raw()?, slices.as_ptr(), count, &mut out) };
        check(code, "fqdb_store_reader_create")?;
        let raw = NonNull::new(out).ok_or(Error::Native { code: sys::FQDB_INTERNAL_ERROR, op: "fqdb_store_reader_create" })?;
        Ok(Reader { raw, count: keys.len(), _store: store.clone() })
    }

    /// 键数。
    pub fn key_count(&self) -> usize {
        self.count
    }

    /// 读取全部键。返回的行借用本读取器，直到下一次 `read/release` 之前有效。
    ///
    /// 借用检查器拒绝在下一次 `read` 之后继续使用旧的行：
    ///
    /// ```compile_fail
    /// fn demo(reader: &mut fengqudb::Reader) -> fengqudb::Result<()> {
    ///     let rows = reader.read()?;
    ///     let _next = reader.read()?; // 第二次可变借用，旧 rows 仍在使用
    ///     let _ = rows.len();
    ///     Ok(())
    /// }
    /// ```
    ///
    /// 读取器不是 `Sync`，不能被多个线程共享：
    ///
    /// ```compile_fail
    /// fn needs_sync<T: Sync>() {}
    /// needs_sync::<fengqudb::Reader>();
    /// ```
    ///
    /// 共享值的视图不能比它的所有者活得更久：
    ///
    /// ```compile_fail
    /// fn demo(store: &fengqudb::Store) -> &[u8] {
    ///     let value = store.get_shared(b"k").unwrap().unwrap();
    ///     value.as_bytes() // 返回了局部变量 value 的借用
    /// }
    /// ```
    #[inline]
    pub fn read(&mut self) -> Result<Rows<'_>> {
        let mut rows: *const sys::fqdb_store_result_row = ptr::null();
        let mut count = 0u32;
        // SAFETY: 句柄有效且由 &mut self 串行化；rows/count 是有效输出。
        let code = unsafe { sys::fqdb_store_reader_read(self.raw.as_ptr(), &mut rows, &mut count) };
        check(code, "fqdb_store_reader_read")?;
        let rows = if count == 0 || rows.is_null() {
            &[][..]
        } else {
            // SAFETY: 成功时 rows 指向 count 个连续行描述，有效到本读取器的下一次
            // read/release/close；返回值通过 &mut self 的生命周期与之绑定。
            unsafe { std::slice::from_raw_parts(rows, count as usize) }
        };
        Ok(Rows { rows })
    }

    /// 释放当前结果，保留准备好的键。
    pub fn release(&mut self) {
        // SAFETY: 句柄有效且由 &mut self 串行化；此前的借用已因 &mut 而结束。
        unsafe { sys::fqdb_store_reader_release(self.raw.as_ptr()) }
    }
}

impl Drop for Reader {
    fn drop(&mut self) {
        // SAFETY: 句柄由 reader_create 创建，只关闭一次；_store 字段在此之后才释放。
        unsafe { sys::fqdb_store_reader_close(self.raw.as_ptr()) }
    }
}

/// 一次 `Reader::read` 的结果，借用读取器。
#[derive(Clone, Copy)]
pub struct Rows<'a> {
    rows: &'a [sys::fqdb_store_result_row],
}

impl<'a> Rows<'a> {
    /// 行数（等于键数，保持输入顺序与重复键）。
    pub fn len(&self) -> usize {
        self.rows.len()
    }

    /// 是否没有行。
    pub fn is_empty(&self) -> bool {
        self.rows.is_empty()
    }

    /// 第 `index` 行。
    pub fn get(&self, index: usize) -> Option<Row<'a>> {
        self.rows.get(index).map(|raw| Row { raw })
    }

    /// 按顺序迭代各行。
    pub fn iter(&self) -> impl Iterator<Item = Row<'a>> + 'a {
        self.rows.iter().map(|raw| Row { raw })
    }
}

/// 读取器结果中的一行。
#[derive(Clone, Copy)]
pub struct Row<'a> {
    raw: &'a sys::fqdb_store_result_row,
}

impl<'a> Row<'a> {
    /// 是否命中（空值也算命中）。
    #[inline]
    pub fn found(&self) -> bool {
        self.raw.found != 0
    }

    /// 值字节；未命中或空值为空切片。
    #[inline]
    pub fn value(&self) -> &'a [u8] {
        let slice = self.raw.value;
        if !self.found() || slice.size == 0 || slice.data.is_null() {
            return &[];
        }
        // SAFETY: 行描述及其字节有效到读取器下一次 read/release，生命周期 'a 来自
        // Reader::read 的 &mut 借用。
        unsafe { std::slice::from_raw_parts(slice.data.cast::<u8>(), slice.size as usize) }
    }

    /// 值的提交版本。
    pub fn version(&self) -> u64 {
        self.raw.version
    }

    /// 到期时间（Unix 毫秒，0 表示不过期）。
    pub fn expires_at_ms(&self) -> u64 {
        self.raw.expires_at_ms
    }
}
