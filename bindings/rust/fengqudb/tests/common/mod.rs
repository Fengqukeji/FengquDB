//! 测试公共工具：临时目录与默认选项。
#![allow(dead_code)]

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};

use fengqudb::{Durability, Options, Store};

static COUNTER: AtomicU32 = AtomicU32::new(0);

/// 临时数据库目录，Drop 时删除。
pub struct TempDir(pub PathBuf);

impl TempDir {
    pub fn new(tag: &str) -> TempDir {
        let nanos = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!("fqdb-rust-{tag}-{}-{n}-{nanos}", std::process::id()));
        TempDir(dir)
    }

    pub fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// 测试用选项：缓冲写入以加快速度，预算与 bench_c 相近。
pub fn test_options() -> Options {
    Options {
        durability: Durability::Buffered,
        cache_bytes: 256 << 20,
        delta_bytes: 16 << 20,
        pinned_bytes: 256 << 20,
        ..Options::default()
    }
}

pub fn open(dir: &TempDir) -> Store {
    Store::open(dir.path(), &test_options()).expect("open store")
}

pub fn now_ms() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_millis() as u64
}
