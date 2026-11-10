import json
from pathlib import Path
import subprocess
import sys
import uuid
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parent))
from store_bindings_smoke import Store, cleanup

def main():
    cli, library = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    unsupported = len(sys.argv) > 3 and sys.argv[3] == "--expect-unsupported-backup"
    parent = Path(__file__).resolve().parents[1] / "build" if unsupported else Path(tempfile.gettempdir())
    root = parent / ("store-cli-" + uuid.uuid4().hex)
    root.mkdir()
    source, backup, restored = (root / name for name in ("源库", "备份", "恢复"))
    with Store(source, library=library) as store:
        for i in range(20):
            with store.transaction() as tx: tx.put(b"key", bytes([i]) * 10000)
        with store.transaction() as tx: tx.put(b"expired", b"expired", 1)
    def call(*args, code=0):
        result = subprocess.run([str(cli), *map(str, args)], capture_output=True, text=True, encoding="utf-8")
        assert result.returncode == code, (args, result.stdout, result.stderr)
        return json.loads(result.stdout if not code else result.stderr)
    assert call("purge-expired", source, "--max-keys", 1)["removed_keys"] == 1
    assert call("verify", source)["stored_keys"] == 1
    if unsupported:
        assert call("backup", source, backup, code=1)["status"] == 12 and not backup.exists()
        with Store(source, library=library) as store: assert store.get(b"key") == bytes([19]) * 10000
        cleanup(source)
        root.rmdir()
        print("PASS unsupported directory-publication filesystem refused backup without changing source")
        return
    assert call("backup", source, backup)["stored_keys"] == 1
    info = call("compact", source)
    assert info["output_file_bytes"] < info["source_file_bytes"]
    assert call("restore", backup, restored)["stored_keys"] == 1
    assert call("verify", restored)["stored_keys"] == 1
    assert call("backup", source, backup, code=1)["status"] == 15
    missing = root / "missing"
    assert call("verify", missing, code=1)["status"] == 1 and not missing.exists()
    for path in (source, backup, restored):
        with Store(path, library=library) as store: assert store.get(b"key") == bytes([19]) * 10000
        cleanup(path)
    root.rmdir()
    print("PASS maintenance CLI verify/compact/backup/restore, Chinese paths and destination protection")

if __name__ == "__main__": main()
