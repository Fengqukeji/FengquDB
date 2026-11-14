"""Data consistency, recovery and API tests against the actual FengquDB Store."""

import json
import os
import subprocess
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import patch

from llmwiki import BudgetError, ConflictError, Limits, Wiki, WikiError
from llmwiki.connectors import load_files
from llmwiki.providers import ExtractiveProvider
from llmwiki.server import WikiServer
from llmwiki.wiki import now_ms

CORE = os.environ.get("FENGQUDB_LIBRARY") or os.environ.get("FQDB_LIBRARY")


def annotation(value="blue", quote=None):
    return {"entities": [{"key": "p", "name": "FengquDB", "type": "product", "aliases": ["风趣数据库"]}],
            "claims": [{"entity": "p", "predicate": "color", "value": value, "quote": quote or value}]}


class FoundationTests(unittest.TestCase):
    def setUp(self):
        if not CORE:
            raise RuntimeError("set FENGQUDB_LIBRARY; these tests require the real Store")
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.wiki = Wiki(self.root / "db", library=CORE)

    def tearDown(self):
        self.wiki.close()
        self.temp.cleanup()

    def drain(self):
        result = self.wiki.run_jobs(limit=100, workers=1)
        self.assertFalse(any("error" in item for item in result["items"]), result)
        return result

    def test_batch_cas_idempotency_and_atomic_rollback(self):
        items = [{"uri": "batch:a", "content": "alpha"}, {"uri": "batch:b", "content": "beta"}]
        first = self.wiki.ingest_many(items, request_key="upload-1")
        self.assertEqual(first, self.wiki.ingest_many(items, request_key="upload-1"))
        with self.assertRaises(ConflictError):
            self.wiki.ingest_many([{"uri": "batch:a", "content": "different"}], request_key="upload-1")
        with self.assertRaises(ConflictError):
            self.wiki.ingest_many([{"uri": "batch:a", "content": "new", "expected_version": 1},
                                   {"uri": "batch:b", "content": "bad", "expected_version": 9}])
        self.assertEqual("alpha", self.wiki.source(first["items"][0]["id"])["content"])
        with self.assertRaises(ConflictError):
            self.wiki.delete_sources([{"id": first["items"][0]["id"], "expected_version": 1},
                                      {"id": first["items"][1]["id"], "expected_version": 9}])
        self.assertFalse(self.wiki.source(first["items"][0]["id"])["deleted"])
        with self.assertRaises(WikiError):
            self.wiki.ingest_many([items[0], items[0]])

    def test_source_versions_prevent_delete_restore_aba(self):
        first = self.wiki.ingest("aba:a", "blue")
        self.wiki.delete_source(first["id"], expected_version=1)
        restored = self.wiki.ingest("aba:a", "blue", expected_version=2)
        self.assertEqual(first["revision"], restored["revision"])
        self.assertEqual(3, restored["version"])
        self.assertTrue(self.wiki.source(first["id"], version=2)["deleted"])
        self.assertEqual(1, self.wiki.source(first["id"], version=1)["version"])
        with self.assertRaises(ConflictError):
            self.wiki.ingest("aba:a", "replacement", expected_version=1)
        with self.assertRaises(ConflictError):
            self.wiki.delete_source(first["id"], expected_version=1)

    def test_grounded_entities_conflicts_resolution_and_invalidation(self):
        a = self.wiki.ingest("facts:a", "The color is blue.", annotations=annotation())
        b = self.wiki.ingest("facts:b", "The color is red.", annotations=annotation("red"))
        self.drain()
        entity = self.wiki.listing("entity")["items"][0]
        detail = self.wiki.entity(entity["id"])
        self.assertEqual(2, len(detail["claims"]))
        self.assertEqual(["风趣数据库"], detail["aliases"])
        conflict = detail["conflicts"][0]
        self.assertEqual("open", conflict["state"])
        answer = self.wiki.ask("color")
        self.assertTrue(any(e["title"] == "Current conflict evidence" for e in answer["evidence"]))
        claim = next(c for c in detail["claims"] if c["value"] == "blue")
        raw = self.wiki.source(claim["source_id"], revision=claim["revision"])
        self.assertEqual(claim["quote"], raw["content"][claim["start"]:claim["end"]])
        resolved = self.wiki.resolve_conflict(conflict["id"], [claim["id"]],
            expected_generation=conflict["generation"], note="Use the signed specification.", actor="reviewer")
        self.assertEqual("resolved", resolved["state"])
        with self.assertRaises(ConflictError):
            self.wiki.resolve_conflict(conflict["id"], [claim["id"]],
                expected_generation=conflict["generation"], note="late decision", actor="reviewer")
        self.wiki.ingest("facts:b", "The color is green.", annotations=annotation("green"), expected_version=b["version"])
        refreshed = self.wiki.conflict(conflict["id"])
        self.assertEqual("open", refreshed["state"])
        self.assertIsNone(refreshed["resolution"])
        self.assertEqual({"blue", "green"}, {c["value"] for c in refreshed["claims"]})
        archived = self.wiki.conflict(conflict["id"], generation=conflict["generation"])
        self.assertEqual({"blue", "red"}, {c["value"] for c in archived["claims"]})
        self.wiki.delete_source(a["id"])
        self.assertEqual("inactive", self.wiki.conflict(conflict["id"])["state"])
        self.wiki.delete_source(b["id"])
        self.drain()
        self.assertEqual("retired", self.wiki.entity(entity["id"])["state"])
        self.assertIsNone(self.wiki.page("entity-" + entity["id"]))

    def test_knowledge_validation_and_pending_entity_lifecycle(self):
        malformed = annotation()
        malformed["claims"][0]["entity"] = []
        with self.assertRaises(WikiError):
            self.wiki.ingest("bad:typed", "blue", annotations=malformed)
        with self.assertRaises(WikiError):
            self.wiki.ingest("bad:a", "no matching text", annotations=annotation())
        with self.assertRaises(WikiError):
            self.wiki.ingest("bad:a", "blue blue", annotations=annotation())
        good = annotation()
        good["claims"][0]["start"] = 5
        a = self.wiki.ingest("facts:a", "blue blue", annotations=good)
        reordered = {"claims": [dict(reversed(list(good["claims"][0].items())))], "entities": good["entities"]}
        self.assertFalse(self.wiki.ingest("facts:a", "blue blue", annotations=reordered)["changed"])
        self.drain()
        eid = self.wiki.listing("entity")["items"][0]["id"]
        self.wiki.ingest("facts:a", "new unannotated evidence")
        self.assertEqual([], self.wiki.entity(eid)["claims"])
        self.assertEqual("active", self.wiki.entity(eid)["state"])
        self.assertTrue(self.wiki.page("entity-" + eid)["stale"])
        self.drain()  # Offline provider extracts no entities; this is now a true retirement.
        self.assertEqual("retired", self.wiki.entity(eid)["state"])

    def test_model_extraction_uses_one_compile_call(self):
        class Structured(ExtractiveProvider):
            identity = "structured-fixture"
            calls = 0
            def compile(self, title, sources, links):
                self.calls += 1
                result = super().compile(title, sources, links)
                return dict(result, **annotation()) if title == "Raw" else result
        compiler = Structured()
        self.wiki.compiler = compiler
        self.wiki.ingest("model:a", "blue", title="Raw")
        self.drain()
        self.assertEqual(2, compiler.calls)  # One source compilation plus the derived entity page.
        self.assertEqual(1, len(self.wiki.listing("claim")["items"]))
        self.assertEqual(1, len(self.wiki.listing("entity")["items"]))
        eid = self.wiki.listing("entity")["items"][0]["id"]
        self.wiki.retry("entity-" + eid)
        archived_before = self.wiki.page("entity-" + eid)
        self.drain()
        self.assertEqual(2, compiler.calls)  # Grounded entity context is part of the reusable artifact key.
        archived_after = self.wiki.page("entity-" + eid, revision=archived_before["revision"])
        self.assertEqual(archived_before["version"], archived_after["version"])

    def test_reviewed_stale_source_can_finish_extraction_without_replacing_edit(self):
        first = self.wiki.ingest("review:a", "before", annotations={})
        self.drain()
        pid = "source-" + first["id"][:24]
        previous = self.wiki.page(pid)
        current = self.wiki.ingest("review:a", "new evidence")
        edited = self.wiki.revise_page(pid, "# Reviewed\n\nNew evidence checked. [S1]",
            expected_revision=previous["revision"], expected_version=previous["version"], actor="reviewer",
            expected_source_versions={first["id"]: current["version"]})
        self.drain()
        self.assertEqual(edited["revision"], self.wiki.page(pid)["revision"])
        self.assertFalse(self.wiki.page(pid)["stale"])
        self.assertFalse(self.wiki.source(first["id"])["knowledge_pending"])

    def test_human_revision_is_pinned_and_release_is_guarded(self):
        a = self.wiki.ingest("manual:a", "before")
        self.wiki.define_page("guide", "Guide", [a["id"]])
        self.drain()
        page = self.wiki.page("guide")
        human = self.wiki.revise_page("guide", "# Edited\n\nReviewed by a human. [S1]",
            expected_revision=page["revision"], expected_version=page["version"], actor="editor")
        self.wiki.ingest("manual:a", "after")
        self.drain()
        current = self.wiki.page("guide")
        self.assertEqual(human["markdown"], current["markdown"])
        self.assertTrue(current["stale"])
        with self.assertRaises(ConflictError):
            self.wiki.revise_page("guide", "Late unsynchronized edit [S1]", expected_revision=human["revision"],
                expected_version=human["version"], actor="editor")
        self.assertEqual("review_required", next(j for j in self.wiki.listing("job")["items"] if j["page_id"] == "guide")["state"])
        with self.assertRaises(ConflictError):
            self.wiki.release_page("guide", expected_revision=page["revision"], expected_version=page["version"], actor="editor")
        self.wiki.release_page("guide", expected_revision=human["revision"], expected_version=human["version"], actor="editor")
        self.drain()
        self.assertIn("after", self.wiki.page("guide")["markdown"])
        self.assertEqual(human["markdown"], self.wiki.page("guide", revision=human["revision"])["markdown"])

    def test_pinned_source_still_extracts_knowledge_without_overwriting(self):
        a = self.wiki.ingest("pin:a", "blue", annotations=annotation())
        self.drain()
        pid = "source-" + a["id"][:24]
        page = self.wiki.page(pid)
        human = self.wiki.revise_page(pid, "# Human\n\nBlue [S1]", expected_revision=page["revision"],
            expected_version=page["version"], actor="editor")
        self.wiki.ingest("pin:a", "replacement with no extracted entities")
        self.drain()
        self.assertEqual(human["markdown"], self.wiki.page(pid)["markdown"])
        self.assertFalse(self.wiki.source(a["id"])["knowledge_pending"])

    def test_answer_cache_and_revocation_discard(self):
        class Answerer(ExtractiveProvider):
            identity = "answerer-fixture"
            calls = 0
            def answer(self, question, evidence):
                self.calls += 1
                return "answer [E1]"
        answerer = Answerer()
        self.wiki.answerer = answerer
        first = self.wiki.ingest("answer:a", "cacheuniqueterm")
        self.assertTrue(self.wiki.ask("cacheuniqueterm")["citation_validated"])
        self.wiki.ask("cacheuniqueterm")
        self.assertEqual(1, answerer.calls)
        class Revoker(Answerer):
            identity = "revoker"
            def answer(inner, question, evidence):
                self.wiki.delete_source(first["id"])
                self.wiki.ingest("answer:a", "cacheuniqueterm")
                return "revoked material [E1]"
        self.wiki.answerer = Revoker()
        result = self.wiki.ask("cacheuniqueterm")
        self.assertEqual("", result["answer"])
        self.assertEqual([], result["evidence"])
        self.assertFalse(result["citation_validated"])
        self.assertTrue(result["retryable"])

    @patch("llmwiki.wiki.now_ms")
    def test_worker_limits_counts_cancel_and_lease_renewal(self, wall_clock):
        base = now_ms() + 1000
        clock = [base]
        wall_clock.side_effect = lambda: clock[0]
        # run_jobs drains due work, not future work. Keep scheduling and
        # renewal on one explicit timeline, independent of wall-clock steps.
        self.wiki.ingest_many([{"uri": "worker:" + str(i), "content": "work " + str(i)} for i in range(7)])
        result = self.wiki.run_jobs(limit=4, workers=3)
        self.assertEqual(4, len(result["items"]))
        self.assertEqual({"done": 4, "pending": 3}, self.wiki.status()["jobs"])
        self.assertEqual([], self.wiki.run_jobs(cancelled=lambda: True)["items"])
        self.drain()
        self.assertEqual({"done": 7}, self.wiki.status()["jobs"])
        entered, release = threading.Event(), threading.Event()
        class Slow(ExtractiveProvider):
            identity = "slow-renewal"
            def compile(self, *args):
                entered.set()
                release.wait(5)
                return super().compile(*args)
        self.wiki.compiler = Slow()
        self.wiki.ingest("worker:slow", "slow")
        renewed = threading.Event()
        renew = self.wiki._renew
        def observe(job, lease_ms):
            started = clock[0]
            result = renew(job, lease_ms)
            if result and started >= base + 300:
                renewed.set()
            return result
        with patch.object(self.wiki, "_renew", side_effect=observe), ThreadPoolExecutor(1) as pool:
            future = pool.submit(self.wiki.run_once, lease_ms=150)
            try:
                self.assertTrue(entered.wait(5))
                # Keep the 150ms lease and simulate the reported 300ms stall.
                # Synchronize actual renewal, rather than assuming OS thread
                # scheduling always runs a heartbeat before a wall-clock sleep.
                clock[0] = base + 300
                self.assertTrue(renewed.wait(5))
                self.assertIsNone(self.wiki._claim(150))
            finally:
                release.set()
            self.assertEqual("done", future.result(5)["state"])
        with patch("llmwiki.storage.View.scan", side_effect=AssertionError("status must not scan job history")):
            self.assertEqual(8, self.wiki.status()["jobs"]["done"])

    def test_quotas_connector_and_schema_refusal(self):
        folder = self.root / "files"
        folder.mkdir()
        (folder / "a.md").write_text("中文", encoding="utf-8-sig")
        (folder / "b.txt").write_text("second", encoding="utf-8")
        items = load_files(folder, self.wiki.limits, uri_prefix="docs:local")
        self.assertEqual(["docs:local/a.md", "docs:local/b.txt"], [i["uri"] for i in items])
        self.wiki.ingest_many(items)
        with self.assertRaises(BudgetError):
            load_files(folder, Limits(batch_items=1))
        with self.assertRaises(BudgetError):
            self.wiki.ingest("huge:a", "x" * (self.wiki.limits.source_bytes + 1))
        link = folder / "escape.md"
        if os.name == "nt":
            target = self.root / "outside"
            target.mkdir()
            subprocess.run(["cmd.exe", "/c", "mklink", "/J", str(link), str(target)],
                check=True, capture_output=True)
            try:
                with self.assertRaises(WikiError):
                    load_files(folder, self.wiki.limits)
            finally:
                link.rmdir()  # Remove only the junction; the target directory is retained.
        else:
            link.symlink_to(self.root / "outside.md")
            try:
                with self.assertRaises(WikiError):
                    load_files(folder, self.wiki.limits)
            finally:
                link.unlink()
        self.wiki.repo.write(lambda tx: tx.put("schema", {"version": 1, "tokenizer": 1}))
        self.wiki.close()
        with self.assertRaisesRegex(WikiError, "fresh namespace"):
            Wiki(self.root / "db", library=CORE)

    def test_http_batch_conflicts_quotas_and_entities(self):
        with WikiServer(("127.0.0.1", 0), self.wiki, background=False, maintenance=False) as server:
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            def call(path, body):
                request = urllib.request.Request("http://127.0.0.1:" + str(server.server_port) + path,
                    json.dumps(body).encode(), {"Content-Type": "application/json"})
                with urllib.request.urlopen(request, timeout=5) as response:
                    return json.load(response)
            try:
                result = call("/v1/sources/batch", {"items": [{"uri": "http:a", "content": "blue", "annotations": annotation()}]})
                with self.assertRaises(urllib.error.HTTPError) as error:
                    call("/v1/sources", {"uri": "http:a", "content": "wrong", "expected_version": 0})
                self.assertEqual(409, error.exception.code)
                error.exception.close()
                with self.assertRaises(urllib.error.HTTPError) as error:
                    call("/v1/sources/batch", {"items": []})
                self.assertEqual(413, error.exception.code)
                error.exception.close()
                run = call("/v1/jobs/run", {"workers": 2, "limit": 8})
                self.assertTrue(run["items"])
                eid = self.wiki.listing("entity")["items"][0]["id"]
                with urllib.request.urlopen("http://127.0.0.1:" + str(server.server_port) + "/v1/entities/" + eid) as response:
                    self.assertEqual(1, len(json.load(response)["claims"]))
                self.assertEqual(1, len(result["items"]))
            finally:
                server.shutdown()
                thread.join(5)


if __name__ == "__main__":
    unittest.main()
