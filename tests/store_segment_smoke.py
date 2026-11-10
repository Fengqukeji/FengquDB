import json
import os
from pathlib import Path
import struct
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'bindings/python'))
from fengqudb import Store
from store_fixture_cleanup import cleanup_owned_store,inspect_owned_store,FixtureError
from store_bindings_smoke import operations

path,library=Path(sys.argv[1]),Path(sys.argv[2])
base,archive,restored=[path.with_name(path.name+suffix) for suffix in ['-base','-increment','-restored']]
def need(ok):
    if not ok:raise AssertionError('segmented binding model')
payload=bytearray(b'p'*(8<<20))
options={'library':library,'segmented':True,'buffered':True,'cache_bytes':4<<20}
with Store(path,**options) as store:
    for index in range(5):
        struct.pack_into('<Q',payload,0,index);store.put_if_changed(b'blob/large',bytes(payload))
    with store.snapshot() as old:
        expected_old=bytes(payload)
        for index in range(5,10):
            struct.pack_into('<Q',payload,0,index);store.put_if_changed(b'blob/large',bytes(payload))
        operations(store);store.checkpoint()
        need(store.get(b'blob/large')==payload and old.get(b'blob/large')==expected_old)
        files=[p for p in path.iterdir() if p.suffix=='.fdb']
        need(len(files)>=2 and max(p.stat().st_size for p in files)<=((64<<20)+65536))
        before=len(files)
        from_version=store.backup(base).snapshot_version;store.hold_history(b'chain',from_version)
        struct.pack_into('<Q',payload,0,10);store.put_if_changed(b'blob/large',bytes(payload))
        store.compact();need(old.get(b'blob/large')==expected_old)
        need(store.history_info().retained_generations>=1)
        store.backup_incremental(archive,from_version)
    store.release_history(b'chain');store.compact();store.verify()
    need(store.history_info().retained_generations==0 and store.get(b'blob/large')==payload)
with Store(path,library=library,create=False) as store:need(store.get(b'blob/large')==payload)
Store.recover_chain(base,[archive],restored,library=library)
with Store(restored,library=library) as copy:
    need(copy.get(b'blob/large')==payload);copy.verify()
owner=Path(os.environ['FQDB_TEST_OWNED_ROOT'])
# A cleaner must reject a foreign file before deleting any acknowledged data.
before_names=set(p.name for p in path.iterdir());foreign=path/'foreign.note';foreign.write_text('owned negative fixture',encoding='utf-8')
try:cleanup_owned_store(path,owner)
except FixtureError:pass
else:raise AssertionError('cleanup accepted a foreign file')
need(before_names|{'foreign.note'}==set(p.name for p in path.iterdir()));foreign.unlink()
metadata=inspect_owned_store(path,owner)[1]
for output in [path,base,archive,restored]:cleanup_owned_store(output,owner)
print(json.dumps({'ok':True,'language':'python','data_segments_before_compaction':before,
                  'after':metadata,'bytes_per_value':len(payload)}),flush=True)
