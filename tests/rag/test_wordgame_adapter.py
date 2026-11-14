"""Read-only host integration: real Pydantic/routes; optional existing KaLM smoke."""
import argparse
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

parser = argparse.ArgumentParser()
parser.add_argument("--core", required=True)
parser.add_argument("--index", required=True)
parser.add_argument("--shared-python", required=True)
parser.add_argument("--wordgame", required=True)
parser.add_argument("--real-model", action="store_true")
parser.add_argument("--output", required=True)
args, remaining = parser.parse_known_args()
ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
for directory in (args.shared_python, ROOT / "bindings/python", ROOT / "integrations", Path(args.wordgame) / "backend"):
    sys.path.insert(0, str(directory))

# Redirect before importing logging/provider; never open the game's business DB.
from app.core import config
config.LOG_DIR = ROOT / "build/rag-wordgame-logs"
from app.core import logging as game_logging
game_logging.LOG_DIR = config.LOG_DIR
from app.common.exceptions import AppError, register_exception_handlers
from app.models.rag import MemoryWrite, MemorySearch, MemoryList, MemoryDelete
from app.services.ai import provider
from app.services.ai.embedding_provider import DIMENSION, IDENTITY, QUERY_INSTRUCTION
from app.api.v1 import rag as api
from app.api.deps import require_account
from fastapi import FastAPI
from fastapi.testclient import TestClient
from wordgame_rag import create_store
import hashlib

report = {"real_model_requested": args.real_model, "business_data_accessed": False}


def make_store(root, callback, **options):
    return create_store(root, embed_with_provider=callback, identity=IDENTITY, dimension=DIMENSION,
        pooling="mean", query_transform="sha256:" + hashlib.sha256(QUERY_INSTRUCTION.encode()).hexdigest(),
        core_library=args.core, index_library=args.index, error_factory=AppError, **options)


class WordGameContractTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.calls = []
        def embed(texts, query=False):
            self.calls.append(query)
            return [[1.0] + [0.0] * (DIMENSION - 1) for _ in texts]
        self.store = make_store(self.folder.name, embed,
            status=lambda: {"state": "external-test"}, warmup=lambda: {"state": "ready"})
        self.addCleanup(self.store.close)

    def test_actual_models_aliases_revisions_and_paging(self):
        old = self.store.put("alice", [MemoryWrite(text="东村", npc_id="npc:1", fact_key="address", known_at=1)])[0]
        new = self.store.put("alice", [MemoryWrite(text="西村", owner_id="npc:1", fact_key="address", known_at=5)])[0]
        rows = self.store.search("alice", MemorySearch(query="住处", npc_id="npc:1", as_of=4, include_public=False))
        self.assertEqual([r["id"] for r in rows], [old["id"]])
        history = self.store.history("alice", new["id"], limit=1)
        self.assertEqual(len(history["items"]), 1)
        self.assertIsNotNone(history["next_revision"])
        self.assertEqual(len(self.store.list("alice", MemoryList(owner_id="npc:1"))["items"]), 2)
        self.assertEqual(self.store.delete_matching("alice", MemoryDelete(scope="world", npc_id="npc:1"))["deleted"], 2)
        self.assertTrue(any(self.calls))

    def test_actual_http_routes_use_facade_and_account_dependency(self):
        app = FastAPI()
        app.include_router(api.router)
        app.dependency_overrides[require_account] = lambda: {"account_id": "synthetic-http"}
        register_exception_handlers(app)
        with patch.object(api, "store", self.store), TestClient(app) as client:
            response = client.post("/rag/memories", json={"text": "synthetic event", "npc_id": "npc:1"})
            self.assertEqual(response.status_code, 200)
            data = response.json()["data"]
            rid = data["id"]
            self.assertEqual(data["owner_id"], "npc:1")
            result = client.post("/rag/search", json={"query": "event", "owner_id": "npc:1", "include_public": False}).json()
            self.assertEqual(result["data"][0]["id"], rid)
            self.assertEqual(client.get("/rag/memories", params={"owner_id": "npc:1"}).json()["data"]["items"][0]["id"], rid)
            self.assertEqual(client.get(f"/rag/memories/{rid}/history").status_code, 200)
            self.assertEqual(client.put(f"/rag/memories/{rid}", json={"text": "corrected", "owner_id": "npc:1", "expected_revision": 1}).status_code, 200)
            self.assertEqual(client.delete(f"/rag/memories/{rid}").status_code, 200)
            self.assertNotEqual(client.get(f"/rag/memories/{rid}").json().get("code"), 0)

    def test_status_and_warmup_do_not_own_provider(self):
        self.assertEqual(self.store.embedding_status()["runtime"]["state"], "external-test")
        self.assertEqual(self.store.warmup()["state"], "ready")

    @unittest.skipUnless(args.real_model, "existing KaLM smoke explicitly selected")
    def test_existing_kalm_inference_dense_lexical_and_hybrid(self):
        start = time.perf_counter()
        with tempfile.TemporaryDirectory() as root:
            store = make_store(root, provider.embed_with_provider,
                status=lambda: provider.embedding_backend().status(), warmup=lambda: provider.embedding_backend().ensure_ready())
            try:
                rows = store.put("synthetic-model", [MemoryWrite(text="青石村的铁匠叫李山，铁匠铺在村子东边。", source_id="synthetic:smith"),
                    MemoryWrite(text="观星台位于北山顶，夜间可观察星空。", source_id="synthetic:stars")])
                result = store.search("synthetic-model", MemorySearch(query="铁匠铺在哪里？", top_k=2))
                self.assertEqual(result[0]["id"], rows[0]["id"])
                self.assertTrue(-1 <= result[0]["similarity"] <= 1)
                for mode in ("lexical", "hybrid"):
                    found = store.search("synthetic-model", {"query": "铁匠", "mode": mode})
                    self.assertIn(rows[0]["id"], [r["id"] for r in found])
                status = store.embedding_status()["runtime"]
                report["real_model"] = {"identity": IDENTITY, "dimension": DIMENSION, "device": status["device"],
                    "state": status["state"], "top_similarity": result[0]["similarity"],
                    "elapsed_seconds_including_startup": time.perf_counter() - start}
            finally:
                store.close()
                provider.close_embedding_provider()


if __name__ == "__main__":
    run = unittest.main(argv=[sys.argv[0]] + remaining, exit=False)
    report.update(tests=run.result.testsRun, failures=len(run.result.failures), errors=len(run.result.errors), skipped=len(run.result.skipped))
    Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    sys.exit(not run.result.wasSuccessful())
