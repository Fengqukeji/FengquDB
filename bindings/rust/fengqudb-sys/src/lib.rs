//! 手写的 libfengqudb C ABI 声明（对应 `include/fengqudb.h` 与 `include/fengqudb_store.h`）。
//!
//! 只声明绑定用到的入口；结构体布局与头文件逐字段一致（`#[repr(C)]`）。
//! 所有函数都是 `unsafe extern "C"`，安全封装见 `fengqudb` crate。
#![allow(non_camel_case_types)]

use std::os::raw::{c_char, c_void};

pub type fqdb_status = i32;

pub const FQDB_OK: fqdb_status = 0;
pub const FQDB_NOT_FOUND: fqdb_status = 1;
pub const FQDB_END: fqdb_status = 2;
pub const FQDB_INVALID_ARGUMENT: fqdb_status = 3;
pub const FQDB_IO_ERROR: fqdb_status = 4;
pub const FQDB_CORRUPT: fqdb_status = 5;
pub const FQDB_LOCKED: fqdb_status = 6;
pub const FQDB_CONFLICT: fqdb_status = 7;
pub const FQDB_NO_MEMORY: fqdb_status = 8;
pub const FQDB_LIMIT: fqdb_status = 9;
pub const FQDB_COMMIT_UNKNOWN: fqdb_status = 10;
pub const FQDB_RECOVERY_REQUIRED: fqdb_status = 11;
pub const FQDB_UNSUPPORTED: fqdb_status = 12;
pub const FQDB_INTERNAL_ERROR: fqdb_status = 13;
pub const FQDB_HISTORY_LOST: fqdb_status = 14;
pub const FQDB_ALREADY_EXISTS: fqdb_status = 15;
pub const FQDB_BUFFER_TOO_SMALL: fqdb_status = 16;
pub const FQDB_TIMEOUT: fqdb_status = 17;
pub const FQDB_NOT_MODIFIED: fqdb_status = 18;
pub const FQDB_CANCELLED: fqdb_status = 19;
pub const FQDB_CONSTRAINT: fqdb_status = 20;
pub const FQDB_ASYNC_REQUIRED: fqdb_status = 21;

pub const FQDB_DURABILITY_SYNC: u32 = 0;
pub const FQDB_DURABILITY_BUFFERED: u32 = 1;
pub const FQDB_STORE_CREATE: u32 = 1;
pub const FQDB_STORE_SEGMENTED: u32 = 2;

pub const FQDB_SELECT: u64 = 53;
pub const FQDB_SELECT_EXPLAIN: u64 = 54;

/// 借用的只读字节切片（`fqdb_slice`）。
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct fqdb_slice {
    pub data: *const c_void,
    pub size: u64,
}

impl fqdb_slice {
    /// 空切片。
    pub const EMPTY: fqdb_slice = fqdb_slice { data: std::ptr::null(), size: 0 };

    /// 由 Rust 字节切片构造；返回值不拥有数据，调用方须保证调用期间切片有效。
    #[inline]
    pub fn from_bytes(bytes: &[u8]) -> fqdb_slice {
        fqdb_slice { data: bytes.as_ptr().cast(), size: bytes.len() as u64 }
    }
}

/// 数据库分配的缓冲（`fqdb_buffer`），必须用 `fqdb_buffer_free` 恰好释放一次。
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct fqdb_buffer {
    pub data: *mut c_void,
    pub size: u64,
}

impl fqdb_buffer {
    pub const EMPTY: fqdb_buffer = fqdb_buffer { data: std::ptr::null_mut(), size: 0 };
}

