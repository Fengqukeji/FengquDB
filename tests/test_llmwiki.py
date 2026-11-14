"""Real FengquDB integration tests; providers are deterministic or local wire fixtures."""

import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from contextlib import ExitStack
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest.mock import patch

from llmwiki import IndexDefinition, PythonIndexProvider, Wiki, WikiError
from llmwiki.providers import CompatibleProvider, ExtractiveProvider, ProviderError
from llmwiki.server import WikiServer
from llmwiki.wiki import now_ms

LIBRARY = os.environ.get("FENGQUDB_LIBRARY") or os.environ.get("FQDB_LIBRARY")


class WikiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not LIBRARY:
            raise RuntimeError("set FENGQUDB_LIBRARY to a native library built from this checkout")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.path = self.root / "db"
        self.wiki = Wiki(self.path, library=LIBRARY)

    def tearDown(self):
        self.wiki.close()
        self.temp.cleanup()

    def drain(self, wiki=None):
        wiki = wiki or self.wiki
        results = []
        for _ in range(100):
            result = wiki.run_once()
            if result is None:
                return results
            self.assertNotIn("error", result)
            results.append(result)
        self.fail("queue failed to drain")

    def add(self, uri="test:one", content="FengquDB provides durable transactional storage.", title="Storage"):
        return self.wiki.ingest(uri, content, title=title)

    def test_idempotence_chinese_and_restart(self):
        source = self.add(content="数据库支持事务与增量更新。中文知识库持久保存。", title="中文资料")
        stats = self.wiki.status()["search"]
        self.assertFalse(self.add(content="数据库支持事务与增量更新。中文知识库持久保存。", title="中文资料")["changed"])
        self.assertEqual(stats, self.wiki.status()["search"])
        self.assertTrue(self.wiki.search("增量更新")["results"])
        self.wiki.close()
        self.wiki = Wiki(self.path, library=LIBRARY)
        self.assertEqual("done", self.drain()[0]["state"])
        self.assertEqual(source["revision"], self.wiki.source(source["id"])["revision"])
        self.assertTrue(self.wiki.lint()["ok"])

    def test_revision_history_and_stale_exclusion(self):
        first = self.add(content="olduniqueterm durable storage")
        self.drain()
        pid = "source-" + first["id"][:24]
        previous_page_revision = self.wiki.page(pid)["revision"]
        second = self.add(content="newuniqueterm reliable storage")
        self.assertNotEqual(first["revision"], second["revision"])
        self.assertTrue(self.wiki.page(pid)["stale"])
        self.assertFalse(self.wiki.search("olduniqueterm")["results"])
        self.assertIn("olduniqueterm", self.wiki.source(first["id"], revision=first["revision"])["content"])
        self.drain()
        self.assertFalse(self.wiki.page(pid)["stale"])
        self.assertIn("newuniqueterm", self.wiki.page(pid)["markdown"])
        archived = self.wiki.page(pid, revision=previous_page_revision)
        self.assertTrue(archived["archived"])
        self.assertIn("olduniqueterm", archived["markdown"])

    def test_mixed_script_tokenization_and_exact_budget_reporting(self):
        self.add(content="FengquDB中文数据库支持 reliable持久存储。")
        self.assertTrue(self.wiki.search("中文数据库")["results"])
        self.add("test:two", "rarecommon anotherterm")
        self.add("test:three", "anotherterm")
        result = self.wiki.search("rarecommon anotherterm", posting_budget=1)
        self.assertTrue(result["truncated"])
        self.assertEqual("approximate", result["ranking_kind"])
        self.assertEqual("posting_limit", result["termination"])
        self.assertEqual("complete", result["index_coverage"])
        self.assertEqual("lower_bound", result["total_hits"]["relation"])

    def test_topics_backlinks_and_selective_rebuild(self):
        first, second = self.add(), self.add("test:two", "Storage uses write-ahead logging.", "Logging")
        self.wiki.define_page("storage-guide", "Storage guide", [first["id"], second["id"]],
                              links=["source-" + second["id"][:24]])
        self.drain()
        page = self.wiki.page("storage-guide")
        self.assertEqual(2, len(page["citations"]))
        target = self.wiki.page("source-" + second["id"][:24])
        self.assertIn("storage-guide", target["backlinks"])
        unaffected_revision = target["revision"]
        self.add(content="FengquDB adds consistent snapshots.")
        self.assertTrue(self.wiki.page("storage-guide")["stale"])
        self.drain()
        self.assertEqual(unaffected_revision, self.wiki.page(target["id"])["revision"])
        self.assertNotEqual(page["revision"], self.wiki.page("storage-guide")["revision"])
        self.assertTrue(self.wiki.lint()["ok"])

    def test_delete_rebuild_and_last_source_retirement(self):
        first = self.add(content="deleteduniqueterm")
        second = self.add("test:second", "retaineduniqueterm")
        self.wiki.define_page("combined", "Combined", [first["id"], second["id"]])
        self.drain()
        self.assertTrue(self.wiki.delete_source(first["id"]))
        self.assertFalse(self.wiki.delete_source(first["id"]))
        self.assertFalse(self.wiki.search("deleteduniqueterm")["results"])
        self.drain()
        self.assertIsNone(self.wiki.page("source-" + first["id"][:24]))
        self.assertEqual(1, self.wiki.page("combined")["missing_sources"])
        self.assertEqual(second["id"], self.wiki.page("combined")["citations"][0]["source_id"])
        self.wiki.delete_source(second["id"])
        self.drain()
        self.assertIsNone(self.wiki.page("combined"))
        self.assertEqual(0, self.wiki.status()["search"]["count"])
        self.assertTrue(self.wiki.source(first["id"])["deleted"])

    def test_atomic_rollback_preserves_old_indexes(self):
        first = self.add(content="oldatomicterm")
        from llmwiki import retrieval
        original = retrieval.index_document
        def broken(*args, **kwargs):
            original(*args, **kwargs)
            raise RuntimeError("injected failure after staged index writes")
        with patch("llmwiki.retrieval.index_document", side_effect=broken):
            with self.assertRaises(RuntimeError):
                self.add(content="newatomicterm")
        self.assertEqual(first["revision"], self.wiki.source(first["id"])["revision"])
        self.assertTrue(self.wiki.search("oldatomicterm")["results"])
        self.assertFalse(self.wiki.search("newatomicterm")["results"])

    def test_expired_lease_survives_restart(self):
        self.add()
        claim = self.wiki._claim(10)
        self.assertEqual(1, claim["attempts"])
        self.wiki.close()
        self.wiki = Wiki(self.path, library=LIBRARY)
        with patch("llmwiki.wiki.now_ms", return_value=claim["due_ms"] + 1):
            self.assertEqual("done", self.wiki.run_once()["state"])

    def test_late_model_result_cannot_overwrite_updated_source(self):
        entered, release = threading.Event(), threading.Event()
        class Blocking(ExtractiveProvider):
            identity = "blocking"
            def compile(self, *args):
                entered.set()
                if not release.wait(10):
                    raise RuntimeError("test timeout")
                return super().compile(*args)
        self.wiki.provider = Blocking()
        self.add(content="oldconcurrentterm")
        with ThreadPoolExecutor(1) as pool:
            future = pool.submit(self.wiki.run_once)
            self.assertTrue(entered.wait(10))
            self.add(content="newconcurrentterm")
            release.set()
            self.assertEqual("superseded", future.result(10)["state"])
        self.drain()
        self.assertFalse(self.wiki.search("oldconcurrentterm")["results"])
        self.assertTrue(self.wiki.search("newconcurrentterm")["results"])

    def test_two_workers_lease_fencing(self):
        entered, release = threading.Event(), threading.Event()
        class FirstBlocks(ExtractiveProvider):
            identity = "lease-fence"
            calls = 0
            def compile(self, *args):
                self.calls += 1
                if self.calls == 1:
                    entered.set()
                    if not release.wait(10):
                        raise RuntimeError("test timeout")
                return super().compile(*args)
        self.wiki.provider = FirstBlocks()
        self.add()
        with ThreadPoolExecutor(1) as pool:
            future = pool.submit(lambda: self.wiki.run_once(lease_ms=1))
            self.assertTrue(entered.wait(10))
            with patch("llmwiki.wiki.now_ms", return_value=now_ms() + 10000):
                self.assertEqual("done", self.wiki.run_once()["state"])
            release.set()
            self.assertEqual("superseded", future.result(10)["state"])

    def test_invalid_model_retry_budget_and_manual_recovery(self):
        class Invalid(ExtractiveProvider):
            identity = "bad"
            def compile(self, *_args):
                return {"markdown": "fabricated [S99]", "used_sources": ["S99"], "links": []}
        self.wiki.provider = Invalid()
        source = self.add()
        base = now_ms()
        for attempt, state in enumerate(("pending", "pending", "failed")):
            with patch("llmwiki.wiki.now_ms", return_value=base + attempt * 100000):
                result = self.wiki.run_once()
                self.assertEqual(state, result["state"])
                self.assertEqual("ProviderError", result["error"])
        self.assertIsNone(self.wiki.run_once())
        self.wiki.provider = ExtractiveProvider()
        self.wiki.retry("source-" + source["id"][:24])
        self.drain()

    def test_artifact_cache_reuses_compilation(self):
        class Counting(ExtractiveProvider):
            calls = 0
            def compile(self, *args):
                self.calls += 1
                return super().compile(*args)
        provider = Counting()
        self.wiki.provider = provider
        source = self.add()
        self.drain()
        self.wiki.retry("source-" + source["id"][:24])
        self.drain()
        self.assertEqual(1, provider.calls)

    def test_namespaces_validation_and_pagination(self):
        first = self.add()
        self.add("test:two")
        self.wiki.close()
        with Wiki(self.path, library=LIBRARY, namespace="another") as other:
            self.assertFalse(other.search("storage")["results"])
            other.ingest("test:one", "partitionuniqueterm")
        self.wiki = Wiki(self.path, library=LIBRARY)
        self.assertEqual(first["revision"], self.wiki.source(first["id"])["revision"])
        self.assertFalse(self.wiki.search("partitionuniqueterm")["results"])
        first_page = self.wiki.listing("source", limit=1)
        self.assertTrue(first_page["next_cursor"])
        second_page = self.wiki.listing("source", limit=1, after=first_page["next_cursor"])
        self.assertNotEqual(first_page["items"][0]["id"], second_page["items"][0]["id"])
        for bad in ("../escape", "UpperCase", "x/y"):
            with self.assertRaises(WikiError):
                self.wiki.define_page(bad, "Title", [first["id"]])
        with self.assertRaises(WikiError):
            self.wiki.search("storage", limit=0)

    def test_parallel_ingestion_and_index_statistics(self):
        with ThreadPoolExecutor(6) as pool:
            results = list(pool.map(lambda i: self.add("parallel:" + str(i), "parallel sharedterm unique" + str(i)), range(12)))
        self.assertEqual(12, len({r["id"] for r in results}))
        self.assertEqual(12, self.wiki.status()["search"]["count"])
        self.assertEqual(12, len(self.wiki.search("sharedterm", limit=50)["results"]))
        self.assertTrue(self.wiki.search("sharedterm", posting_budget=2)["truncated"])

    def test_ask_evidence_and_missing_evidence(self):
        self.add(content="FengquDB durable storage supports atomic transactions.")
        self.drain()
        answer = self.wiki.ask("atomic transactions")
        self.assertTrue(answer["citation_validated"])
        self.assertEqual("finished", answer["termination"])
        self.assertEqual("complete", answer["index_coverage"])
        self.assertEqual("exact", answer["ranking_kind"])
        self.assertFalse(answer["stale"])
        self.assertIn("[E1]", answer["answer"])
        self.assertFalse(self.wiki.ask("nonexistentterm") ["citation_validated"])
        class InvalidAnswer(ExtractiveProvider):
            def answer(self, *_args):
                return "Unverifiable [E999]"
        self.wiki.provider = InvalidAnswer()
        with self.assertRaises(ProviderError):
            self.wiki.ask("atomic")

    def test_ask_marks_changed_evidence(self):
        self.add(content="beforeanswerterm")
        wiki = self.wiki
        class Mutating(ExtractiveProvider):
            def answer(self, *args):
                wiki.ingest("test:one", "afteranswerterm", title="Storage")
                return super().answer(*args)
        self.wiki.provider = Mutating()
        self.assertTrue(self.wiki.ask("beforeanswerterm")["stale"])

    def test_raw_reference_like_text_export_and_backup(self):
        source = self.add(content="Raw text [S999] and [[untrusted-link]] must remain data.")
        self.drain()
        exported = self.wiki.export(self.root / "export")
        self.assertEqual(1, exported["pages"])
        manifest = json.loads((self.root / "export/manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(source["id"], manifest["sources"][0]["id"])
        with self.assertRaises(FileExistsError):
            self.wiki.export(self.root / "export")
        self.wiki.backup(str(self.root / "backup"))
        with Wiki(self.root / "backup", library=LIBRARY) as backup:
            self.assertEqual(source["revision"], backup.source(source["id"])["revision"])
            self.assertTrue(backup.lint()["ok"])

    def test_process_exit_preserves_pending_work(self):
        code = "import os; from llmwiki import Wiki; w=Wiki(os.environ['WIKI_TEST_DB'], library=os.environ['WIKI_TEST_LIB']); w.ingest('crash:test','crashrecoveryterm'); os._exit(0)"
        db = self.root / "processdb"
        environment = dict(os.environ, WIKI_TEST_DB=str(db), WIKI_TEST_LIB=LIBRARY)
        subprocess.run([sys.executable, "-c", code], env=environment, check=True, timeout=20)
        with Wiki(db, library=LIBRARY) as recovered:
            self.assertTrue(recovered.search("crashrecoveryterm")["results"])
            self.assertEqual("done", recovered.run_once()["state"])

    def test_native_maintenance_preserves_policy_and_content(self):
        source = self.add()
        self.drain()
        before = self.wiki.repo.store.write_options()
        self.wiki.enable_maintenance()
        after = self.wiki.repo.store.write_options()
        self.assertEqual((before.mode, before.period_ms, before.threshold_bytes),
                         (after.mode, after.period_ms, after.threshold_bytes))
        self.assertGreater(self.wiki.verify()["stored_keys"], 0)
        result = self.wiki.compact()
        self.assertGreater(result["output_file_bytes"], 0)
        self.assertEqual(source["revision"], self.wiki.source(source["id"])["revision"])
        self.assertTrue(self.wiki.search("storage")["results"])

    def test_index_provider_routes_mutation_visibility_and_reads(self):
        self.wiki.close()
        index = PythonIndexProvider()
        with ExitStack() as stack:
            names = ("upsert", "delete", "set_page_state", "get_page_state", "get_document", "statistics", "search")
            calls = {name: stack.enter_context(patch.object(index, name, wraps=getattr(index, name))) for name in names}
            self.wiki = Wiki(self.path, library=LIBRARY, index_provider=index)
            source = self.add()
            self.drain()
            self.assertTrue(self.wiki.ask("storage")["citation_validated"])
            self.assertEqual("python-bm25", self.wiki.status()["index_provider"]["name"])
            self.add(content="replacement storage")
            self.wiki.delete_source(source["id"])
            self.drain()
            for name in names:
                self.assertTrue(calls[name].called, name)
            states = [call.args[2] for call in calls["set_page_state"].call_args_list]
            self.assertIn(None, states)
            self.assertTrue(any(state and state["stale"] for state in states))
            self.assertTrue(any(state and not state["stale"] for state in states))

    def test_index_provider_cannot_silently_claim_another_format(self):
        class Other(PythonIndexProvider):
            definition = IndexDefinition("future-native", 1, 1)
        source = self.add()
        self.wiki.close()
        with self.assertRaises(WikiError):
            Wiki(self.path, library=LIBRARY, index_provider=Other())
        self.wiki = Wiki(self.path, library=LIBRARY)
        self.assertEqual(source["revision"], self.wiki.source(source["id"])["revision"])
        self.assertTrue(self.wiki.search("storage")["results"])

    def test_legacy_python_index_adoption_rejects_foreign_provider(self):
        class Other(PythonIndexProvider):
            definition = IndexDefinition("future-native", 1, 1)
        source = self.add()
        self.wiki.repo.write(lambda tx: tx.delete("index/provider"))
        self.wiki.close()
        with self.assertRaises(WikiError):
            Wiki(self.path, library=LIBRARY, index_provider=Other())
        self.wiki = Wiki(self.path, library=LIBRARY)
        self.assertEqual(source["revision"], self.wiki.source(source["id"])["revision"])
        with self.wiki.repo.snapshot() as view:
            self.assertEqual("python-bm25", view.get("index/provider")["name"])

    def test_async_index_provider_is_rejected_before_opening_store(self):
        class Asynchronous(PythonIndexProvider):
            atomic_updates = False
        unopened = self.root / "unopened"
        with self.assertRaises(WikiError):
            Wiki(unopened, library=LIBRARY, index_provider=Asynchronous())
        self.assertFalse(unopened.exists())

    def test_complete_lexical_empty_result_is_explicitly_confirmed(self):
        self.add()
        result = self.wiki.search("unmatcheduniqueterm")
        self.assertEqual("exact", result["ranking_kind"])
        self.assertEqual("finished", result["termination"])
        self.assertEqual("complete", result["index_coverage"])
        self.assertEqual({"value": 0, "relation": "exact"}, result["total_hits"])
        self.assertTrue(result["empty_result_confirmed"])
        self.assertIsNone(result["snapshot_version"])
        self.assertIsNone(result["applied_sequence"])
        self.assertTrue(self.wiki.ask("unmatcheduniqueterm")["empty_result_confirmed"])

    def test_interrupted_empty_result_is_not_a_confirmed_miss(self):
        class Partial(PythonIndexProvider):
            def search(self, *_args, **_kwargs):
                return {"results": [], "ranking_kind": "approximate", "termination": "candidate_limit",
                        "index_coverage": "complete", "total_hits": {"value": 0, "relation": "lower_bound"},
                        "empty_result_confirmed": True}
        self.wiki.close()
        self.wiki = Wiki(self.path, library=LIBRARY, index_provider=Partial())
        result = self.wiki.ask("storage")
        self.assertFalse(result["empty_result_confirmed"])
        self.assertTrue(result["truncated"])
        self.assertEqual("candidate_limit", result["termination"])
        self.assertIn("has not been confirmed", result["answer"])

    def test_approximate_finished_empty_result_is_not_truncation_or_proof(self):
        class Approximate(PythonIndexProvider):
            def search(self, *_args, **_kwargs):
                return {"results": [], "ranking_kind": "approximate", "termination": "finished",
                        "index_coverage": "complete", "total_hits": {"value": None, "relation": "unknown"}}
        self.wiki.close()
        self.wiki = Wiki(self.path, library=LIBRARY, index_provider=Approximate())
        result = self.wiki.ask("storage")
        self.assertFalse(result["empty_result_confirmed"])
        self.assertFalse(result["truncated"])
        self.assertEqual("finished", result["termination"])
        self.assertEqual("approximate", result["ranking_kind"])
        self.assertIn("has not been confirmed", result["answer"])

    def test_legacy_empty_provider_has_unknown_coverage(self):
        class Legacy(PythonIndexProvider):
            def search(self, *_args, **_kwargs):
                return {"results": [], "truncated": False}
        self.wiki.close()
        self.wiki = Wiki(self.path, library=LIBRARY, index_provider=Legacy())
        result = self.wiki.search("storage")
        self.assertEqual("unknown", result["termination"])
        self.assertEqual("unknown", result["index_coverage"])
        self.assertTrue(result["truncated"])
        self.assertFalse(result["empty_result_confirmed"])

    def test_http_auth_validation_and_routes(self):
        token = "test-secret-with-at-least-24-characters"
        with self.assertRaises(WikiError):
            WikiServer(("0.0.0.0", 0), self.wiki, background=False)
        with WikiServer(("127.0.0.1", 0), self.wiki, token=token, background=False) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            base = "http://127.0.0.1:" + str(server.server_port)
            def call(path, data=None, auth=True, method=None):
                headers = {"Content-Type": "application/json"}
                if auth:
                    headers["Authorization"] = "Bearer " + token
                request = urllib.request.Request(base + path, data=json.dumps(data).encode() if data is not None else None,
                                                 headers=headers, method=method)
                with urllib.request.urlopen(request, timeout=5) as response:
                    return json.load(response)
            try:
                self.assertTrue(call("/health", auth=False)["ok"])
                with self.assertRaises(urllib.error.HTTPError) as error:
                    call("/v1/status", auth=False)
                self.assertEqual(401, error.exception.code)
                error.exception.close()
                source = call("/v1/sources", {"uri": "api:test", "content": "apiuniqueterm"})
                self.assertTrue(call("/v1/search?q=apiuniqueterm")["results"])
                self.assertEqual(source["id"], call("/v1/sources/" + source["id"])["source_id"])
                with self.assertRaises(urllib.error.HTTPError) as error:
                    call("/v1/pages", {"id": "../bad"})
                self.assertEqual(400, error.exception.code)
                error.exception.close()
                self.assertTrue(call("/v1/sources/" + source["id"], method="DELETE")["deleted"])
            finally:
                server.shutdown()
                thread.join(5)


class TransportTests(unittest.TestCase):
    def test_compatible_wire_contract_and_redacted_failures(self):
        captured = []
        class Fixture(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass
            def do_POST(self):
                captured.append((self.path, self.headers.get("Authorization"), json.loads(self.rfile.read(int(self.headers["Content-Length"])))))
                data = json.dumps({"choices": [{"message": {"content": json.dumps({"markdown": "Evidence [S1]", "used_sources": ["S1"], "links": []})}}]}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
        with ThreadingHTTPServer(("127.0.0.1", 0), Fixture) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                provider = CompatibleProvider("http://127.0.0.1:" + str(server.server_port) + "/v1", "local-model", api_key="fixture-key")
                self.assertEqual(["S1"], provider.compile("Title", [{"label": "S1", "content": "untrusted content"}], [])["used_sources"])
                self.assertEqual("/v1/chat/completions", captured[0][0])
                self.assertEqual("Bearer fixture-key", captured[0][1])
                self.assertEqual("local-model", captured[0][2]["model"])
                self.assertIn("untrusted", captured[0][2]["messages"][0]["content"])
            finally:
                server.shutdown()
                thread.join(5)
        provider = CompatibleProvider("http://127.0.0.1:1/v1", "local", api_key="secret", timeout=1)
        with self.assertRaises(ProviderError) as error:
            provider.answer("question", [])
        self.assertNotIn("secret", str(error.exception))


if __name__ == "__main__":
    unittest.main()
