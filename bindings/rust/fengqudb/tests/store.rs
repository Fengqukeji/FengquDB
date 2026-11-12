//! 读写、事务、未命中、空值、过期、关闭后报错、共享视图生命周期、并发读取。

mod common;

use common::{now_ms, open, test_options, TempDir};
use fengqudb::{status, Error, FieldType, IndexField, Store, INDEX_REQUIRED};

fn assert_send_sync<T: Send + Sync>() {}
fn assert_send<T: Send>() {}

#[test]
fn marker_traits() {
    assert_send_sync::<Store>();
    assert_send_sync::<fengqudb::PreparedKey>();
    assert_send_sync::<fengqudb::SharedValue>();
    assert_send_sync::<fengqudb::Buffer>();
    assert_send_sync::<fengqudb::QueryResult>();
    assert_send::<fengqudb::Reader>();
    assert_send::<fengqudb::Transaction<'static>>();
    fn is_error<E: std::error::Error + Send + Sync + 'static>() {}
    is_error::<Error>();
}

#[test]
fn put_get_delete_miss_empty() {
    let dir = TempDir::new("crud");
    let db = open(&dir);
    assert_eq!(db.get(b"absent").unwrap(), None);
    assert!(db.get_shared(b"absent").unwrap().is_none());
    let v1 = db.put(b"k", b"hello").unwrap();
    assert_eq!(db.get(b"k").unwrap().as_deref(), Some(&b"hello"[..]));
    let v2 = db.put(b"k", b"world").unwrap();
    assert!(v2 > v1);
    assert_eq!(db.get(b"k").unwrap().as_deref(), Some(&b"world"[..]));
    // 空值是命中，不是未命中。
    db.put(b"empty", b"").unwrap();
    assert_eq!(db.get(b"empty").unwrap(), Some(Vec::new()));
    let shared = db.get_shared(b"empty").unwrap().expect("empty value is a hit");
    assert!(shared.is_empty());
    assert_eq!(shared.as_bytes(), b"");
    db.delete(b"k").unwrap();
    assert_eq!(db.get(b"k").unwrap(), None);
    db.delete(b"never-existed").unwrap();
    db.sync().unwrap();
    assert!(db.info().unwrap().stored_keys >= 1);
}

#[test]
fn expiry() {
    let dir = TempDir::new("ttl");
    let db = open(&dir);
    db.put_with_expiry(b"soon", b"v", now_ms() + 150).unwrap();
    db.put_with_expiry(b"later", b"v", now_ms() + 3_600_000).unwrap();
    assert!(db.get(b"soon").unwrap().is_some());
    std::thread::sleep(std::time::Duration::from_millis(400));
    assert_eq!(db.get(b"soon").unwrap(), None);
    assert!(db.get_shared(b"soon").unwrap().is_none());
    let later = db.get_shared(b"later").unwrap().unwrap();
    assert!(later.expires_at_ms() > now_ms());
    // 已过期的时间点：写入后读不到。
    db.put_with_expiry(b"past", b"v", now_ms().saturating_sub(10_000).max(1)).unwrap();
    assert_eq!(db.get(b"past").unwrap(), None);
}

#[test]
fn transactions() {
    let dir = TempDir::new("txn");
    let db = open(&dir);
    db.put(b"a", b"1").unwrap();
    let mut txn = db.begin(0).unwrap();
    txn.put(b"b", b"2").unwrap();
    txn.delete(b"a").unwrap();
    // 事务内读到自己的写入，外部尚不可见。
    assert_eq!(txn.get(b"b").unwrap().as_deref(), Some(&b"2"[..]));
    assert_eq!(txn.get(b"a").unwrap(), None);
    assert_eq!(db.get(b"b").unwrap(), None);
    assert_eq!(db.get(b"a").unwrap().as_deref(), Some(&b"1"[..]));
    let version = txn.commit().unwrap();
    assert!(version > 0);
    assert_eq!(db.get(b"b").unwrap().as_deref(), Some(&b"2"[..]));
    assert_eq!(db.get(b"a").unwrap(), None);

    // 放弃（Drop）即回滚。
    let mut txn = db.begin(0).unwrap();
    txn.put(b"rolled", b"x").unwrap();
    drop(txn);
    assert_eq!(db.get(b"rolled").unwrap(), None);

    // put_many：同一键最后一个生效。
    let mut txn = db.begin(0).unwrap();
    txn.put_many(&[(b"m1", b"one"), (b"m2", b"two"), (b"m1", b"uno")]).unwrap();
    txn.commit().unwrap();
    assert_eq!(db.get(b"m1").unwrap().as_deref(), Some(&b"uno"[..]));
    assert_eq!(db.get(b"m2").unwrap().as_deref(), Some(&b"two"[..]));
}

