"""Complete memory governance against the real shared Core/Index; synthetic data."""
import argparse
from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest

parser = argparse.ArgumentParser()
parser.add_argument("--core", required=True)
parser.add_argument("--index", required=True)
parser.add_argument("--shared-python", required=True)
parser.add_argument("--crash-root")
parser.add_argument("--crash-kind", choices=("purge", "write"))
args, remaining = parser.parse_known_args()
REPO = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(args.shared_python).resolve()))
sys.path.insert(0, str(REPO / "bindings/python"))
sys.path.insert(0, str(REPO / "integrations"))

from fengqudb import Store, Error
import fengqudb_index as ffi
from fengqudb_rag import (MemoryStore, MemoryWrite, MemoryImport, MemorySearch, MemoryList, MemoryDelete,
    EmbeddingContract, CallbackEmbedding, QueryBudget, RagConflict, RagNotFound, RagIntegrityError,
    RagStaleRead, RagCancelled, RagDeadline, RagLimit)
from fengqudb_rag.repository import Repository, ROOT, load, rkey, nkey, save, take, digest, public
from wordgame_rag import create_store

CORE, INDEX = str(Path(args.core).resolve()), str(Path(args.index).resolve())


class FakeEmbedding:
    def __init__(self):
        self.calls, self.hook, self.failure = [], None, None

    def __call__(self, texts, *, query=False):
        self.calls.append((list(texts), query))
        if self.hook:
            callback, self.hook = self.hook, None
            callback()
        if self.failure:
            raise self.failure
        return [[0.0, 1.0, 0.0] if text.startswith("orthogonal") else [1.0, 0.0, 0.0] for text in texts]


class Client(ffi.NativeIndex):
    def __init__(self):
        super().__init__(INDEX)
        self.fail_after = None
        self.calls = 0
        self.after_query = None
        self.transform = None

    def mutate(self, tx, **mutation):
        super().mutate(tx, **mutation)
        self.calls += 1
        if self.fail_after is not None:
            self.fail_after -= 1
            if self.fail_after == 0:
                self.fail_after = None
                document = mutation.get("document")
                rid = mutation.get("id") or document.id
                generation = mutation.get("lifecycle_generation") or document.lifecycle_generation
                super().mutate(tx, operation=ffi.Mutation.ERASE, index_space=mutation["index_space"],
                    partition=mutation["partition"], id=rid, expected_projection_version=999999,
                    next_projection_version=1000000, lifecycle_generation=generation)

    def execute(self, view, **query):
        result = super().execute(view, **query)
        if self.after_query:
            callback, self.after_query = self.after_query, None
            callback()
        return self.transform(result) if self.transform else result


def engine(root, fake=None, client=None, **options):
    fake = fake or FakeEmbedding()
    provider = CallbackEmbedding(fake, EmbeddingContract("synthetic-v1", 3))
    return MemoryStore(root, embedding=provider, core_library=CORE, index=client or Client(), **options)


def crash_child():
    store = engine(args.crash_root)
    if args.crash_kind == "purge":
        original = Repository.purge_step
        def killed(self, *a, **kw):
            original(self, *a, **kw)
            os._exit(86)
        Repository.purge_step = killed
        store.delete_matching("alice", MemoryDelete(scope="world", owner_id="one"))
    else:
        original = store.client.mutate
        def killed(*a, **kw):
            original(*a, **kw)
            os._exit(86)
        store.client.mutate = killed
        store.put("alice", [MemoryWrite(text="uncommitted", fact_key="address", known_at=5)])
    raise RuntimeError("crash point was not reached")


class MemoryTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.fake, self.client = FakeEmbedding(), Client()
        self.memory = engine(self.folder.name, self.fake, self.client)
        self.addCleanup(lambda: self.memory.close())

    def put(self, text="住在东村", account="alice", **values):
        return self.memory.put(account, [MemoryWrite(text=text, **values)])[0]

    def search(self, account="alice", **values):
        return self.memory.search(account, MemorySearch(query=values.pop("query", "住在哪里"),
            as_of=values.pop("as_of", 10), **values))

    def reopen(self, **options):
        self.memory.close()
        self.memory = engine(self.folder.name, self.fake, self.client, **options)

    def test_exact_dedupe_sources_owners_and_restart_skip_embedding(self):
        first = self.put(source_id="chat:1", owner_id="one")
        self.fake.calls.clear()
        same = self.put(source_id="chat:1", npc_id="one")
        self.assertEqual(first["id"], same["id"])
        self.assertEqual(self.fake.calls, [])
        self.assertNotEqual(first["id"], self.put(source_id="chat:2", owner_id="one")["id"])
        self.assertNotEqual(first["id"], self.put(source_id="chat:1", owner_id="two")["id"])
        self.reopen()
        self.fake.calls.clear()
        self.assertEqual(self.put(source_id="chat:1", owner_id="one")["id"], first["id"])
        self.assertEqual(self.fake.calls, [])

    def test_import_chunking_and_durable_dedupe(self):
        item = MemoryImport(text="可追溯的原始资料。" * 400, owner_id="one")
        first = self.memory.import_text("alice", item)
        self.assertGreater(len(first), 1)
        self.assertTrue(all(0 < len(r["text"]) <= 1200 for r in first))
        self.assertTrue(all(r["source_id"].startswith("import:") for r in first))
        self.fake.calls.clear()
        self.assertEqual([r["id"] for r in first], [r["id"] for r in self.memory.import_text("alice", item)])
        self.assertEqual(self.fake.calls, [])
        with self.assertRaises(ValueError):
            self.memory.import_text("alice", MemoryImport(text="document", fact_key="one"))

    def test_fact_timeline_half_open_and_owners(self):
        old = self.put(fact_key="address", owner_id="one", source_id="first", known_at=1)
        new = self.put("搬到西村", fact_key="address", owner_id="one", source_id="second", known_at=5)
        self.assertEqual([r["text"] for r in self.search(owner_id="one", as_of=4)], ["住在东村"])
        self.assertEqual([r["text"] for r in self.search(owner_id="one", as_of=5)], ["搬到西村"])
        self.assertEqual(self.memory.get("alice", old["id"])["revision"], old["revision"])
        self.assertEqual(self.memory.get("alice", old["id"])["projection_version"], 2)
        history = self.memory.history("alice", new["id"])["items"]
        self.assertEqual([r["revision"] for r in history], [1, 2])
        self.assertEqual(history[0]["expires_at"], 5)
        self.assertEqual(new["supersedes"], old["id"])
        self.put(fact_key="address", owner_id="two")
        self.assertEqual(len(self.search(owner_id="two")), 1)

    def test_fact_expiry_never_extends_and_empty_gap(self):
        old = self.put(fact_key="address", known_at=1, expires_at=3)
        new = self.put("new", fact_key="address", known_at=5)
        self.assertEqual(self.memory.get("alice", old["id"])["expires_at"], 3)
        self.assertEqual(self.search(as_of=3), [])
        self.assertEqual(self.search(as_of=4), [])
        self.assertEqual([r["id"] for r in self.search(as_of=5)], [new["id"]])

    def test_batch_fact_versions_atomic_and_retry_does_not_reopen(self):
        items = [MemoryWrite(text="a", fact_key="f", known_at=1), MemoryWrite(text="b", fact_key="f", known_at=5)]
        first = self.memory.put("alice", items)
        self.assertEqual(first[0]["expires_at"], 5)
        self.assertEqual([r["id"] for r in first], [r["id"] for r in self.memory.put("alice", items)])
        with self.assertRaises(RagConflict):
            self.memory.put("alice", [MemoryWrite(text="c", fact_key="f", known_at=8), MemoryWrite(text="d", fact_key="f", known_at=7)])
        self.assertEqual([r["text"] for r in self.search()], ["b"])
        self.assertEqual(self.memory.stats("alice")["records"], 2)

    def test_correction_cas_source_identity_and_audit_pages(self):
        row = self.put(source_id="file:1", source_uri="original.txt")
        new = self.memory.put("alice", [MemoryWrite(text="corrected", source_id="file:1", expected_revision=1)], record_id=row["id"])[0]
        self.assertEqual(new["id"], row["id"])
        self.assertEqual(new["revision"], 2)
        first = self.memory.history("alice", row["id"], limit=1)
        self.assertEqual(first["items"][0]["source_uri"], "original.txt")
        self.assertEqual(first["next_revision"], 1)
        second = self.memory.history("alice", row["id"], after_revision=1)
        self.assertEqual(second["items"][0]["text"], "corrected")
        with self.assertRaises(RagConflict):
            self.memory.put("alice", [MemoryWrite(text="stale", source_id="file:1", expected_revision=1)], record_id=row["id"])
        for values in ({"source_id": "other"}, {"owner_id": "other", "source_id": "file:1"}):
            with self.assertRaises(RagConflict):
                self.memory.put("alice", [MemoryWrite(text="move", **values)], record_id=row["id"])

    def test_title_only_edit_reuses_embedding_and_revision_allocations(self):
        row = self.put(fact_key="f", known_at=1)
        self.fake.calls.clear()
        edited = self.memory.put("alice", [MemoryWrite(text="住在东村", title="new title", fact_key="f", known_at=1)], record_id=row["id"])[0]
        self.assertEqual(self.fake.calls, [])
        new = self.put("new fact", fact_key="f", known_at=5)
        self.assertEqual((edited["revision"], new["revision"]), (2, 3))
        self.assertEqual([r["revision"] for r in self.memory.history("alice", new["id"])["items"]], [1, 2, 3])

    def test_fact_edit_cannot_change_period_or_edit_closed_head(self):
        old = self.put(fact_key="f", known_at=1)
        with self.assertRaises(RagConflict):
            self.memory.put("alice", [MemoryWrite(text="changed", fact_key="f", known_at=2)], record_id=old["id"])
        self.put("new", fact_key="f", known_at=5)
        with self.assertRaises(RagConflict):
            self.memory.put("alice", [MemoryWrite(text="changed", fact_key="f", known_at=1, expires_at=5)], record_id=old["id"])

    def test_edit_closed_during_embedding_is_rejected(self):
        old = self.put(fact_key="f", known_at=1, expires_at=3)
        self.fake.hook = lambda: self.put("new version", fact_key="f", known_at=5)
        with self.assertRaises(RagConflict):
            self.memory.put("alice", [MemoryWrite(text="edited pending", fact_key="f", known_at=1, expires_at=3)], record_id=old["id"])
        self.assertEqual(self.memory.get("alice", old["id"])["text"], "住在东村")

    def test_delete_complete_lineage_and_no_old_fact_reappears(self):
        old = self.put(fact_key="f", known_at=1)
        new = self.put("new", fact_key="f", known_at=5)
        self.memory.delete("alice", new["id"])
        for row in (old, new):
            with self.assertRaises(RagNotFound):
                self.memory.get("alice", row["id"])
        self.assertEqual(self.search(as_of=2), [])
        self.assertEqual(self.memory.stats("alice")["records"], 0)

    def test_source_cleanup_is_exact_and_can_continue_missing_head(self):
        old = self.put(fact_key="f", known_at=1, source_id="keep")
        self.put("new", fact_key="f", known_at=5, source_id="remove")
        self.put(owner_id="other", source_id="remove")
        result = self.memory.delete_matching("alice", MemoryDelete(scope="world", source_id="remove"))
        self.assertEqual(result["deleted"], 1)
        self.assertEqual(self.memory.get("alice", old["id"])["expires_at"], 5)
        new = self.put("third", fact_key="f", known_at=6, source_id="third")
        self.assertEqual(new["lineage_id"], old["id"])
        self.assertEqual(new["revision"], 3)
        self.assertEqual(len(self.memory.history("alice", new["id"])["items"]), 2)
        self.assertEqual(len(self.search(owner_id="other", include_public=False)), 1)

    def test_delete_or_cleanup_during_embedding_prevents_resurrection(self):
        row = self.put(owner_id="one")
        self.fake.hook = lambda: self.memory.delete("alice", row["id"])
        with self.assertRaises(RagConflict):
            self.memory.put("alice", [MemoryWrite(text="edited after deletion", owner_id="one")], record_id=row["id"])
        self.fake.hook = lambda: self.memory.delete_matching("alice", MemoryDelete(scope="world", owner_id="one"))
        with self.assertRaises(RagConflict):
            self.put("import after clear", owner_id="one")
        self.assertEqual(self.memory.stats("alice")["records"], 0)

    def test_native_failure_rolls_back_fact_transition_and_batch(self):
        old = self.put(fact_key="f", known_at=1)
        self.client.fail_after = 1
        with self.assertRaises(RagConflict):
            self.put("new", fact_key="f", known_at=5)
        self.assertIsNone(self.memory.get("alice", old["id"])["expires_at"])
        self.client.fail_after = 2
        with self.assertRaises(RagConflict):
            self.memory.put("alice", [MemoryWrite(text="x"), MemoryWrite(text="y")])
        self.assertEqual(self.memory.stats("alice")["records"], 1)

    def test_ownership_account_scope_timeline_public_and_time(self):
        shared = self.put(known_at=10, expires_at=20)
        own = self.put(owner_id="one", known_at=10)
        self.put(owner_id="two", known_at=10)
        self.put(scope="another", known_at=10)
        self.put(timeline="fork", known_at=10)
        self.put(account="bob")
        self.assertEqual(self.search(owner_id="one", as_of=9), [])
        self.assertEqual({r["id"] for r in self.search(owner_id="one", as_of=10)}, {shared["id"], own["id"]})
        self.assertEqual([r["id"] for r in self.search(owner_id="one", include_public=False, as_of=10)], [own["id"]])
        self.assertEqual([r["id"] for r in self.search(owner_id="one", as_of=20)], [own["id"]])
        for method in (self.memory.get, self.memory.delete, self.memory.history):
            with self.assertRaises(RagNotFound):
                method("bob", own["id"])

    def test_filter_injection_and_source_filter_cannot_expand(self):
        self.put(source_id="doc")
        self.put(owner_id="secret", source_id="doc")
        value = "' OR owner_id != '' OR known_at >= 0"
        self.assertEqual(self.search(scope=value), [])
        self.assertEqual(self.search(owner_id=value, include_public=False), [])
        self.assertEqual(self.search(source_id=value), [])

    def test_management_list_stable_keyset_and_history_visibility(self):
        rows = [self.put(str(i), source_id="doc", known_at=i, expires_at=i + 2) for i in range(7)]
        ids, after = [], ""
        while True:
            page = self.memory.list("alice", MemoryList(source_id="doc", after=after, limit=2))
            ids += [r["id"] for r in page["items"]]
            after = page["next_after"]
            if not after:
                break
        self.assertEqual(ids, sorted(r["id"] for r in rows))
        self.assertEqual(len(self.memory.list("alice", MemoryList(source_id="doc", as_of=3))["items"]), 2)

    def test_same_account_thousand_owners_share_one_store(self):
        self.memory.put("alice", [MemoryWrite(text=f"owner {i}", owner_id=str(i)) for i in range(1000)], budget=QueryBudget(timeout_ms=30000))
        self.assertEqual(len(self.memory._accounts), 1)
        self.assertEqual(self.memory.stats("alice")["records"], 1000)
        self.assertEqual(self.memory.delete_matching("alice", MemoryDelete(scope="world", owner_id="42"))["deleted"], 1)
        self.assertEqual(self.memory.stats("alice")["records"], 999)

    def test_bulk_cleanup_failure_keeps_barrier_and_recovers_after_reopen(self):
        self.memory.put("alice", [MemoryWrite(text=f"bulk {i}", owner_id="one") for i in range(270)])
        kept = self.put(owner_id="two")
        self.client.fail_after = 130
        with self.assertRaises(RagConflict):
            self.memory.delete_matching("alice", MemoryDelete(scope="world", owner_id="one"))
        self.assertEqual(self.memory.stats("alice")["pending_cleanup"], 1)
        self.assertEqual(self.search(owner_id="one", include_public=False), [])
        self.assertEqual(self.memory.get("alice", kept["id"])["owner_id"], "two")
        self.reopen()
        self.assertEqual(self.memory.stats("alice")["records"], 1)
        self.assertEqual(self.memory.stats("alice")["pending_cleanup"], 0)

    def test_large_edit_history_is_deleted_in_bounded_steps(self):
        row = self.put(source_id="doc")
        for i in range(135):
            row = self.memory.put("alice", [MemoryWrite(text="住在东村", title=str(i), source_id="doc")], record_id=row["id"])[0]
        self.assertEqual(row["revision"], 136)
        self.memory.delete("alice", row["id"])
        self.assertEqual(self.memory.stats("alice")["records"], 0)
        with self.memory._account("alice") as account, account.store.snapshot() as view:
            self.assertEqual(take(view, ROOT + b"A/", 1), [])
            self.assertEqual(take(view, ROOT + b"V/", 1), [])

    def test_history_does_not_leak_source_under_cleanup_barrier(self):
        first = self.put(fact_key="f", known_at=1, source_id="remove")
        first = self.memory.put("alice", [MemoryWrite(text="edited", fact_key="f", known_at=1, source_id="remove")], record_id=first["id"])[0]
        last = self.put("new", fact_key="f", known_at=5, source_id="keep")
        with self.memory._account("alice") as account, account.lock:
            account.repo.start_purge(dict(scope="world", owner_id="", timeline="main", source_id="remove"))
        history = self.memory.history("alice", last["id"])["items"]
        self.assertEqual([r["source_id"] for r in history], ["keep"])
        self.memory.recover("alice")

    def test_retirement_is_persistent_and_clear_allows_new_generation(self):
        first = self.put(owner_id="one")
        self.memory.delete_matching("alice", MemoryDelete(scope="world", owner_id="one"))
        new = self.put("new generation", owner_id="one")
        self.assertGreater(new["namespace_generation"], first["namespace_generation"])
        self.memory.retire_namespace("alice", MemoryDelete(scope="world", owner_id="one"))
        self.reopen()
        with self.assertRaises(RagConflict):
            self.put("revive", owner_id="one")
        self.assertEqual(self.search(owner_id="one"), [])

    def test_scoped_handle_get_history_write_and_selector_permissions(self):
        a = self.put(owner_id="one")
        b = self.put(owner_id="two")
        scoped = self.memory.scoped("alice", scope="world", owner_id="one")
        self.assertEqual(scoped.get(a["id"])["id"], a["id"])
        for method in (scoped.get, scoped.delete, scoped.history):
            with self.assertRaises(RagNotFound):
                method(b["id"])
        with self.assertRaises(RagNotFound):
            scoped.put([dict(text="leak", owner_id="two")])
        with self.assertRaises(RagConflict):
            scoped.search(dict(query="query"))
        self.assertEqual(len(scoped.search(dict(query="query", include_public=False))), 1)

    def test_final_version_and_lifecycle_recheck(self):
        row = self.put()
        self.client.after_query = lambda: self.memory.put("alice", [MemoryWrite(text="changed")], record_id=row["id"])
        with self.assertRaises(RagStaleRead):
            self.search()
        self.client.after_query = lambda: self.memory.delete("alice", row["id"])
        with self.assertRaises(RagStaleRead):
            self.search()

    def test_dense_threshold_and_native_result_evidence(self):
        self.put()
        self.assertAlmostEqual(self.search()[0]["similarity"], 1)
        self.assertEqual(self.search(query="orthogonal query", min_similarity=0.5), [])
        self.client.transform = lambda result: dict(result, match_evidence=0)
        with self.assertRaises(RagIntegrityError):
            self.search()
        self.client.transform = None

    def test_model_identity_missing_metadata_and_no_legacy_relabel(self):
        self.put()
        self.memory.close()
        callback = CallbackEmbedding(FakeEmbedding(), EmbeddingContract("different-same-dimension", 3))
        other = MemoryStore(self.folder.name, embedding=callback, core_library=CORE, index=self.client)
        with self.assertRaises(RagIntegrityError):
            other.stats("alice")
        other.close()
        path = Path(self.folder.name) / hashlib.sha256(b"alice").hexdigest() / "memory"
        with Store(path, library=CORE, create=False) as store:
            store.delete(ROOT + b"meta", force_reliable=True)
        self.reopen()
        with self.assertRaises(RagIntegrityError):
            self.memory.stats("alice")

    def test_vector_validation_normalization_and_provider_contract_change(self):
        for bad in ([True, 0, 0], [0, 0, 0], [float("nan"), 0, 0], [1, 0]):
            self.fake.failure = None
            callback = lambda texts, query=False, value=bad: [value for _ in texts]
            store = MemoryStore(Path(self.folder.name) / str(len(repr(bad))),
                embedding=CallbackEmbedding(callback, EmbeddingContract("bad", 3)), core_library=CORE, index=self.client)
            try:
                with self.assertRaises((RagIntegrityError, ValueError)):
                    store.put("alice", [MemoryWrite(text="bad")])
                self.assertEqual(store.stats("alice")["records"], 0)
            finally:
                store.close()
        self.memory.runtime.provider._contract = EmbeddingContract("changed", 3)
        with self.assertRaises(RagIntegrityError):
            self.memory.stats("alice")

    def test_account_cache_lru_and_close_wait(self):
        self.reopen(max_accounts=2)
        a = self.put(account="a")
        self.put(account="b")
        self.put(account="c")
        self.assertEqual(len(self.memory._accounts), 2)
        self.assertEqual(self.memory.get("a", a["id"])["text"], a["text"])
        entered, release, closed = threading.Event(), threading.Event(), threading.Event()
        def held():
            with self.memory._account("a"):
                entered.set()
                release.wait(3)
        thread = threading.Thread(target=held)
        thread.start()
        self.assertTrue(entered.wait(2))
        closer = threading.Thread(target=lambda: (self.memory.close(), closed.set()))
        closer.start()
        self.assertFalse(closed.wait(0.05))
        release.set()
        thread.join(3)
        closer.join(3)
        self.assertTrue(closed.is_set())

    def test_input_validation_and_pydantic_shaped_requests(self):
        class Model:
            def model_dump(self):
                return dict(text="model input", scope="world", owner_id="one")
        row = self.memory.put("alice", [Model()])[0]
        self.assertEqual(row["owner_id"], "one")
        with self.assertRaises(ValueError):
            MemoryDelete()
        with self.assertRaises(ValueError):
            MemorySearch(query="x", as_of=True)
        with self.assertRaises(ValueError):
            self.memory.put("alice", [dict(text="x", unknown=True)])
        with self.assertRaises(ValueError):
            self.memory.history("alice", row["id"], limit=0)
        for invalid_id in ("", "not-an-id"):
            with self.assertRaises(ValueError):
                self.memory.put("alice", [dict(text="must not insert")], record_id=invalid_id)

    def test_callback_embedding_large_finite_values_normalize(self):
        callback = lambda texts, query=False: [[1e308, 0, 0] for _ in texts]
        store = MemoryStore(Path(self.folder.name) / "big", embedding=CallbackEmbedding(callback,
            EmbeddingContract("big", 3)), core_library=CORE, index=self.client)
        try:
            store.put("a", [MemoryWrite(text="finite")])
            self.assertAlmostEqual(store.search("a", MemorySearch(query="finite"))[0]["similarity"], 1)
        finally:
            store.close()

    def test_embedding_failure_and_incomplete_batch_leave_no_records(self):
        self.fake.failure = RuntimeError("model unavailable")
        with self.assertRaises(RuntimeError):
            self.put()
        self.assertEqual(self.memory.stats("alice")["records"], 0)
        self.fake.failure = None
        self.memory.runtime.provider.callback = lambda texts, query=False: []
        with self.assertRaises(RagIntegrityError):
            self.put("another")

    def test_lexical_hybrid_strict_time_stats_and_public_union(self):
        one = self.put("needlealpha 可见旧闻", owner_id="one", known_at=1)
        shared = self.put("needlealpha 共享资料", known_at=1)
        self.put("needlealpha 未来资料", owner_id="one", known_at=20)
        self.put("needlealpha 私人资料", owner_id="two", known_at=1)
        for mode in ("lexical", "hybrid"):
            rows = self.search(owner_id="one", query="needlealpha", mode=mode, as_of=10)
            self.assertEqual({r["id"] for r in rows}, {one["id"], shared["id"]})
            self.assertTrue(all(r["score_kind"] == "rrf" for r in rows))
        before = self.search(owner_id="one", include_public=False, query="needlealpha", mode="lexical")
        self.put("needlealpha needlealpha 更多未来", owner_id="one", known_at=30)
        after = self.search(owner_id="one", include_public=False, query="needlealpha", mode="lexical")
        self.assertEqual([(r["id"], r["score"]) for r in before], [(r["id"], r["score"]) for r in after])

    def test_lexical_query_does_not_load_model(self):
        self.put("lexicalneedle")
        self.fake.failure = RuntimeError("must not load query model")
        rows = self.search(query="lexicalneedle", mode="lexical")
        self.assertEqual(len(rows), 1)
        self.assertIsNone(rows[0]["similarity"])

    def test_long_queries_preserve_text_visibility_and_explicit_work_budget(self):
        terms = ["t" + chr(97 + i // 26) + chr(97 + i % 26) for i in range(257)]
        text = " ".join(terms)
        visible = self.put(text, owner_id="one", known_at=1)
        self.put(text, owner_id="two", known_at=1)
        self.put(text, owner_id="one", known_at=20)
        observed = []
        original = self.client.execute

        def execute(view, **query):
            if query.get("mode") == 0:
                observed.append(query["text"])
            return original(view, **query)

        self.client.execute = execute
        queries = [" ".join(terms[:count]) for count in (33, 65, 128)]
        queries.append(" ".join([terms[0]] * 300))
        for query in queries:
            for mode in ("lexical", "hybrid"):
                with self.subTest(words=len(query.split()), mode=mode):
                    rows = self.search(query=query, mode=mode, owner_id="one", include_public=False, as_of=10)
                    self.assertEqual([row["id"] for row in rows], [visible["id"]])
                    self.assertEqual(observed[-1], query)
        for count in (129, 257):
            query = " ".join(terms[:count])
            for mode in ("lexical", "hybrid"):
                with self.subTest(words=count, mode=mode), self.assertRaises(RagLimit):
                    self.memory.search("alice", MemorySearch(query=query, mode=mode,
                        owner_id="one", include_public=False, as_of=10))
                self.assertEqual(observed[-1], query)
        with self.assertRaises(RagLimit):
            self.memory.search("alice", MemorySearch(query=queries[0], mode="lexical", owner_id="one",
                include_public=False, as_of=10), budget=QueryBudget(posting_budget=1))
        self.assertEqual(observed[-1], queries[0])

    def test_chinese_queries_preserve_owner_time_and_raw_text(self):
        content = "背景音乐。切换曲目时取消旧请求，避免覆盖新曲目。"
        visible = self.put(content, owner_id="one", known_at=1)
        self.put(content, owner_id="two", known_at=1)
        self.put(content, owner_id="one", known_at=20)
        queries = ("音乐连续切换时如何防止旧请求覆盖新曲目",
                   "音乐 连续切换时如何防止旧请求覆盖新曲目", " ".join(["音乐"] * 250))
        observed = []
        original = self.client.execute
        def execute(view, **query):
            if query.get("mode") == 0:
                observed.append(query["text"])
            return original(view, **query)
        self.client.execute = execute
        for query in queries:
            for mode in ("lexical", "hybrid"):
                with self.subTest(query=query[:30], mode=mode):
                    rows = self.search(query=query, mode=mode, owner_id="one", include_public=False, as_of=10)
                    self.assertEqual([visible["id"]], [row["id"] for row in rows])
                    self.assertEqual(query, observed[-1])

    def test_context_budget_citations_exclusions_and_final_validation(self):
        self.put("first evidence", source_id="recent")
        row = self.put("second evidence", source_id="keep")
        request = MemorySearch(query="evidence")
        result = self.memory.context("alice", request, max_chars=1000, exclude_sources=["recent"])
        self.assertEqual([r["id"] for r in result["context"]["citations"]], [row["id"]])
        self.assertLessEqual(result["context"]["characters"], 1000)
        zero = self.memory.context("alice", request, max_chars=0)
        self.assertEqual(zero["context"]["text"], "")
        with self.assertRaises(ValueError):
            self.memory.context("alice", request, max_tokens=5)
        def predicate(record):
            if record["id"] == row["id"]:
                self.memory.delete("alice", row["id"])
            return True
        with self.assertRaises(RagStaleRead):
            self.memory.context("alice", request, predicate=predicate)

    def test_reranker_is_bounded_to_authorized_candidates(self):
        a = self.put("a")
        b = self.put("b")
        self.put("private", owner_id="secret")
        result = self.memory.search("alice", MemorySearch(query="q"), reranker=lambda q, rows: [(b["id"], 9), (a["id"], 1)])
        self.assertEqual(result[0]["id"], b["id"])
        self.assertEqual(result[0]["score_kind"], "reranker")
        with self.assertRaises(RagIntegrityError):
            self.memory.search("alice", MemorySearch(query="q"), reranker=lambda q, rows: ["f" * 32])

    def test_query_limits_and_cancellation_never_become_empty_success(self):
        self.memory.put("alice", [MemoryWrite(text=str(i)) for i in range(4)])
        with self.assertRaises(RagLimit):
            self.memory.search("alice", MemorySearch(query="q"), budget=QueryBudget(candidate_budget=1))
        with self.assertRaises(RagCancelled):
            self.memory.search("alice", MemorySearch(query="q"), cancelled=lambda: True)
        with self.assertRaises(RagLimit):
            self.memory.search("alice", MemorySearch(query="q"), budget=QueryBudget(result_bytes=1))
        from fengqudb_rag.repository import encode
        base = self.memory.retrieve("alice", MemorySearch(query="q"))
        with self.assertRaises(RagLimit):
            self.memory.context("alice", MemorySearch(query="q"),
                budget=QueryBudget(result_bytes=len(encode(base)) + 20))

    def test_wordgame_facade_translates_errors_and_keeps_model_external(self):
        class AppError(RuntimeError):
            pass
        facade = create_store(Path(self.folder.name) / "facade", embed_with_provider=self.fake,
            identity="KaLM-synthetic-adapter", dimension=3, core_library=CORE, index=self.client, error_factory=AppError)
        try:
            facade.put("alice", [dict(text="event", owner_id="gm", scope="game:uuid", source_id="event:id")])
            self.assertEqual(facade.stats("alice")["storage"], "fengqudb")
            with self.assertRaises(AppError):
                facade.get("bob", "f" * 32)
        finally:
            facade.close()

    def _child(self, kind):
        command = [sys.executable, __file__, "--core", CORE, "--index", INDEX,
            "--shared-python", str(Path(args.shared_python).resolve()), "--crash-root", self.folder.name, "--crash-kind", kind]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 86, result.stdout + result.stderr)

    def test_process_exit_before_commit_preserves_previous_fact(self):
        old = self.put(fact_key="address", known_at=1)
        self.memory.close()
        self._child("write")
        self.reopen()
        self.assertIsNone(self.memory.get("alice", old["id"])["expires_at"])
        self.assertEqual(self.memory.stats("alice")["records"], 1)

    def test_process_exit_after_cleanup_batch_recovers_remaining_jobs(self):
        self.memory.put("alice", [MemoryWrite(text=f"crash {i}", owner_id="one") for i in range(260)])
        kept = self.put(owner_id="two")
        self.memory.close()
        self._child("purge")
        self.reopen()
        self.assertEqual(self.memory.stats("alice")["records"], 1)
        self.assertEqual(self.memory.stats("alice")["pending_cleanup"], 0)
        self.assertEqual(self.memory.get("alice", kept["id"])["id"], kept["id"])


if __name__ == "__main__":
    if args.crash_root:
        crash_child()
    else:
        unittest.main(argv=[sys.argv[0]] + remaining)
