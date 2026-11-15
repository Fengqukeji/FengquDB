//! FengquDB 的安全 Rust 封装（基于 `fengqudb-sys` 的 C ABI 声明）。
//!
//! * [`Store`]：打开/读写/事务/索引/查询；可克隆、可跨线程。
//! * 共享（零拷贝）读取：[`Store::get_shared`] → [`SharedValue`]；热点读取用
//!   [`PreparedKey`] 与批量 [`Reader`]（`read(&mut self) -> Rows<'_>`）。
//! * 查询：底层 [`query::Query`]（逐字节与 Python 绑定一致）与类型化 [`builder`]；
//!   响应 [`QueryResult`] 在原生缓冲上惰性解码。
//!
//! 所有 `unsafe` 集中在 `buffer.rs`、`store.rs`、`shared.rs`、`error.rs`，每处都有 SAFETY 注释。

mod buffer;
pub mod builder;
mod error;
pub mod query;
mod result;
mod shared;
mod store;
mod wire;

pub use buffer::Buffer;
pub use error::{status, status_message, Error, Result};
pub use result::{Access, AccessIter, QueryResult, ResultRow, ResultRows, ResultValues, Stats, Value};
pub use shared::{PreparedKey, Reader, Row, Rows, SharedValue};
pub use store::{
    Durability, FieldType, IndexField, Options, Snapshot, Store, StoreInfo, Transaction, WriteMode, WriteOptions,
    INDEX_REQUIRED, INDEX_UNIQUE,
};
