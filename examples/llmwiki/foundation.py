"""Run with the paired FENGQUDB_LIBRARY / FENGQUDB_INDEX_LIBRARY environment variables."""

import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from llmwiki import NativeIndexProvider, Wiki


def run(path):
    with Wiki(path, namespace="foundation-example-v2", index_provider=NativeIndexProvider()) as wiki:
        def source(uri, seconds):
            return {"uri": uri, "title": uri, "content": "服务超时是%d秒。" % seconds,
                "annotations": {"entities": [{"key": "service", "type": "system", "name": "Demo service"}],
                    "claims": [{"entity": "service", "predicate": "timeout_seconds", "value": seconds,
                                "quote": str(seconds) + "秒"}]}}
        uploaded = wiki.ingest_many([source("docs:spec", 30), source("docs:old-note", 60)], request_key="initial-import")
        wiki.define_page("operations", "运维说明", [s["id"] for s in uploaded["items"]])
        jobs = wiki.run_jobs(limit=20, workers=2)
        entity = wiki.entity(wiki.listing("entity")["items"][0]["id"])
        conflict = entity["conflicts"][0]
        selected = next(c for c in entity["claims"] if c["value"] == 30)
        wiki.resolve_conflict(conflict["id"], [selected["id"]], expected_generation=conflict["generation"],
                              note="以当前规格为准；旧笔记保留作为竞争证据。", actor="example-reviewer")
        wiki.run_jobs(limit=20, workers=2)
        page = wiki.page("operations")
        edited = wiki.revise_page("operations", "# 运维说明\n\n规格和历史笔记需要共同审阅。[S1] [S2]",
            expected_revision=page["revision"], expected_version=page["version"], actor="example-editor")
        return {"jobs": jobs, "entity": wiki.entity(entity["id"]), "human_revision": edited["revision"],
                "answer": wiki.ask("服务 超时"), "status": wiki.status()}


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="fengquwiki-example-") as directory:
        print(json.dumps(run(Path(directory) / "db"), ensure_ascii=False, indent=2))
