//! 错误类型：保留原生状态码，实现 `std::error::Error`。

use std::ffi::CStr;
use std::fmt;

/// 原生状态码常量（与 `fengqudb.h` 一致）。
pub mod status {
    pub use fengqudb_sys::{
        FQDB_ALREADY_EXISTS as ALREADY_EXISTS, FQDB_ASYNC_REQUIRED as ASYNC_REQUIRED,
        FQDB_BUFFER_TOO_SMALL as BUFFER_TOO_SMALL, FQDB_CANCELLED as CANCELLED,
        FQDB_COMMIT_UNKNOWN as COMMIT_UNKNOWN, FQDB_CONFLICT as CONFLICT,
        FQDB_CONSTRAINT as CONSTRAINT, FQDB_CORRUPT as CORRUPT, FQDB_END as END,
        FQDB_HISTORY_LOST as HISTORY_LOST, FQDB_INTERNAL_ERROR as INTERNAL_ERROR,
        FQDB_INVALID_ARGUMENT as INVALID_ARGUMENT, FQDB_IO_ERROR as IO_ERROR, FQDB_LIMIT as LIMIT,
        FQDB_LOCKED as LOCKED, FQDB_NOT_FOUND as NOT_FOUND, FQDB_NOT_MODIFIED as NOT_MODIFIED,
        FQDB_NO_MEMORY as NO_MEMORY, FQDB_OK as OK, FQDB_RECOVERY_REQUIRED as RECOVERY_REQUIRED,
        FQDB_TIMEOUT as TIMEOUT, FQDB_UNSUPPORTED as UNSUPPORTED,
    };
}

/// 绑定返回的错误。不在堆上分配，便于热路径返回。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// 原生库返回了非 OK 状态码；`op` 是出错的 C 入口名。
    Native { code: i32, op: &'static str },
    /// Store 已被 `close()`（逻辑关闭）；之后经由该 Store 的调用都返回此错误。
    Closed,
    /// 调用方参数不合法（在调用原生库之前检出）。
    Invalid(&'static str),
    /// 查询响应或值无法解码。
    Decode(&'static str),
}

pub type Result<T> = std::result::Result<T, Error>;

impl Error {
    /// 原生状态码；非原生错误返回 `None`。
    pub fn code(&self) -> Option<i32> {
        match self {
            Error::Native { code, .. } => Some(*code),
            _ => None,
        }
    }

    /// 是否为指定的原生状态码。
    pub fn is_status(&self, code: i32) -> bool {
        self.code() == Some(code)
    }
}

/// 原生状态码的文字说明。
pub fn status_message(code: i32) -> String {
    // SAFETY: fqdb_status_message 对任意状态码返回指向静态字符串的非空指针
    // （未知码返回通用文字）；这里仍对空指针做防御。
    unsafe {
        let ptr = fengqudb_sys::fqdb_status_message(code);
        if ptr.is_null() {
            return format!("status {code}");
        }
        CStr::from_ptr(ptr).to_string_lossy().into_owned()
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Native { code, op } => write!(f, "{op}: {} (status {code})", status_message(*code)),
            Error::Closed => f.write_str("store is closed"),
            Error::Invalid(what) => write!(f, "invalid argument: {what}"),
            Error::Decode(what) => write!(f, "decode error: {what}"),
        }
    }
}

impl std::error::Error for Error {}

/// 把原生状态码转换为 `Result<()>`。
#[inline]
pub(crate) fn check(code: i32, op: &'static str) -> Result<()> {
    if code == fengqudb_sys::FQDB_OK {
        Ok(())
    } else {
        Err(Error::Native { code, op })
    }
}
