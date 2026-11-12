//! 热路径零堆分配回归测试：用计数分配器断言 Rust 侧没有每次调用的分配。
//!
//! 本文件只有一个 #[test]，保证测试期间没有其它线程在分配。

mod common;

use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

use common::{open, TempDir};
use fengqudb::query::{feature_request, SelectTarget};

struct Counting;
static ON: AtomicBool = AtomicBool::new(false);
static COUNT: AtomicUsize = AtomicUsize::new(0);

// SAFETY: 全部转发给 System，仅额外计数。
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if ON.load(Ordering::Relaxed) {
            COUNT.fetch_add(1, Ordering::Relaxed);
        }
        System.alloc(layout)
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        System.dealloc(ptr, layout)
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if ON.load(Ordering::Relaxed) {
            COUNT.fetch_add(1, Ordering::Relaxed);
        }
        System.realloc(ptr, layout, new_size)
    }
}

#[global_allocator]
static GLOBAL: Counting = Counting;

/// 运行 `f` 并返回期间的分配次数。
fn allocations(f: impl FnOnce()) -> usize {
    COUNT.store(0, Ordering::SeqCst);
    ON.store(true, Ordering::SeqCst);
    f();
    ON.store(false, Ordering::SeqCst);
    COUNT.load(Ordering::SeqCst)
}

#[test]
fn hot_paths_do_not_allocate() {
    let dir = TempDir::new("noalloc");
    let db = open(&dir);
    for i in 0..64u32 {
        db.put(&i.to_be_bytes(), &[i as u8; 256]).unwrap();
    }
    let key = 7u32.to_be_bytes();
    let prepared = db.prepare_key(&key).unwrap();
    let keys: Vec<[u8; 4]> = (0..32u32).map(|i| i.to_be_bytes()).collect();
    let refs: Vec<&[u8]> = keys.iter().map(|k| &k[..]).collect();
    let mut reader = db.reader(&refs).unwrap();
    // 一次最小查询请求（空前缀全扫描）；预先构造，热循环只执行。
    let request = fengqudb::query::PreparedQuery::new(fengqudb::query::Query::new(Vec::new()).encode().unwrap());
    let raw_request = feature_request(53, &[], &[], request.encoded());

    // 预热：首次调用可能初始化原生/线程局部状态。
    let mut sink = 0usize;
    sink += prepared.get_shared().unwrap().unwrap().len();
    sink += db.get_shared(&key).unwrap().unwrap().len();
    sink += reader.read().unwrap().len();
    sink += db.select_request(&raw_request).unwrap().as_bytes().len();

    let prepared_allocs = allocations(|| {
        for _ in 0..10_000 {
            sink += prepared.get_shared().unwrap().unwrap().as_bytes()[255] as usize;
        }
    });
    let keyed_allocs = allocations(|| {
        for _ in 0..10_000 {
            sink += db.get_shared(&key).unwrap().unwrap().as_bytes()[0] as usize;
        }
    });
    let reader_allocs = allocations(|| {
        for _ in 0..1_000 {
            let rows = reader.read().unwrap();
            for row in rows.iter() {
                sink += row.value()[0] as usize;
            }
        }
    });
    let miss_allocs = allocations(|| {
        for _ in 0..1_000 {
            assert!(db.get_shared(b"absent").unwrap().is_none());
        }
    });
    let select_allocs = allocations(|| {
        for _ in 0..200 {
            let result = db.select_request(&raw_request).unwrap();
            sink += result.rows().count() + result.cursor().unwrap().len();
        }
    });
    assert!(sink > 0);
    assert_eq!(prepared_allocs, 0, "prepared get_shared");
    assert_eq!(keyed_allocs, 0, "keyed get_shared");
    assert_eq!(reader_allocs, 0, "reader read");
    assert_eq!(miss_allocs, 0, "miss");
    assert_eq!(select_allocs, 0, "select + lazy row decoding");
}
