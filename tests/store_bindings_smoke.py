import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bindings" / "python"))
from fengqudb import Store as NativeStore, Error, StoreOperation as Op, StoreMaintenanceIO

import os
from store_fixture_cleanup import cleanup_owned_store
class Store(NativeStore):
    def __init__(self,*args,**options):
        options.setdefault('segmented',os.environ.get('FQDB_TEST_SEGMENTED')=='1')
        super().__init__(*args,**options)
        if self.write_options().mode: self.configure_write(mode="reliable")

def queries(store):
    first=b"\x0a\x01a\x10\x02\x1a\x03one"
    second=b"\x0a\x01a\x10\x04\x1a\x03two"
    store.put_many([(b"ops/query/1",first),(b"ops/query/2",second)])
    store.define_compound_index(b"ops-query",b"ops/query/",[(1,4),(2,5)],3)
    rows=store.query_index(b"ops-query",equal=b"\x0a\x01a",lower=b"\x10\x02",upper=b"\x10\x04",upper_inclusive=True,fields=[3]).rows
    assert [(r[0],r[1]) for r in rows]==[(b"ops/query/1",b"\x1a\x03one"),(b"ops/query/2",b"\x1a\x03two")]
    assert store.project(b"ops/query/1",[3])==b"\x1a\x03one"
    with store.transaction() as tx:
        tx.put(b"ops/query/3",b"\x0a\x01a\x10\x06")
        assert len(tx.query_index(b"ops-query",equal=b"\x0a\x01a").rows)==3
        tx.put(b"ops/query/3",b"\x0a\x01b\x10\x06")
        assert len(tx.query_index(b"ops-query",equal=b"\x0a\x01a").rows)==2
    store.verify()

def operations(store):
    queries(store)
    assert store.put_if_absent(b"ops/cas",b"one").changed
    entry=store.get_entry(b"ops/cas")
    assert entry.value==b"one" and store.compare_exchange(b"ops/cas",entry.version,b"two").matched
    assert not store.compare_exchange(b"ops/cas",entry.version,b"wrong").matched
    assert not store.put_if_changed(b"ops/cas",b"two").changed
    assert store.add_i64(b"ops/counter",3,0,10).value==3
    try: store.add_i64(b"ops/counter",-4,0,10); raise AssertionError("counter bound")
    except Error as error: assert error.code==20
    store.put_many([(b"ops/a",b"first"),(b"ops/empty",b""),(b"ops/a",b"last")])
    assert store.get_many([b"ops/a",b"ops/missing",b"ops/empty",b"ops/a"])==[b"last",None,b"",b"last"]
    assert store.get_entries([b"ops/a"])[0].version>0
    with store.snapshot() as snap,store.prepare_key(b"ops/a") as key:
        store.put_if_changed(b"ops/a",b"new")
        assert snap.get_many([b"ops/a"])[0]==b"last" and key.get()==b"new"
        buffer=bytearray(16)
        assert key.get_into(buffer)==3 and buffer[:3]==b"new"
        store.checkpoint()
        assert key.get()==b"new"
    batch=store.prepare_batch([(b"ops/prepared",b"payload")])
    with store.transaction() as tx:
        tx.put_batch(batch); batch.close()
        assert tx.get_many([b"ops/prepared"])[0]==b"payload"
        assert tx.get_entries([b"ops/prepared"])[0].version==0
        tx.patch(b"ops/prepared",0,1,b"P")
    assert store.get(b"ops/prepared")==b"Payload"
    store.delete_many([b"ops/a",b"ops/empty"])
    assert store.get_many([b"ops/a",b"ops/empty"])==[None,None]

def history_roundtrip(path,library):
    root=path.with_name(path.name+'-history');base=path.with_name(path.name+'-history-base')
    archive=path.with_name(path.name+'-history-inc');copy=path.with_name(path.name+'-history-copy')
    with Store(root,library=library) as db:
        db.put_if_changed(b'key',b'A');db.hold_history(b'backup');from_version=db.backup(base).snapshot_version
        db.hold_history(b'backup',from_version);db.put_if_changed(b'key',b'B');db.compact()
        assert db.history_info().floor==from_version and db.history_info().retained_generations==1
    with Store(root,library=library) as db:
        assert db.history_info().active_pins==1
        db.backup_incremental(archive,from_version);db.release_history(b'backup');db.compact()
        assert db.history_info().retained_generations==0
    Store.recover_chain(base,[archive],copy,library=library)
    with Store(copy,library=library) as db:assert db.get(b'key')==b'B' and db.history_info().active_pins==0
    for p in (root,base,archive,copy):cleanup(p)

