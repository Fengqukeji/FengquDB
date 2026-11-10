import json
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'bindings/python'))
from fengqudb import Store
from store_fixture_cleanup import inspect_owned_store
path,library,owner=map(Path,sys.argv[1:])
assert not path.exists()
with Store(path,library=library,segmented=True,buffered=True,cache_bytes=0) as store:
    value=b'B'*(8<<20)
    for revision in range(10):
        store.put_if_changed(b'ballast',bytes([revision])+value[1:])
    store.delete_many([b'ballast']);store.checkpoint()
    assert store.get(b'ballast') is None
metadata=inspect_owned_store(path,owner)[1]
assert metadata['segments']>=2
print(json.dumps(metadata),flush=True)