#[test]
fn transaction_conflict_reports_status() {
    let dir = TempDir::new("conflict");
    let db = open(&dir);
    db.put(b"c", b"0").unwrap();
    let mut t1 = db.begin(0).unwrap();
    assert_eq!(t1.get(b"c").unwrap().as_deref(), Some(&b"0"[..]));
    t1.put(b"c", b"1").unwrap();
    db.put(b"c", b"other").unwrap();
    let err = t1.commit().expect_err("read-write conflict");
    assert!(err.is_status(status::CONFLICT), "{err}");
    assert_eq!(db.get(b"c").unwrap().as_deref(), Some(&b"other"[..]));
}

#[test]
fn snapshot_isolated_reads() {
    let dir = TempDir::new("snap");
    let db = open(&dir);
    db.put(b"s", b"old").unwrap();
    let snap = db.snapshot().unwrap();
    db.put(b"s", b"new").unwrap();
    assert_eq!(snap.get(b"s").unwrap().as_deref(), Some(&b"old"[..]));
    assert_eq!(snap.get_shared(b"s").unwrap().unwrap().as_bytes(), b"old");
    assert_eq!(db.get(b"s").unwrap().as_deref(), Some(&b"new"[..]));
    assert!(snap.get(b"nope").unwrap().is_none());
}

#[test]
fn use_after_close_is_an_error() {
    let dir = TempDir::new("closed");
    let db = open(&dir);
    db.put(b"k", b"v").unwrap();
    let clone = db.clone();
    db.close();
    assert!(db.is_closed() && clone.is_closed());
    for result in [
        db.put(b"k", b"w").map(|_| ()),
        db.get(b"k").map(|_| ()),
        db.get_shared(b"k").map(|_| ()),
        db.delete(b"k"),
        db.sync(),
        db.prepare_key(b"k").map(|_| ()),
        db.reader(&[b"k"]).map(|_| ()),
        db.begin(0).map(|_| ()),
        db.snapshot().map(|_| ()),
        db.drop_index("x"),
    ] {
        assert_eq!(result, Err(Error::Closed));
    }
    db.close(); // 幂等
    assert_eq!(Error::Closed.to_string(), "store is closed");
}

#[test]
fn open_errors_carry_status_codes() {
    let dir = TempDir::new("noexist");
    let mut options = test_options();
    options.create = false;
    let err = Store::open(dir.path(), &options).expect_err("missing directory without CREATE");
    let code = err.code().expect("native status code");
    assert_ne!(code, status::OK);
    let text = err.to_string();
    assert!(text.contains("fqdb_store_open") && text.contains(&code.to_string()), "{text}");
    // 全 0 选项会被原生库拒绝（参见规范）。
    assert!(Store::open("\0bad", &test_options()).is_err());
}

#[test]
fn shared_view_survives_writes_and_store_close() {
    let dir = TempDir::new("shared");
    let db = open(&dir);
    let payload = vec![0xABu8; 4096];
    db.put(b"big", &payload).unwrap();
    let baseline = db.info().unwrap().pinned_bytes;
    let view = db.get_shared(b"big").unwrap().unwrap();
    let prepared = db.prepare_key(b"big").unwrap();
    let prepared_view = prepared.get_shared().unwrap().unwrap();
    assert_eq!(view.as_bytes(), &payload[..]);
    assert!(view.version() > 0);
    assert!(db.info().unwrap().pinned_bytes >= baseline);

    // 覆盖、删除、同步之后旧视图不变。
    db.put(b"big", &vec![0x11u8; 4096]).unwrap();
    assert_eq!(view.as_bytes(), &payload[..]);
    db.delete(b"big").unwrap();
    db.sync().unwrap();
    assert_eq!(view.as_bytes(), &payload[..]);
    assert!(db.get_shared(b"big").unwrap().is_none());
    assert!(prepared.get_shared().unwrap().is_none());
    assert_eq!(prepared_view.as_bytes(), &payload[..]);

    // 关闭并丢弃全部 Store 句柄：原生库关闭，视图仍有效。
    db.close();
    drop(prepared);
    drop(db);
    assert_eq!(view.as_bytes(), &payload[..]);
    assert_eq!(prepared_view.as_bytes(), &payload[..]);
    drop(view);
    drop(prepared_view);
}

#[test]
fn pinned_budget_is_released_on_drop() {
    let dir = TempDir::new("pins");
    let db = open(&dir);
    for i in 0..200u32 {
        db.put(&i.to_be_bytes(), &[i as u8; 1024]).unwrap();
    }
    let before = db.info().unwrap().pinned_bytes;
    let held: Vec<_> = (0..200u32).map(|i| db.get_shared(&i.to_be_bytes()).unwrap().unwrap()).collect();
    let during = db.info().unwrap().pinned_bytes;
    assert!(during > before, "held leases must be charged: {before} -> {during}");
    drop(held);
    assert_eq!(db.info().unwrap().pinned_bytes, before, "dropping leases returns the budget");
}

