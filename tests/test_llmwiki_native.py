"""Wiki domain projections and actual shared-index integration; no retrieval algorithm."""

import os
import tempfile
import unittest
from pathlib import Path

from bindings.python.fengqudb import Error
from bindings.python import fengqudb_index as ffi
from bindings.python import fengqudb_index_wire as wire
from llmwiki import NativeIndexProvider, Wiki
from llmwiki.native_index_provider import NativeProjectionError
from test_fengqudb_index_python import result_bytes

CORE = os.environ.get("FENGQUDB_LIBRARY") or os.environ.get("FQDB_LIBRARY")
INDEX = os.environ.get("FENGQUDB_INDEX_LIBRARY")


class RecordingClient:
    """Records wire mutations and returns explicit fixtures; implements no search."""
    def __init__(self):
        self.calls = []
        self.response = result_bytes()
        self.failure = None
    def apply(self, handle, request, **_options):
        if self.failure:
            raise self.failure
        operation = {}
        for number, wire_type, value in wire.fields(request):
            operation[number] = wire.decode_document(value) if number == 5 else value
        self.calls.append((handle, operation))
    def query(self, handle, request, **_options):
        self.last_query = {number: value for number, _, value in wire.fields(request)}
        return self.response
    def maintain(self, handle, **request):
        self.last_maintenance = request
        return {"finished": False, "next_key": b"checkpoint"}