def watch_roundtrip(path,library):
    import concurrent.futures,time
    root=path.with_name(path.name+'-watch')
    with Store(root,library=library) as db:
        with db.watch(prefix=b'watch/') as watch:
            db.put_many([(b'watch/a',b'one'),(b'watch/empty',b'')])
            db.compact()
            batch=watch.next_batch()
            assert [(r.key,r.value,r.kind) for r in batch.rows]==[(b'watch/a',b'one',1),(b'watch/empty',b'',1)]
            assert watch.next_batch().version==batch.version
            watch.ack(batch.version)
            assert watch.next_batch(1) is None
        with db.watch(metadata_only=True) as watch:
            db.put_if_changed(b'watch/a',b'two')
            batch=watch.next_batch();assert batch.rows[0].value is None and batch.rows[0].kind==1
            watch.ack(batch.version)
        watch=db.watch()
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            future=pool.submit(watch.next_batch,5000)
            time.sleep(.02);assert not future.done()
            watch.close()
            try:future.result();raise AssertionError('close did not cancel watch')
            except Error as e:assert e.code==19
        watch.close()
        db.compact();db.verify()
    cleanup(root)

def io_budget_roundtrip(path,library):
    root=path.with_name(path.name+'-io');backup=path.with_name(path.name+'-io-base')
    stats=StoreMaintenanceIO()
    with Store(root,library=library) as db:
        db.put_if_changed(b'key',b'x'*64000)
        try:db.verify(max_read_bytes=1,io_stats=stats);raise AssertionError('read quota ignored')
        except Error as error:assert error.code==9 and stats.read_bytes<=1 and stats.write_bytes==0
        db.backup(backup,io_stats=stats);assert stats.write_bytes>64000 and stats.sync_calls>0
        try:db.compact(max_write_bytes=1,io_stats=stats);raise AssertionError('write quota ignored')
        except Error as error:assert error.code==9 and stats.write_bytes<=1
        assert db.get(b'key')==b'x'*64000
        db.put_if_changed(b'doc/1',b'\x08\x01')
        try:db.define_index(b'io-index',b'doc/',1,1,3,max_write_bytes=1,io_stats=stats);raise AssertionError('index quota ignored')
        except Error as error:assert error.code==9 and stats.write_bytes<=1
        db.define_index(b'io-index',b'doc/',1,1,3,io_stats=stats);assert stats.write_bytes>0
        db.configure(job_max_read_bytes=65536,job_max_write_bytes=65536);db.configure()
        db.compact();db.verify()
    cleanup(root);cleanup(backup)

