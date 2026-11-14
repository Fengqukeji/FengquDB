"""Real shared Index/Core integration for RAG-owned source/chunk governance."""
import argparse
import math
from pathlib import Path
import sys
import tempfile
import threading
import unittest

parser = argparse.ArgumentParser()
parser.add_argument("--core", required=True)
parser.add_argument("--index", required=True)
parser.add_argument("--shared-python", required=True)
args, remaining = parser.parse_known_args()
ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True  # Shared wrappers belong to the Wiki worktree.
sys.path.insert(0, str(Path(args.shared_python).resolve()))
sys.path.insert(0, str(ROOT / "bindings/python"))

from fengqudb import Store, Error
import fengqudb_index as ffi
from fengqudb_rag import (Chunk, QueryBudget, NativeIndexProvider, NativeRagStore, RagConflict,
    RagNotFound, RagUnsupported, RagIntegrityError, RagStaleRead, RagCancelled, RagLimit, RagDeadline)

CORE, INDEX = str(Path(args.core).resolve()), str(Path(args.index).resolve())
SPACE = b"synthetic-unit-vectors-3d-v1"


class InstrumentedClient(ffi.NativeIndex):
    """Wrap real calls; change responses or inject a real native CAS failure."""
    def __init__(self):
        super().__init__(INDEX)
        self.transform = None
        self.fail_stage = False
        self.calls = []

    def execute(self, handle, **query):
        self.calls.append(query)
        result = super().execute(handle, **query)
        return self.transform(result) if self.transform is not None else result

    def mutate(self, transaction, **mutation):
        super().mutate(transaction, **mutation)
        if self.fail_stage:
            self.fail_stage = False
            doc = mutation["document"]
            super().mutate(transaction, index_space=mutation["index_space"], partition=mutation["partition"],
                operation=ffi.Mutation.ERASE, id=doc.id, expected_projection_version=999,
                next_projection_version=1000, lifecycle_generation=doc.lifecycle_generation)


class NativeSourcesTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "data"
        self.client = InstrumentedClient()
        self.store = Store(self.path, library=CORE)
        self.addCleanup(self.store.close)
        self.provider = NativeIndexProvider(self.client)
        self.rag = NativeRagStore(self.store, self.provider, tenant_id="alice", vector_space=SPACE, dimension=3)
        self.ns = self.rag.namespace("game:world-1", "npc:one", clock_id="game-hours-v1")

    def put(self, source="event:1", text="oldterm 东村见闻", *, namespace=None, revision=0,
            operation=None, chunks=None, vectors=None, label=""):
        chunks = chunks or [Chunk(text)]
        return self.rag.replace_source(namespace or self.ns, source, chunks, expected_revision=revision,
            operation_id=operation or source + ":write:" + str(revision), source_revision_label=label,
            vectors=vectors or [[1.0, 0.0, 0.0] for _ in chunks], vector_space=SPACE)

    def query(self, *, namespace=None, **kwargs):
        return self.rag.search(namespace or self.ns, as_of=kwargs.pop("as_of", 10),
            query_vector=kwargs.pop("query_vector", [1.0, 0.0, 0.0]), query_space=SPACE, **kwargs)

    def test_atomic_source_replacement_removes_old_chunks_and_projections(self):
        first = self.put(chunks=[Chunk("oldterm 第一段"), Chunk("oldterm 第二段")])
        second = self.put(text="newterm 新资料", revision=1)
        self.assertEqual(second["revision"], 2)
        self.assertEqual(len(second["chunk_ids"]), 1)
        self.assertFalse(set(first["chunk_ids"]) & set(second["chunk_ids"]))
        result = self.query()
        self.assertEqual([r["text"] for r in result["items"]], ["newterm 新资料"])
        for record_id in first["chunk_ids"]:
            self.assertIsNone(self.store.get(self.rag._chunk_key(self.ns, record_id)))
        lexical = self.rag.search(self.ns, as_of=10, mode="lexical", text="oldterm", strict_statistics=False)
        self.assertEqual(lexical["items"], [])
        self.assertEqual(lexical["statistics_policy"], "partition_unfiltered")

    def test_native_stage_failure_rolls_back_domain_and_postings(self):
        old = self.put()
        self.client.fail_stage = True
        with self.assertRaises(RagConflict):
            self.put(text="must-not-commit", revision=1)
        self.assertEqual(self.rag.get_source(self.ns, "event:1")["revision"], 1)
        self.assertEqual(self.query()["items"][0]["text"], "oldterm 东村见闻")
        self.assertEqual(self.rag.operation_status(self.ns, "event:1:write:1")["status"], "not_found")
        self.assertIsNotNone(self.store.get(self.rag._chunk_key(self.ns, old["chunk_ids"][0])))

    def test_source_ticket_cas_rechecked_after_embedding(self):
        self.put()
        one = self.rag.prepare_source(self.ns, "event:1", [Chunk("one")], expected_revision=1, operation_id="one")
        two = self.rag.prepare_source(self.ns, "event:1", [Chunk("two")], expected_revision=1, operation_id="two")
        self.rag.commit_source(one, vectors=[[0, 1, 0]], vector_space=SPACE)
        with self.assertRaises(RagConflict):
            self.rag.commit_source(two, vectors=[[1, 0, 0]], vector_space=SPACE)
        self.assertEqual(self.query()["items"][0]["text"], "one")

    def test_delete_during_embedding_prevents_resurrection(self):
        self.put()
        ticket = self.rag.prepare_source(self.ns, "event:1", [Chunk("stale result")], expected_revision=1, operation_id="stale")
        self.rag.delete_source(self.ns, "event:1", expected_revision=1, operation_id="delete")
        with self.assertRaises(RagConflict):
            self.rag.commit_source(ticket, vectors=[[1, 0, 0]], vector_space=SPACE)
        self.assertEqual(self.query()["match_evidence"], "proven_none")
        with self.assertRaises(RagUnsupported):
            self.put(revision=2, operation="revive")

    def test_retry_uses_receipt_and_preparation_needs_no_vectors(self):
        original = self.put(operation="stable-op")
        replay = self.rag.prepare_source(self.ns, "event:1", [Chunk("oldterm 东村见闻")],
            expected_revision=0, operation_id="stable-op")
        self.assertTrue(replay.replay)
        result = self.rag.commit_source(replay)
        self.assertTrue(result["replay"])
        self.assertEqual(result["chunk_ids"], original["chunk_ids"])
        self.assertEqual(self.rag.operation_status(self.ns, "stable-op")["status"], "applied_current")
        with self.assertRaises(RagConflict):
            self.put(text="different payload", operation="stable-op")
        self.put(text="new", revision=1)
        with self.assertRaises(RagConflict):
            self.put(operation="stable-op")
        self.assertEqual(self.rag.operation_status(self.ns, "stable-op")["status"], "superseded")

    def test_identical_source_with_new_operation_is_noop(self):
        first = self.put()
        ticket = self.rag.prepare_source(self.ns, "event:1", [Chunk("oldterm 东村见闻")],
            expected_revision=1, operation_id="same-again")
        self.assertTrue(ticket.unchanged)
        result = self.rag.commit_source(ticket)
        self.assertEqual((result["revision"], result["chunk_ids"]), (1, first["chunk_ids"]))

    def test_exact_owner_world_timeline_and_source_isolation(self):
        self.put(source="allowed", text="allowed")
        self.put(source="other-source", text="other source")
        for scope, owner, timeline in (("game:world-1", "npc:two", "main"),
                                        ("game:world-1", "", "main"),
                                        ("game:world-2", "npc:one", "main"),
                                        ("game:world-1", "npc:one", "fork")):
            namespace = self.rag.namespace(scope, owner, timeline=timeline, clock_id="game-hours-v1")
            for i in range(8):
                self.put(source=f"secret:{i}", text="PRIVATE", namespace=namespace)
        result = self.query(source_id="allowed", top_k=1)
        self.assertEqual([r["text"] for r in result["items"]], ["allowed"])
        injection = "' OR known_at >= 0 OR owner_id = '"
        self.assertEqual(self.query(source_id=injection)["items"], [])
        wrong = self.rag.namespace(injection, "npc:one", clock_id="game-hours-v1")
        self.assertEqual(self.query(namespace=wrong)["items"], [])

    def test_tenant_and_foreign_namespace_rejected(self):
        with self.assertRaises(RagConflict):
            NativeRagStore(self.store, NativeIndexProvider(self.client), tenant_id="bob", vector_space=SPACE, dimension=3)
        with Store(Path(self.directory.name) / "other", library=CORE) as other:
            foreign = NativeRagStore(other, NativeIndexProvider(self.client), tenant_id="bob", vector_space=SPACE, dimension=3)
            with self.assertRaises(RagConflict):
                foreign.get_source(self.ns, "event:1")
            with self.assertRaises(RagIntegrityError):
                self.provider.attach(other)

    def test_time_interval_and_explicit_cosine_threshold(self):
        self.put(chunks=[Chunk("old", known_at=1, expires_at=5), Chunk("new", known_at=5), Chunk("future", known_at=20)])
        self.assertEqual(self.query(as_of=0)["items"], [])
        self.assertEqual([r["text"] for r in self.query(as_of=4)["items"]], ["old"])
        self.assertEqual([r["text"] for r in self.query(as_of=5)["items"]], ["new"])
        self.assertEqual(self.query(as_of=5)["items"][0]["similarity"], 1.0)
        self.assertEqual(self.query(as_of=5, query_vector=[0, 1, 0], min_similarity=0.5)["items"], [])

    def test_query_modes_send_only_supported_fields(self):
        self.put()
        self.query()
        dense = self.client.calls[-1]
        self.assertEqual(dense["mode"], 1)
        self.assertNotIn("text", dense)
        self.rag.search(self.ns, as_of=10, text="oldterm", mode="lexical", strict_statistics=False)
        lexical = self.client.calls[-1]
        for name in ("vector_f32_le", "vector_space", "min_similarity"):
            self.assertNotIn(name, lexical)
        with self.assertRaises(RagUnsupported):
            self.rag.search(self.ns, as_of=10, text="oldterm", mode="lexical")
        with self.assertRaises(ValueError):
            self.query(text="not-allowed")
        with self.assertRaises(ValueError):
            self.query(min_similarity=2)

    def test_source_revision_label_remains_opaque(self):
        self.put(label="git:revision-A")
        row = self.query()["items"][0]
        self.assertEqual(row["source_revision_label"], "git:revision-A")
        self.assertEqual(row["source_version"], 1)

    def test_final_recheck_blocks_deleted_source(self):
        self.put()
        def after(result):
            self.client.transform = None
            self.rag.delete_source(self.ns, "event:1", expected_revision=1, operation_id="delete-during-query")
            return result
        self.client.transform = after
        with self.assertRaises(RagStaleRead):
            self.query()
        self.assertEqual(self.query()["items"], [])

    def test_final_recheck_blocks_replaced_source(self):
        self.put()
        def after(result):
            self.client.transform = None
            self.put(text="new publication", revision=1)
            return result
        self.client.transform = after
        with self.assertRaises(RagStaleRead):
            self.query()
        self.assertEqual(self.query()["items"][0]["text"], "new publication")

    def test_deleting_other_source_keeps_current_source_visible(self):
        self.put(source="kept", text="kept")
        self.put(source="removed", text="removed")
        def after(result):
            self.client.transform = None
            self.rag.delete_source(self.ns, "removed", expected_revision=1, operation_id="remove")
            return result
        self.client.transform = after
        self.assertEqual([r["text"] for r in self.query(source_id="kept")["items"]], ["kept"])

    def test_query_cancellation_and_post_query_cancellation(self):
        self.put()
        with self.assertRaises(RagCancelled):
            self.query(cancelled=lambda: True)
        cancelled = threading.Event()
        def after(result):
            self.client.transform = None
            cancelled.set()
            return result
        self.client.transform = after
        with self.assertRaises(RagCancelled):
            self.query(cancelled=cancelled.is_set)

    def test_cancel_during_real_native_scan(self):
        self.put(chunks=[Chunk("one"), Chunk("two"), Chunk("three")])
        checks = [0]
        def cancelled():
            checks[0] += 1
            return checks[0] >= 5
        with self.assertRaises(RagCancelled):
            self.query(cancelled=cancelled)
        self.assertGreaterEqual(checks[0], 5)
        self.assertIn("control", self.client.calls[-1])

    def test_deadline_while_waiting_for_lifecycle_gate(self):
        acquired, release = threading.Event(), threading.Event()
        def hold():
            with self.rag._gate.lock:
                acquired.set()
                release.wait(3)
        thread = threading.Thread(target=hold, daemon=True)
        thread.start()
        self.assertTrue(acquired.wait(2))
        try:
            with self.assertRaises(RagDeadline):
                self.query(budget=QueryBudget(timeout_ms=30))
        finally:
            release.set()
            thread.join(2)
        self.assertFalse(thread.is_alive())

    def test_real_native_budget_does_not_return_successful_partial(self):
        self.put(chunks=[Chunk("first"), Chunk("second"), Chunk("third")])
        with self.assertRaises(RagLimit):
            self.query(budget=QueryBudget(candidate_budget=1))
        with self.assertRaises(RagLimit):
            self.query(budget=QueryBudget(result_bytes=1))

    def test_bad_completion_evidence_and_wrong_snapshot_rejected(self):
        self.put()
        for key, value, error in (("termination", 1, RagLimit), ("index_coverage", 1, RagLimit),
                                  ("ranking_kind", 1, RagUnsupported), ("snapshot_version", 999999, RagIntegrityError),
                                  ("match_evidence", 0, RagIntegrityError)):
            self.client.transform = lambda result, k=key, v=value: {**result, k: v}
            with self.assertRaises(error, msg=key):
                self.query()
        self.client.transform = None

    def test_projection_metadata_and_source_locator_tampering_rejected(self):
        self.put()
        def wrong_owner(result):
            result["hits"][0]["document"]["metadata"]["owner_id"] = "npc:another"
            return result
        def wrong_locator(result):
            result["hits"][0]["document"]["source_locator"] = b"another-chunk"
            return result
        for transform in (wrong_owner, wrong_locator):
            self.client.transform = transform
            with self.assertRaises(RagIntegrityError):
                self.query()
        self.client.transform = None

    def test_empty_result_requires_proven_none(self):
        self.assertEqual(self.query()["match_evidence"], "proven_none")
        self.client.transform = lambda result: {**result, "match_evidence": 0}
        with self.assertRaises(RagIntegrityError):
            self.query()

    def test_vector_contract_and_validation(self):
        ticket = self.rag.prepare_source(self.ns, "event:1", [Chunk("text")], expected_revision=0, operation_id="op")
        with self.assertRaises(RagConflict):
            self.rag.commit_source(ticket, vectors=[[1, 0, 0]], vector_space=b"other-same-dim")
        for values in ([True, 0, 0], [math.nan, 0, 0], [math.inf, 0, 0], [0, 0, 0], [1, 0], [1e308, 0, 0]):
            with self.assertRaises(ValueError):
                self.rag.commit_source(ticket, vectors=[values], vector_space=SPACE)
        with self.assertRaises(RagNotFound):
            self.rag.get_source(self.ns, "event:1")
        self.rag.commit_source(ticket, vectors=[[1, 0, 0]], vector_space=SPACE)
        with self.assertRaises(RagConflict):
            self.rag.search(self.ns, as_of=10, query_vector=[1, 0, 0], query_space=b"wrong")

    def test_reopen_preserves_source_and_receipts(self):
        original = self.put(operation="persistent")
        self.store.close()
        with Store(self.path, library=CORE, create=False) as reopened:
            rag = NativeRagStore(reopened, NativeIndexProvider(InstrumentedClient()), tenant_id="alice", vector_space=SPACE, dimension=3)
            ns = rag.namespace("game:world-1", "npc:one", clock_id="game-hours-v1")
            self.assertEqual(rag.get_source(ns, "event:1")["chunk_ids"], original["chunk_ids"])
            self.assertEqual(rag.operation_status(ns, "persistent")["status"], "applied_current")
            self.assertEqual(rag.search(ns, as_of=10, query_vector=[1, 0, 0], query_space=SPACE)["items"][0]["text"], "oldterm 东村见闻")

    def test_delete_receipt_is_idempotent_and_old_write_does_not_revive(self):
        self.put(operation="old-write")
        first = self.rag.delete_source(self.ns, "event:1", expected_revision=1, operation_id="delete")
        second = self.rag.delete_source(self.ns, "event:1", expected_revision=1, operation_id="delete")
        self.assertEqual(first["revision"], second["revision"])
        self.assertTrue(second["replay"])
        self.assertEqual(self.rag.operation_status(self.ns, "old-write")["status"], "retired")
        with self.assertRaises(RagConflict):
            self.put(operation="old-write")

    def test_unsupported_features_and_clock_contract_fail_explicitly(self):
        with self.assertRaises(RagUnsupported):
            self.query(include_public=True)
        with self.assertRaises(RagUnsupported):
            self.query(mode="hybrid")
        with self.assertRaises(RagUnsupported):
            self.rag.put_fact()
        with self.assertRaises(RagUnsupported):
            self.rag.retire_namespace()
        with self.assertRaises(RagConflict):
            self.rag.namespace("game:world-1", "npc:one", clock_id="seconds")
        with self.assertRaises(ValueError):
            Chunk("backward", known_at=5, expires_at=5)
        with self.assertRaises(ValueError):
            self.query(as_of=True)

    def test_policy_flags_cannot_implicitly_disable_strict_statistics(self):
        for value in (None, 0, 1, "false"):
            with self.assertRaises(ValueError):
                self.rag.search(self.ns, as_of=10, text="text", mode="lexical", strict_statistics=value)
            with self.assertRaises(ValueError):
                self.query(include_public=value)
            with self.assertRaises(ValueError):
                self.rag.get_source(self.ns, "event:1", include_deleted=value)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + remaining)
