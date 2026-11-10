"""Public Python configuration, deferred completion, ownership and restart."""
import asyncio,json,sys,struct
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'bindings/python'))
from fengqudb import Store,Error,StoreWriteOptions

async def exercise(path,library):
    defaults=StoreWriteOptions()
    assert (defaults.mode,defaults.period_ms,defaults.threshold_bytes)==(1,50,1048576)
    with Store(path,library=library) as db:
        p=db.write_options();assert (p.mode,p.period_ms,p.threshold_bytes)==(1,50,1048576)
        db.configure_write(period_ms=5000)
        original=b'\x09'+struct.pack('<Q',7)+b'\x12\x04body'
        projected=original[:9]
        db.put(b'project',original)
        assert db.project(b'project',[1,1])==projected and db.project(b'project',[])==b''
        with db.prepare_project_reader([b'project',b'missing'],[1]) as reader:
            with reader.read_shared() as held:
                alias=held[0].value
                assert bytes(alias)==projected and held[1] is None
                db.put(b'project',b'\x09'+struct.pack('<Q',8))
                with reader.read_shared() as fresh: assert bytes(fresh[0].value)[1]==8
                reader.release()
                assert bytes(alias)==projected
                alias.release()
        db.put(b'field-a',struct.pack('<q',100));db.put(b'field-b',struct.pack('<q',200))
        result=db.add_i64_fields([(b'field-a',0,-10,0,1000),(b'field-b',0,10,0,1000)],force_reliable=True)
        assert result.changed and result.value==210 and db.write_state().durable_version>=result.version
        assert struct.unpack('<q',db.get(b'field-a'))[0]==90
        before=db.info().version
        try:
            db.add_i64_fields([(b'field-a',0,-1),(b'field-b',0,-211,0,1000)])
            raise AssertionError('invalid field update succeeded')
        except Error: pass
        assert db.info().version==before and struct.unpack('<q',db.get(b'field-a'))[0]==90
        tx=db.transaction();tx.put(b'tx',b'value')
        v=tx.commit();tx.close()
        assert db.get(b'tx')==b'value' and db.write_state().durable_version<v
        tx=db.transaction();tx.put(b'forced-tx',b'yes')
        forced=tx.commit(force_reliable=True);tx.close()
        assert db.write_state().durable_version>=forced
        db.put(b'paid',b'yes',force_reliable=True)
        assert db.write_state().durable_version>=v
        db.patch(b'paid',0,3,b'new',force_reliable=True)
        db.delete(b'paid',force_reliable=True)
        assert db.get(b'paid') is None
        tx=db.transaction();tx.put(b'async',b'value')
        receipt=tx.submit(force_reliable=True);tx.close()
        assert await receipt.wait_async()>0;receipt.close()
        await db.configure_write_async(period_ms=5000,threshold_bytes=4096)
        data=bytearray(b'a'*1024)
        receipts=[db.submit_put(('k%d'%i).encode(),data) for i in range(4)]
        data[:]=b'b'*1024
        for receipt in receipts:assert await receipt.wait_async()>0;receipt.close()
        assert db.get(b'k0')==b'a'*1024
        db.configure_write(mode="reliable",period_ms=37,threshold_bytes=32768)
        with db.transaction() as tx:tx.put(b'strict',b'yes')
    with Store(path,library=library) as db:
        p=db.write_options();assert (p.mode,p.period_ms,p.threshold_bytes)==(0,37,32768)
        assert db.get(b'strict')==b'yes'
        assert struct.unpack('<q',db.get(b'field-b'))[0]==210
        await db.reset_write_options_async();p=db.write_options()
        assert (p.mode,p.period_ms,p.threshold_bytes)==(1,50,1048576)
        with db.transaction() as tx:
            tx.delete(b'strict');receipt=tx.submit()
        db.sync();assert await receipt.wait_async()>0;receipt.close()
        assert db.get(b'strict') is None
    with Store(path,library=library) as db:
        assert db.write_options().period_ms==50
        db.configure_write()
        assert db.write_options().period_ms==50
    print(json.dumps(dict(ok=True,language='python',periodic=True,strict=True,reopen=True)))

if __name__=='__main__':asyncio.run(exercise(sys.argv[1],sys.argv[2]))
