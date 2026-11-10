import gc,sys,weakref
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'bindings/python'))
import fengqudb
from fengqudb import Store, Error
root,lib=sys.argv[1:]
root=Path(root);root.mkdir(parents=True,exist_ok=True)
for segmented in (False,True):
    db=Store(Path(root)/('segments' if segmented else 'shared'),library=lib,buffered=True,segmented=segmented)
    db.put_many([(b'a',b'original'),(b'e',b'')])
    shared=db.get_shared(b'a'); view=shared.value[1:5]; exporter=weakref.ref(view.obj)
    assert shared.value.readonly and bytes(view)==b'rigi'
    try: shared.value[0]=3; raise AssertionError('writable')
    except TypeError: pass
    # Simulate host-wrapper allocation failure after native lease acquisition.
    baseline=db.info().pinned_bytes
    def no_memory(*args): raise MemoryError('injected host allocation failure')
    for symbol,call in [('StoreSharedValue',lambda: db.get_shared(b'a')),
                        ('StoreSharedBatch',lambda: db.get_many_shared([b'a']))]:
        original=getattr(fengqudb,symbol);setattr(fengqudb,symbol,no_memory)
        try:
            try: call();raise AssertionError('allocation failure ignored')
            except MemoryError: pass
        finally: setattr(fengqudb,symbol,original)
        gc.collect();assert db.info().pinned_bytes==baseline
    reader=db.prepare_reader([b'a',b'missing',b'e',b'a'])
    batch=reader.read_shared(); old=batch[0].value[:]
    assert batch[1] is None and batch[2].value.nbytes==0 and bytes(old)==b'original'
    with db.transaction() as tx: tx.patch(b'a',0,8,b'new'); tx.delete(b'e')
    db.checkpoint();db.compact()
    shared.close();batch.close();reader.release();gc.collect()
    assert bytes(view)==b'rigi' and bytes(old)==b'original' and exporter() is not None
    with reader.read_shared() as fresh:
        assert bytes(fresh[0].value)==b'new' and fresh[2] is None
    assert db.get_shared(b'missing') is None
    with db.get_many_shared([b'a',b'missing']) as fresh: assert bytes(fresh[0].value)==b'new' and fresh[1] is None
    with db.prepare_key(b'a') as key:
        value=key.get_shared(); assert bytes(value.value)==b'new';value.close()
    db.close()
    with reader.read_shared() as fresh: assert bytes(fresh[0].value)==b'new'
    reader.close(); del shared,batch,old
    gc.collect();assert bytes(view)==b'rigi'
    view.release();del view;gc.collect();assert exporter() is None
with Store(Path(root)/'limited',library=lib,buffered=True,pinned_bytes=65536) as db:
    db.put_many([(b'big',b'x'*131072)])
    assert db.get(b'big')==b'x'*131072
    try: db.get_shared(b'big');raise AssertionError('pin limit ignored')
    except Error as e: assert e.code==9
    assert db.get(b'big')==b'x'*131072
print('PASS Python shared and derived views, COW, close/next read/compaction, collection and pin fallback')