macro_rules! opaque {
    ($($name:ident),* $(,)?) => {$(
        #[repr(C)]
        pub struct $name { _private: [u8; 0] }
    )*};
}
opaque!(
    fqdb_store,
    fqdb_store_txn,
    fqdb_store_snapshot,
    fqdb_store_value,
    fqdb_store_key,
    fqdb_store_reader,
    fqdb_store_maintenance_options,
);

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct fqdb_store_options {
    pub struct_size: u32,
    pub abi_version: u32,
    pub durability: u32,
    pub flags: u32,
    pub cache_bytes: u64,
    pub delta_bytes: u64,
    pub writer_bytes: u64,
    pub transaction_bytes: u64,
    pub all_transaction_bytes: u64,
    pub pinned_bytes: u64,
    pub max_snapshots: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct fqdb_store_info {
    pub version: u64,
    pub durable_version: u64,
    pub file_bytes: u64,
    pub indexed_keys: u64,
    pub stored_keys: u64,
    pub stored_value_bytes: u64,
    pub delta_bytes: u64,
    pub cache_bytes: u64,
    pub pinned_bytes: u64,
    pub transaction_bytes: u64,
    pub recovered_transactions: u64,
    pub repaired_bytes: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct fqdb_store_atomic_info {
    pub version: u64,
    pub observed_version: u64,
    pub value: i64,
    pub matched: u32,
    pub changed: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct fqdb_store_maintenance_info {
    pub snapshot_version: u64,
    pub final_version: u64,
    pub source_file_bytes: u64,
    pub output_file_bytes: u64,
    pub stored_keys: u64,
    pub stored_value_bytes: u64,
    pub copied_keys: u64,
    pub catchup_rounds: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct fqdb_store_index_field {
    pub field: u32,
    pub type_: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct fqdb_store_put_item {
    pub key: fqdb_slice,
    pub value: fqdb_slice,
    pub expires_at_ms: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct fqdb_store_result_row {
    pub value: fqdb_slice,
    pub version: u64,
    pub expires_at_ms: u64,
    pub found: u32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct fqdb_store_shared_value {
    pub owner: *mut fqdb_store_value,
    pub row: fqdb_store_result_row,
}

impl fqdb_store_shared_value {
    /// 全零初始值，用作输出参数。
    pub const fn zeroed() -> Self {
        fqdb_store_shared_value {
            owner: std::ptr::null_mut(),
            row: fqdb_store_result_row {
                value: fqdb_slice::EMPTY,
                version: 0,
                expires_at_ms: 0,
                found: 0,
                reserved: 0,
            },
        }
    }
}

extern "C" {
    pub fn fqdb_abi_version() -> u32;
    pub fn fqdb_status_message(status: fqdb_status) -> *const c_char;
    pub fn fqdb_buffer_free(buffer: *mut fqdb_buffer);

    pub fn fqdb_store_open(
        directory: *const c_char,
        options: *const fqdb_store_options,
        out: *mut *mut fqdb_store,
    ) -> fqdb_status;
    pub fn fqdb_store_close(store: *mut fqdb_store);
    pub fn fqdb_store_get_info(store: *mut fqdb_store, out: *mut fqdb_store_info) -> fqdb_status;
    pub fn fqdb_store_sync(store: *mut fqdb_store) -> fqdb_status;

    pub fn fqdb_store_get(store: *mut fqdb_store, key: fqdb_slice, out: *mut fqdb_buffer) -> fqdb_status;
    pub fn fqdb_store_put(
        store: *mut fqdb_store,
        key: fqdb_slice,
        value: fqdb_slice,
        expires_at_ms: u64,
        out: *mut fqdb_store_atomic_info,
        force_reliable: u32,
    ) -> fqdb_status;
    pub fn fqdb_store_delete(
        store: *mut fqdb_store,
        key: fqdb_slice,
        out: *mut fqdb_store_atomic_info,
        force_reliable: u32,
    ) -> fqdb_status;

    pub fn fqdb_store_get_shared(
        store: *mut fqdb_store,
        key: fqdb_slice,
        out: *mut fqdb_store_shared_value,
    ) -> fqdb_status;
    pub fn fqdb_store_snapshot_get_shared(
        snapshot: *const fqdb_store_snapshot,
        key: fqdb_slice,
        out: *mut fqdb_store_shared_value,
    ) -> fqdb_status;
    pub fn fqdb_store_key_prepare(
        store: *mut fqdb_store,
        key: fqdb_slice,
        out: *mut *mut fqdb_store_key,
    ) -> fqdb_status;
    pub fn fqdb_store_key_close(key: *mut fqdb_store_key);
    pub fn fqdb_store_key_get_shared(
        key: *mut fqdb_store_key,
        out: *mut fqdb_store_shared_value,
    ) -> fqdb_status;
    pub fn fqdb_store_value_close(value: *mut fqdb_store_value);

    pub fn fqdb_store_reader_create(
        store: *mut fqdb_store,
        keys: *const fqdb_slice,
        count: u32,
        out: *mut *mut fqdb_store_reader,
    ) -> fqdb_status;
    pub fn fqdb_store_reader_read(
        reader: *mut fqdb_store_reader,
        rows: *mut *const fqdb_store_result_row,
        count: *mut u32,
    ) -> fqdb_status;
    pub fn fqdb_store_reader_release(reader: *mut fqdb_store_reader);
    pub fn fqdb_store_reader_close(reader: *mut fqdb_store_reader);

    pub fn fqdb_store_snapshot_create(
        store: *mut fqdb_store,
        out: *mut *mut fqdb_store_snapshot,
    ) -> fqdb_status;
    pub fn fqdb_store_snapshot_close(snapshot: *mut fqdb_store_snapshot);
    pub fn fqdb_store_snapshot_get(
        snapshot: *const fqdb_store_snapshot,
        key: fqdb_slice,
        out: *mut fqdb_buffer,
    ) -> fqdb_status;

    pub fn fqdb_store_txn_begin(
        store: *mut fqdb_store,
        timeout_ms: u64,
        out: *mut *mut fqdb_store_txn,
    ) -> fqdb_status;
    pub fn fqdb_store_txn_close(txn: *mut fqdb_store_txn);
    pub fn fqdb_store_txn_get(
        txn: *mut fqdb_store_txn,
        key: fqdb_slice,
        out: *mut fqdb_buffer,
    ) -> fqdb_status;
    pub fn fqdb_store_txn_put(
        txn: *mut fqdb_store_txn,
        key: fqdb_slice,
        value: fqdb_slice,
        expires_at_ms: u64,
    ) -> fqdb_status;
    pub fn fqdb_store_txn_delete(txn: *mut fqdb_store_txn, key: fqdb_slice) -> fqdb_status;
    pub fn fqdb_store_txn_put_many(
        txn: *mut fqdb_store_txn,
        items: *const fqdb_store_put_item,
        count: u32,
    ) -> fqdb_status;
    pub fn fqdb_store_txn_commit(
        txn: *mut fqdb_store_txn,
        version: *mut u64,
        force_reliable: u32,
    ) -> fqdb_status;
    pub fn fqdb_store_txn_execute(
        txn: *mut fqdb_store_txn,
        request: fqdb_slice,
        out: *mut fqdb_buffer,
    ) -> fqdb_status;

    pub fn fqdb_store_select(
        store: *mut fqdb_store,
        request: fqdb_slice,
        out: *mut fqdb_buffer,
    ) -> fqdb_status;

    pub fn fqdb_store_index_define_compound(
        store: *mut fqdb_store,
        name: fqdb_slice,
        key_prefix: fqdb_slice,
        fields: *const fqdb_store_index_field,
        count: u32,
        flags: u32,
        options: *const fqdb_store_maintenance_options,
        out: *mut fqdb_store_maintenance_info,
    ) -> fqdb_status;
    pub fn fqdb_store_index_drop(
        store: *mut fqdb_store,
        name: fqdb_slice,
        options: *const fqdb_store_maintenance_options,
        out: *mut fqdb_store_maintenance_info,
    ) -> fqdb_status;
}