@unittest.skipUnless(CORE, "set FENGQUDB_LIBRARY for provider/domain integration")
class NativeProviderDomainTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.client = RecordingClient()
        self.index = NativeIndexProvider(client=self.client, index_space=b"wiki-test")
        self.wiki = Wiki(Path(self.directory.name) / "db", library=CORE, index_provider=self.index)
    def tearDown(self):
        self.wiki.close()
        self.directory.cleanup()
    def ingest(self, body="nativefixtureterm 中文资料"):
        return self.wiki.ingest("native:test", body, title="Native source")
    def drain(self):
        for _ in range(10):
            result = self.wiki.run_once()
            if result is None:
                return
            self.assertEqual("done", result["state"])
        self.fail("fixture queue did not drain")
    def mutation_documents(self):
        return [operation[5] for _, operation in self.client.calls if operation.get(3) == 1]

    def test_projection_versions_do_not_reuse_domain_hash_or_lifecycle(self):
        source = self.ingest()
        original = self.mutation_documents()[-1]
        self.ingest("replacement body")
        replacement = self.mutation_documents()[-1]
        self.assertGreater(replacement["projection_version"], original["projection_version"])
        self.assertEqual(original["lifecycle_generation"], replacement["lifecycle_generation"])
        self.wiki.delete_source(source["id"])
        self.ingest()
        revived = self.mutation_documents()[-1]
        self.assertGreater(revived["lifecycle_generation"], original["lifecycle_generation"])
        self.assertEqual(original["domain_revision"], revived["domain_revision"])
        self.assertEqual(source["revision"], revived["metadata"]["domain_revision_hash"])
        self.assertGreater(revived["metadata"]["source_publication_generation"].value, 1)

    def test_dirty_patch_has_no_body_and_cached_publication_advances(self):
        source = self.ingest()
        self.drain()
        page_id = "source-" + source["id"][:24]
        before = self.mutation_documents()[-1]
        self.wiki.retry(page_id)
        patch_request = self.client.calls[-1][1]
        self.assertEqual(2, patch_request[3])
        self.assertFalse(patch_request[5]["texts"])
        self.assertFalse(patch_request[5]["metadata"]["visible"])
        self.wiki.retry(page_id)  # Already dirty, but the new build target is a new projection.
        next_patch = self.client.calls[-1][1]
        self.assertEqual(2, next_patch[3])
        self.assertGreater(next_patch[7], patch_request[7])
        self.drain()
        after = self.mutation_documents()[-1]
        self.assertEqual(before["domain_revision"], after["domain_revision"])
        self.assertGreater(after["projection_version"], before["projection_version"])
        self.assertGreater(after["metadata"]["publication_generation"].value,
                           before["metadata"]["publication_generation"].value)
        self.assertEqual(before["lifecycle_generation"], after["lifecycle_generation"])

    def test_same_snapshot_materialization_and_projection_mismatch_rejected(self):
        source = self.ingest()
        document = self.mutation_documents()[-1]
        self.client.response = result_bytes([ffi.Document(**document)])
        result = self.wiki.search("nativefixtureterm")
        self.assertIn("中文资料", result["results"][0]["text"])
        self.assertEqual(source["revision"], result["results"][0]["revision"])
        self.assertEqual("has_hits", result["match_evidence"])
        self.assertIsNotNone(result["snapshot_version"])
        self.assertEqual(b"wiki-test", self.client.last_query[1])
        self.assertTrue(self.client.last_query[2])
        self.assertIn(6, self.client.last_query)  # mandatory visibility predicate
        broken = dict(document, projection_version=document["projection_version"] + 1)
        self.client.response = result_bytes([ffi.Document(**broken)])
        with self.assertRaises(NativeProjectionError):
            self.wiki.search("nativefixtureterm")

    def test_native_apply_failure_rolls_back_domain_ingestion(self):
        self.client.failure = Error(9, "fixture limit")
        with self.assertRaises(Error):
            self.ingest()
        self.assertFalse(self.wiki.listing("source")["items"])
        with self.wiki.repo.snapshot() as view:
            self.assertIsNone(view.get("native-index/revision-sequence"))

    def test_default_missing_native_is_explicit_and_creates_no_store(self):
        from unittest.mock import patch
        path = Path(self.directory.name) / "missing-native"
        with patch("llmwiki.wiki.NativeIndexProvider", side_effect=ffi.IndexUnavailable("native unavailable")):
            with self.assertRaises(ffi.IndexUnavailable):
                Wiki(path, library=CORE)
        self.assertFalse(path.exists())

    def test_proven_none_does_not_require_total_count(self):
        self.client.response = result_bytes(evidence=2, total=None)
        answer = self.wiki.ask("no-match")
        self.assertTrue(answer["empty_result_confirmed"])
        self.assertEqual("proven_none", answer["match_evidence"])
        self.assertIsNone(answer["total_hits"]["value"])
        self.client.response = result_bytes(termination=1, evidence=0)
        answer = self.wiki.ask("partial")
        self.assertFalse(answer["empty_result_confirmed"])
        self.assertEqual("exact", answer["ranking_kind"])
        self.assertEqual("posting_limit", answer["termination"])
        self.assertTrue(answer["truncated"])
        self.client.response = result_bytes(termination=7, evidence=0)
        limited = self.wiki.search("prefix")
        self.assertEqual("term_limit", limited["termination"])
        self.assertTrue(limited["truncated"])
        gc = self.wiki.maintain_index(operation=4, after_key="abcd")
        self.assertEqual(b"\xab\xcd", self.client.last_maintenance["after_key"])
        self.assertEqual(b"checkpoint".hex(), gc["next_key"])
        self.wiki.maintain_index(operation=2, vector_space="embedding-v1", max_records=16)
        self.assertEqual(b"embedding-v1", self.client.last_maintenance["vector_space"])


