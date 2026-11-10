import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid
sys.path.insert(0, str(Path(__file__).resolve().parent))
from store_bindings_smoke import Store, Op, cleanup

def main():
    cli, library = (Path(p).resolve() for p in sys.argv[1:3])
    root = Path(tempfile.gettempdir()) / ("fqdb-features-cli-" + uuid.uuid4().hex)
    root.mkdir()
    source, base, archive, recovered, point = (root / n for n in ("源库", "基础备份", "增量", "恢复", "版本点"))
    with Store(source, library=library) as store:
        with store.transaction() as tx: tx.put(b"doc/1", b"\x0a\x01x")
        version = store.backup(base).snapshot_version
    def call(*args, status=0):
        result = subprocess.run([str(cli), *map(str, args)], capture_output=True, text=True, encoding="utf-8")
        assert result.returncode == (0 if status == 0 else 1), (args,result.stdout,result.stderr)
        response = json.loads(result.stdout if not status else result.stderr)
        assert response["status"] == status, response
        return response
    call("index-define",source,"name","doc/",1,4,3)
    call("backup-incremental",source,archive,version)
    call("recover-chain",base,recovered,archive)
    call("recover-chain",base,point,archive,"--target-version",version)
    with Store(recovered,library=library) as store:
        assert store.execute(Op.INDEX_LOOKUP,b"name",value=b"\x0a\x01x").rows[0][0]==b"doc/1"
    call("recover-chain",base,recovered,archive,status=15)
    call("index-drop",source,"name")
    call("compact",source)
    missing=root/"history-lost"
    call("backup-incremental",source,missing,version,status=14)
    assert not missing.exists()
    for path in (source,base,archive,recovered,point): cleanup(path)
    root.rmdir()
    print("PASS extended CLI index build/drop, incremental chain/PITR, history/destination protection")

if __name__ == "__main__": main()