#[test]
fn prepared_key_and_reader() {
    let dir = TempDir::new("reader");
    let db = open(&dir);
    db.put(b"a", b"alpha").unwrap();
    db.put(b"e", b"").unwrap();
    let mut reader = db.reader(&[b"a", b"missing", b"e", b"a"]).unwrap();
    assert_eq!(reader.key_count(), 4);
    {
        let rows = reader.read().unwrap();
        assert_eq!(rows.len(), 4);
        let r: Vec<_> = rows.iter().map(|r| (r.found(), r.value().to_vec())).collect();
        assert_eq!(r, vec![(true, b"alpha".to_vec()), (false, vec![]), (true, vec![]), (true, b"alpha".to_vec())]);
        assert!(rows.get(4).is_none());
        assert!(rows.get(0).unwrap().version() > 0);
    }
    // 写入后再次读取看到新值。
    db.put(b"a", b"ALPHA").unwrap();
    db.put(b"missing", b"now").unwrap();
    {
        let rows = reader.read().unwrap();
        assert_eq!(rows.get(0).unwrap().value(), b"ALPHA");
        assert!(rows.get(1).unwrap().found());
        assert_eq!(rows.get(1).unwrap().value(), b"now");
    }
    reader.release();
    let rows = reader.read().unwrap();
    assert_eq!(rows.get(3).unwrap().value(), b"ALPHA");

    let empty = db.reader(&[]).unwrap().read().map(|r| r.len());
    assert_eq!(empty, Ok(0));

    // Store 关闭并丢弃之后，读取器与准备键仍可用（子句柄持有底层库）。
    let mut reader2 = db.reader(&[b"a"]).unwrap();
    let key = db.prepare_key(b"a").unwrap();
    db.close();
    drop(db);
    assert_eq!(reader2.read().unwrap().get(0).unwrap().value(), b"ALPHA");
    assert_eq!(key.get_shared().unwrap().unwrap().as_bytes(), b"ALPHA");
}

#[test]
fn concurrent_readers_with_a_writer() {
    let dir = TempDir::new("threads");
    let db = open(&dir);
    for i in 0..64u32 {
        db.put(&i.to_be_bytes(), &i.to_le_bytes()).unwrap();
    }
    let stop = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let mut handles = Vec::new();
    for t in 0..8u32 {
        let db = db.clone();
        let stop = stop.clone();
        handles.push(std::thread::spawn(move || {
            let keys: Vec<[u8; 4]> = (0..64u32).map(|i| ((i + t) % 64).to_be_bytes()).collect();
            let key_refs: Vec<&[u8]> = keys.iter().map(|k| &k[..]).collect();
            let mut reader = db.reader(&key_refs).unwrap();
            let prepared = db.prepare_key(&keys[0]).unwrap();
            let mut reads = 0u64;
            while !stop.load(std::sync::atomic::Ordering::Relaxed) || reads < 200 {
                let rows = reader.read().unwrap();
                for (j, row) in rows.iter().enumerate() {
                    assert!(row.found());
                    // 值要么是原始值，要么是写线程写的同长度新值。
                    assert_eq!(row.value().len(), 4, "row {j}");
                }
                let v = prepared.get_shared().unwrap().unwrap();
                assert_eq!(v.len(), 4);
                reads += 1;
            }
            reads
        }));
    }
    for round in 0..100u32 {
        for i in 0..64u32 {
            db.put(&i.to_be_bytes(), &(i + round).to_le_bytes()).unwrap();
        }
    }
    stop.store(true, std::sync::atomic::Ordering::Relaxed);
    let total: u64 = handles.into_iter().map(|h| h.join().expect("reader thread")).sum();
    assert!(total >= 8 * 200);
}

#[test]
fn compound_index_define_and_drop() {
    let dir = TempDir::new("index");
    let db = open(&dir);
    let fields = [IndexField { field: 2, ty: FieldType::Bytes }, IndexField { field: 3, ty: FieldType::Uint64 }];
    db.define_compound_index("cls-level", b"p/", &fields, INDEX_REQUIRED).unwrap();
    // 同名同参数重复定义的行为由原生库决定；这里只要求删除后可再次定义。
    db.drop_index("cls-level").unwrap();
    db.define_compound_index("cls-level", b"p/", &fields, 0).unwrap();
    assert!(db.define_compound_index("bad", b"p/", &[], 0).is_err());
    let err = db.drop_index("does-not-exist").expect_err("unknown index");
    assert!(err.code().is_some());
}
