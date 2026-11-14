"""Exercise the copied SDK from an empty cwd, with no source-tree imports."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


CODE = r'''
from pathlib import Path
import json
import sys
import FengquDB as fq
from FengquDB.bindings.python.fengqudb import native_library_path
from FengquDB.integrations.wordgame_rag import create_store

root = Path.cwd()
package = Path(fq.__file__).resolve().parent
assert "site-packages" not in str(package)
for stem in ("fengqudb", "fengqudb_index"):
    assert Path(native_library_path(stem)).is_relative_to(package)
with fq.Store(root / "main") as db:
    db.put(b"score", (99).to_bytes(8, "little", signed=True), force_reliable=True)
    assert int.from_bytes(db.get(b"score"), "little", signed=True) == 99
    db.configure_write(mode=fq.StoreWriteMode.RELIABLE)
    assert db.write_options().mode == fq.StoreWriteMode.RELIABLE
    db.reset_write_options()
    index = fq.NativeIndex()
    with db.transaction() as tx:
        tx.put(b"original/manual", "数据库可靠保存".encode())
        index.mutate(tx, index_space=b"knowledge", partition=b"public", operation=fq.Mutation.UPSERT,
            document=fq.Document(id=b"manual", projection_version=1, lifecycle_generation=1,
                                 texts={"body": "数据库可靠保存"}, metadata={"kind": "manual"}))
        tx.commit(force_reliable=True)
    with db.snapshot() as view:
        result = index.execute(view, index_space=b"knowledge", partition=b"public",
            text="可靠", where=fq.Predicate(1, field="kind", values=("manual",)))
        assert result["hits"][0]["document"]["id"] == b"manual"
    for _ in range(32):
        with db.transaction() as tx:
            state = index.maintain(tx, index_space=b"knowledge", partition=b"public",
                                  operation=fq.Maintenance.COMPACT_POSTINGS)
            tx.commit(force_reliable=True)
        if state["finished"]:
            break
    assert state["finished"]
    db.delete(b"score", force_reliable=True)

embedding = fq.CallbackEmbedding(lambda texts, query=False: [[1., 0., 0.] for _ in texts],
                                fq.EmbeddingContract("sdk-smoke", 3))
with fq.MemoryStore(root / "rag", embedding=embedding) as memory:
    row = memory.put("account-1", [fq.MemoryWrite(text="李山住在东村。", scope="world-1",
        owner_id="npc-1", known_at=8, source_id="event-42")])[0]
    request = fq.MemorySearch(query="李山住哪里？", scope="world-1", owner_id="npc-1",
                              include_public=False, as_of=10, mode="hybrid")
    assert memory.search("account-1", request)[0]["id"] == row["id"]
    assert memory.context("account-1", request, max_chars=2000)
    memory.delete("account-1", row["id"])
    assert memory.search("account-1", request) == []

with fq.Wiki(root / "wiki") as wiki:
    source = wiki.ingest("manual:storage", "数据库支持事务与可靠保存。", title="存储说明")
    wiki.run_jobs()
    assert wiki.search("可靠保存")["results"]
    assert wiki.ask("怎样可靠保存？")
    assert wiki.lint()["ok"]
    assert wiki.delete_source(source["id"])

assert not any(name in sys.modules for name in ("fengqudb", "fengqudb_index", "llmwiki", "fengqudb_rag"))
print(json.dumps({"sdk": str(package), "core": native_library_path(),
                  "index": native_library_path("fengqudb_index"),
                  "checks": ["core", "index", "maintenance", "rag", "wiki", "relative-imports"]}))
'''


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True, help="Directory containing FengquDB/")
    args = parser.parse_args()
    environment = dict(os.environ)
    environment.pop("FENGQUDB_LIBRARY", None)
    environment.pop("FENGQUDB_INDEX_LIBRARY", None)
    environment["PYTHONPATH"] = str(args.sdk.resolve())
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    with tempfile.TemporaryDirectory(prefix="fq-sdk-") as directory:
        for name in ("fengqudb", "fengqudb_index", "llmwiki", "fengqudb_rag"):
            (Path(directory) / (name + ".py")).write_text('raise RuntimeError("unexpected top-level import")\n')
        subprocess.run([sys.executable, "-c", CODE], cwd=directory, env=environment, check=True)