@unittest.skipUnless(CORE and INDEX, "actual Core bridge + native index library have not been supplied")
class NativeWikiLibraryIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.client = ffi.NativeIndex(INDEX)
        self.path = Path(self.directory.name) / "db"

    def tearDown(self):
        self.directory.cleanup()

    def test_wiki_ingest_publish_snapshot_invalidate_and_delete(self):
        with Wiki(self.path, library=CORE) as wiki:
            self.assertIsInstance(wiki.index, NativeIndexProvider)
            source = wiki.ingest("actual:test", "actualoldterm 中文原件", title="Native")
            while (result := wiki.run_once()) is not None:
                self.assertEqual("done", result["state"], result)
            self.assertTrue(wiki.ask("actualoldterm")["citation_validated"])
            page_id = "source-" + source["id"][:24]
            with wiki.repo.snapshot() as view:
                first_publication = wiki.index.get_document(view, "p-" + page_id)
            wiki.retry(page_id)
            wiki.retry(page_id)
            while (result := wiki.run_once()) is not None:
                self.assertEqual("done", result["state"], result)
            with wiki.repo.snapshot() as view:
                republished = wiki.index.get_document(view, "p-" + page_id)
            self.assertEqual(first_publication["revision"], republished["revision"])
            self.assertGreater(republished["publication_generation"], first_publication["publication_generation"])
            with wiki.repo.snapshot() as old:
                wiki.ingest("actual:test", "actualnewterm 新版资料", title="Native")
                self.assertTrue(wiki.index.search(old, "actualoldterm")["results"])
                self.assertFalse(wiki.search("actualoldterm")["results"])
            while (result := wiki.run_once()) is not None:
                self.assertEqual("done", result["state"], result)
            self.assertTrue(wiki.search("actualnewterm")["results"])
            wiki.delete_source(source["id"])
            self.assertFalse(wiki.search("actualnewterm")["results"])
            self.assertTrue(wiki.ask("nonexistentterm")["empty_result_confirmed"])
            while (result := wiki.run_once()) is not None:
                self.assertEqual("retired", result["state"], result)
            wiki.ingest("actual:test", "actualoldterm 中文原件", title="Native")
            while (result := wiki.run_once()) is not None:
                self.assertEqual("done", result["state"], result)
            revived = wiki.search("actualoldterm")
            self.assertTrue(revived["results"])
            self.assertTrue(all(item["lifecycle_generation"] > 1 for item in revived["results"]))

    def test_extended_search_vectors_conflicts_and_maintenance(self):
        from test_llmwiki_foundation import annotation
        from llmwiki import ConflictError
        with Wiki(self.path, library=CORE, index_provider=NativeIndexProvider(client=self.client)) as wiki:
            wiki.ingest_many([{"uri": "extended:a", "title": "Native knowledge", "content": "alpha beta 中文颜色 blue", "annotations": annotation()},
                              {"uri": "extended:b", "content": "alpha distant beta red", "annotations": annotation("red")}])
            done = wiki.run_jobs(limit=20, workers=2)
            self.assertTrue(all(i["state"] in ("done", "superseded") for i in done["items"]), done)
            found = wiki.search("alpha", filters={"kind": "chunk"}, include_positions=True, include_explain=True, statistics_policy=1)
            self.assertEqual(2, len(found["results"]))
            self.assertTrue(found["results"][0]["positions"])
            json_safe = __import__("json").dumps(found)
            self.assertIn("utf8_bytes", json_safe)
            phrase = wiki.search("", phrases=["alpha beta"], filters={"kind": "chunk"})
            self.assertEqual(1, len(phrase["results"]))
            self.assertEqual(2, len(wiki.search("alph", prefix_last=True, filters={"kind": "chunk"})["results"]))
            selected = found["results"][0]
            updated = wiki.set_vectors([{"id": selected["id"], "vector": [1., 0.],
                "expected_projection_version": selected["projection_version"]}], vector_space="embedding-fixture-v1")
            self.assertGreater(updated["items"][0]["projection_version"], selected["projection_version"])
            with self.assertRaises(ConflictError):
                wiki.set_vectors([{"id": selected["id"], "vector": [0., 1.],
                    "expected_projection_version": selected["projection_version"]}], vector_space="embedding-fixture-v1")
            dense = wiki.search("", mode=2, vector_f32_le=[1., 0.], vector_space="embedding-fixture-v1")
            self.assertEqual(selected["id"], dense["results"][0]["id"])
            self.assertIn(dense["ranking_kind"], ("exact", "approximate"))
            self.assertTrue(wiki.maintain_index(operation=1, max_terms=128)["finished"])
            self.assertTrue(wiki.maintain_index(operation=3, max_records=128)["finished"])
            self.assertIn("next_key", wiki.maintain_index(operation=4, max_records=128))
            eid = wiki.listing("entity")["items"][0]["id"]
            self.assertEqual("open", wiki.entity(eid)["conflicts"][0]["state"])
            self.assertTrue(wiki.ask("alpha", filters={"kind": "chunk"})["citation_validated"])



if __name__ == "__main__":
    unittest.main()
