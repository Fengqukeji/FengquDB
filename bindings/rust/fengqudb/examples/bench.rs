//! 与 tools/lang_conformance/bench_c.c 同数据、同操作的基准。
//!
//! 用法: bench DIR [SECONDS] [--allocs] [--extra]
//! 输出每行 JSON: {"op":..,"ops":..,"ns_per_op":..,"sink":..}
//! 数据: 32768 条 256 B 记录，键 'k'+8 字节大端序号；select_topn 需要环境变量
//! FQDB_BENCH_SELECT_HEX（64 条 p/ 文档与 bench_c 完全相同）。
//!
//! `--allocs`：改为统计每个操作在 Rust 堆上的分配次数（stderr），不计时。
//! `--extra`：额外输出 get_copy_buffer（复制读取但不经过 Rust Vec）。

use std::alloc::{GlobalAlloc, Layout, System};
use std::hint::black_box;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::Instant;

use fengqudb::query::SelectTarget;
use fengqudb::{Durability, Options, Store};

/// 只在 `--allocs` 模式下计数的分配器；计时模式下只多一次 relaxed 加载。
struct Counting;
static COUNTING: AtomicBool = AtomicBool::new(false);
static ALLOCS: AtomicU64 = AtomicU64::new(0);

// SAFETY: 完全转发给 System 分配器，只额外做计数。
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if COUNTING.load(Ordering::Relaxed) {
            ALLOCS.fetch_add(1, Ordering::Relaxed);
        }
        System.alloc(layout)
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        System.dealloc(ptr, layout)
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if COUNTING.load(Ordering::Relaxed) {
            ALLOCS.fetch_add(1, Ordering::Relaxed);
        }
        System.realloc(ptr, layout, new_size)
    }
}

#[global_allocator]
static GLOBAL: Counting = Counting;

const N: u64 = 32768;

fn make_key(id: u64) -> [u8; 9] {
    let mut k = [0u8; 9];
    k[0] = b'k';
    k[1..].copy_from_slice(&id.to_be_bytes());
    k
}

fn report(op: &str, ops: u64, seconds: f64, sink: u64) {
    println!("{{\"op\":\"{op}\",\"ops\":{ops},\"ns_per_op\":{:.1},\"sink\":{sink}}}", seconds * 1e9 / ops as f64);
}

/// 计时循环：每轮 `batch` 次调用，累计 `per_batch_ops` 个操作，至少运行 `seconds` 秒。
fn timed(op: &str, seconds: f64, per_batch_ops: u64, mut batch: impl FnMut() -> u64) {
    let mut sink = 0u64;
    let mut ops = 0u64;
    let t0 = Instant::now();
    let elapsed = loop {
        sink = sink.wrapping_add(batch());
        ops += per_batch_ops;
        let e = t0.elapsed().as_secs_f64();
        if e >= seconds {
            break e;
        }
    };
    report(op, ops, elapsed, sink);
}

/// 统计 `batch` 运行期间的堆分配次数，折算为每操作。
fn count_allocs(op: &str, per_batch_ops: u64, rounds: u64, mut batch: impl FnMut() -> u64) {
    black_box(batch()); // 预热（首次调用可能初始化线程局部状态）
    ALLOCS.store(0, Ordering::SeqCst);
    COUNTING.store(true, Ordering::SeqCst);
    let mut sink = 0u64;
    for _ in 0..rounds {
        sink = sink.wrapping_add(batch());
    }
    COUNTING.store(false, Ordering::SeqCst);
    let n = ALLOCS.load(Ordering::SeqCst);
    eprintln!(
        "{{\"op\":\"{op}\",\"allocs\":{n},\"ops\":{},\"allocs_per_op\":{:.4},\"sink\":{sink}}}",
        per_batch_ops * rounds,
        n as f64 / (per_batch_ops * rounds) as f64
    );
}

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len() / 2).map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).expect("hex")).collect()
}