def main():
    path, library = Path(sys.argv[1]), Path(sys.argv[2])
    if len(sys.argv) > 3:
        with Store(path, library=library) as store:
            if sys.argv[3] == "seed":
                with store.transaction() as tx:
                    tx.put("跨语言".encode(), b"\x08\x96\x01\x12\x02\x00\xff")
                    tx.put(b"python-expired", b"expired", 1)
                    tx.put(b"inter/1",b"\x0a\x06python")
                store.execute(Op.HASH_SET,b"interop-hash",b"python",b"\x00\xff")
                store.define_index(b"interop-index",b"inter/",1,4,3)
                assert store.add_i64(b"interop-counter",1).value==1
                store.put_many([(b"interop-bulk",b"python"),(b"interop-empty",b"")])
            else:
                assert store.get("跨语言".encode()) == b"\x08\x96\x01\x12\x02\x00\xff"
                assert store.get(b"java") == b"java" and store.get(b"csharp") == b"csharp"
                assert store.hash_get(b"interop-hash",b"python")==b"\x00\xff"
                assert store.hash_get(b"interop-hash",b"java")==b"java" and store.hash_get(b"interop-hash",b"csharp")==b"csharp"
                assert store.execute(Op.INDEX_LOOKUP,b"interop-index",value=b"\x0a\x06python").rows[0][0]==b"inter/1"
                assert int.from_bytes(store.get(b"interop-counter"),"little",signed=True)==6
                assert store.get_many([b"interop-bulk",b"interop-java-batch",b"interop-csharp-batch",b"interop-empty"])==[b"csharp",b"java",b"csharp",b""]
        if sys.argv[3] == "seed":
            print("PASS Python seeded shared Protobuf bytes")
            return
        cleanup(path)
        print("PASS Python read Java/C# writes from the same Store directory")
        return
    assert not path.exists(), "fixture destination already exists"
    incremental = path.with_name(path.name+"-incremental")
    chain = path.with_name(path.name+"-chain")
    backup, restored = path.with_name(path.name + "-backup"), path.with_name(path.name + "-restored")
    with Store(path, library=library) as store:
        with store.transaction() as tx:
            tx.put(b"a", b"0123456789")
            tx.put(b"empty", b"")
        with store.snapshot() as old:
            with store.transaction() as tx:
                point = tx.savepoint()
                tx.patch(b"a", 2, 5, b"X")
                assert tx.get(b"a") == b"01X789"
                tx.rollback_to(point)
                tx.patch(b"a", 2, 3, b"ABC")
            assert old.get(b"a") == b"0123456789"
            assert store.get(b"a") == b"01ABC56789"
        with store.snapshot() as snap, snap.scan(reverse=True) as cursor:
            rows = list(cursor)
            assert [row[0] for row in rows] == [b"empty", b"a"]
            assert rows[0][1] == b""
        tx = store.transaction()
        try:
            tx.put(b"cancel", b"no")
            tx.cancel()
            try:
                tx.commit()
                raise AssertionError("cancelled transaction committed")
            except Error as error:
                assert error.code == 19
        finally:
            tx.close()
        with store.transaction() as tx:
            tx.expire(b"a", 1)
        assert store.get(b"a") is None
        purged = store.purge_expired(max_keys=1)
        assert purged.removed_keys == 1 and purged.removed_value_bytes == 10
        assert store.purge_expired().removed_keys == 0
        store.checkpoint()
        assert store.info().delta_bytes == 0
        version = store.info().version
        assert store.compact().final_version == version
        assert store.verify().stored_keys == 1
        assert store.backup(backup).snapshot_version == version
        operations(store)
        assert store.execute(Op.HASH_SET,b"hash",b"field",b"value").changed
        assert store.execute(Op.HASH_GET,b"hash",b"field").value == b"value"
        assert store.hash_get(b"hash",b"field")==b"value" and store.hash_get(b"hash",b"missing") is None
        assert store.execute(Op.SET_ADD,b"set",b"member").changed
        store.execute(Op.LIST_PUSH_RIGHT,b"list",value=b"right")
        assert store.execute(Op.LIST_GET,b"list",argument=-1).value == b"right"
        store.execute(Op.SORTED_ADD,b"scores",b"member",score=-1.5)
        assert store.execute(Op.SORTED_RANGE,b"scores").rows[0][2] == -1.5
        with store.transaction() as tx: tx.put(b"doc/1",b"\x0a\x01x")
        store.define_index(b"by-name",b"doc/",1,4,3)
        assert store.execute(Op.INDEX_LOOKUP,b"by-name",value=b"\x0a\x01x").rows[0][0] == b"doc/1"
        store.configure(group_max=8)
        assert store.runtime_info().group_transactions > 0
        store.backup_incremental(incremental,version)
        store.checkpoint()
        Store.recover_chain(backup,[incremental],chain,library=library)
        with Store(chain,library=library) as recovered:
            assert recovered.execute(Op.HASH_GET,b"hash",b"field").value == b"value"
            assert recovered.execute(Op.INDEX_LOOKUP,b"by-name",value=b"\x0a\x01x").rows[0][0] == b"doc/1"
            recovered.verify()
            recovered.drop_index(b"by-name")
    with Store(path, library=library) as reopened:
        assert reopened.get(b"empty") == b"" and reopened.get(b"a") is None
        assert reopened.info().recovered_transactions == 0
    Store.restore(backup, restored, library=library)
    with Store(restored, library=library) as copy: assert copy.get(b"empty") == b"" and copy.get(b"a") is None
    cleanup(incremental)
    cleanup(chain)
    cleanup(backup)
    cleanup(restored)
    cleanup(path)
    root=path.with_name(path.name+"-root-\u5b58\u6863")
    resolved=Path(Store.path_at(root,"slot1",library=library))
    assert resolved.is_absolute() and not root.exists()
    with Store.open_at(root,"slot1",library=library) as store:
        store.configure_write(mode="reliable")
        store.put_if_absent(b"root",b"configured")
    with Store.open_at(root,"slot1",library=library,create=False) as store:
        assert store.get(b"root")==b"configured"
    assert len(list(root.iterdir()))==1 and all(len(p.name)<=30 for p in resolved.iterdir())
    cleanup(resolved); root.rmdir()
    history_roundtrip(path,library)
    watch_roundtrip(path,library)
    io_budget_roundtrip(path,library)
    print("PASS Python Store binding, transactions, compact/verify/backup/restore and restart")

def cleanup(path):
    cleanup_owned_store(path,Path(os.environ['FQDB_TEST_OWNED_ROOT']))

if __name__ == "__main__": main()