fn varint(out: &mut Vec<u8>, mut v: u64) {
    while v >= 128 {
        out.push((v | 128) as u8);
        v >>= 7;
    }
    out.push(v as u8);
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let positional: Vec<&String> = args.iter().filter(|a| !a.starts_with("--")).collect();
    let dir = positional.first().map(|s| s.as_str()).unwrap_or("/tmp/fqdb-bench");
    let seconds: f64 = positional.get(1).and_then(|s| s.parse().ok()).unwrap_or(1.0);
    let alloc_mode = args.iter().any(|a| a == "--allocs");
    let extra = args.iter().any(|a| a == "--extra");

    // 预算取值与 bench_c.c 相同。
    let options = Options {
        durability: Durability::Buffered,
        create: true,
        cache_bytes: 2 << 30,
        delta_bytes: 16 << 20,
        writer_bytes: 32 << 20,
        transaction_bytes: 64 << 20,
        all_transaction_bytes: 256 << 20,
        pinned_bytes: 2 << 30,
        max_snapshots: 1024,
        ..Options::default()
    };
    let db = Store::open(dir, &options).expect("open");
    let mut value = [0u8; 256];
    for i in 0..N {
        value.fill((i & 0xff) as u8);
        db.put(&make_key(i), &value).expect("put");
    }
    db.sync().expect("sync");

    // 1. 准备键 + 共享读取
    {
        let pk = db.prepare_key(&make_key(4242)).expect("prepare");
        let batch = || {
            let mut sink = 0u64;
            for _ in 0..1000 {
                let sv = pk.get_shared().expect("get_shared").expect("hit");
                let bytes = sv.as_bytes();
                sink += u64::from(bytes[bytes.len() - 1]);
            }
            sink
        };
        if alloc_mode {
            count_allocs("get_shared_prepared", 1000, 100, batch);
        } else {
            timed("get_shared_prepared", seconds, 1000, batch);
        }
    }
    // 2. 按键共享读取
    {
        let mut id = 0u64;
        let batch = || {
            let mut sink = 0u64;
            for _ in 0..1000 {
                let key = make_key(id.wrapping_mul(2654435761) % N);
                id += 1;
                let sv = db.get_shared(&key).expect("get_shared").expect("hit");
                sink += u64::from(sv.as_bytes()[0]);
            }
            sink
        };
        if alloc_mode {
            count_allocs("get_shared_key", 1000, 100, batch);
        } else {
            timed("get_shared_key", seconds, 1000, batch);
        }
    }
    // 3. 复制读取（调用方拥有 Vec<u8>）
    {
        let mut id = 0u64;
        let batch = || {
            let mut sink = 0u64;
            for _ in 0..1000 {
                let key = make_key(id.wrapping_mul(2654435761) % N);
                id += 1;
                let v = db.get(&key).expect("get").expect("hit");
                sink += u64::from(v[0]);
            }
            sink
        };
        if alloc_mode {
            count_allocs("get_copy", 1000, 100, batch);
        } else {
            timed("get_copy", seconds, 1000, batch);
        }
    }
    if extra {
        let mut id = 0u64;
        let batch = || {
            let mut sink = 0u64;
            for _ in 0..1000 {
                let key = make_key(id.wrapping_mul(2654435761) % N);
                id += 1;
                let v = db.get_buffer(&key).expect("get").expect("hit");
                sink += u64::from(v.as_bytes()[0]);
            }
            sink
        };
        if alloc_mode {
            count_allocs("get_copy_buffer", 1000, 100, batch);
        } else {
            timed("get_copy_buffer", seconds, 1000, batch);
        }
    }
    // 4. 32 键批量读取器：每个操作 = 1 条记录
    {
        let keys: Vec<[u8; 9]> = (0..32u64).map(|i| make_key(i * 997)).collect();
        let key_refs: Vec<&[u8]> = keys.iter().map(|k| &k[..]).collect();
        let mut reader = db.reader(&key_refs).expect("reader");
        let batch = || {
            let mut sink = 0u64;
            for _ in 0..200 {
                let rows = reader.read().expect("read");
                for row in rows.iter() {
                    sink += u64::from(row.value()[0]);
                }
            }
            sink
        };
        if alloc_mode {
            count_allocs("reader32_per_record", 200 * 32, 100, batch);
        } else {
            timed("reader32_per_record", seconds, 200 * 32, batch);
        }
    }
    // 5. 查询：请求字节来自 FQDB_BENCH_SELECT_HEX；数据与 bench_c 相同。
    if let Ok(hex) = std::env::var("FQDB_BENCH_SELECT_HEX") {
        for i in 0..64u64 {
            let mut buf = Vec::new();
            buf.push(0x08);
            varint(&mut buf, i);
            let class: &[u8] = if i % 2 == 1 { b"mage" } else { b"warrior" };
            buf.push(0x12);
            buf.push(class.len() as u8);
            buf.extend_from_slice(class);
            buf.push(0x18);
            varint(&mut buf, i % 20);
            db.put(format!("p/{i}").as_bytes(), &buf).expect("put p");
        }
        let request = unhex(&hex);
        let batch = || {
            let mut sink = 0u64;
            for _ in 0..200 {
                let out = db.select_request(&request).expect("select");
                sink += out.as_bytes().len() as u64;
            }
            sink
        };
        if alloc_mode {
            count_allocs("select_topn", 200, 100, batch);
        } else {
            timed("select_topn", seconds, 200, batch);
        }
    }
}
