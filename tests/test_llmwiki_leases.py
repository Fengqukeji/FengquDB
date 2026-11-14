"""Deterministic lease faults against the real Store/Index, without extending TTLs."""

import os
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import patch

from llmwiki import Wiki
from llmwiki.storage import Error
from llmwiki.wiki import now_ms


class LeaseTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.wiki = Wiki(Path(self.directory.name) / "db", library=os.environ.get("FENGQUDB_LIBRARY"))
        self.source = self.wiki.ingest("lease:source", "durable lease evidence")
        self.base = now_ms() + 1000
        self.clock = [self.base]
        self.clock_patch = patch("llmwiki.wiki.now_ms", side_effect=lambda: self.clock[0])
        self.clock_patch.start()

    def tearDown(self):
        self.clock_patch.stop()
        self.wiki.close()
        self.directory.cleanup()

    def test_claim_deadline_is_not_backdated_by_writer_admission(self):
        write = self.wiki.repo.write
        def delayed(operation):
            self.clock[0] += 200
            return write(operation)
        with patch.object(self.wiki.repo, "write", side_effect=delayed):
            job = self.wiki._claim(150)
        self.assertEqual(150, job["due_ms"] - self.clock[0])
        self.assertEqual(1, job["attempts"])

    def test_late_renewal_and_takeover_use_the_same_token_fence(self):
        job = self.wiki._claim(150)
        self.clock[0] += 300
        self.assertTrue(self.wiki._renew(job, 150))
        self.assertIsNone(self.wiki._claim(150))
        self.clock[0] += 151
        replacement = self.wiki._claim(150)
        self.assertEqual(2, replacement["attempts"])
        self.assertNotEqual(job["token"], replacement["token"])
        self.assertFalse(self.wiki._renew(job, 150))
        with self.wiki.repo.snapshot() as view:
            current = view.get("job/" + job["page_id"])
        self.assertEqual(replacement["token"], current["token"])

    def test_heartbeat_retries_a_transient_write_failure(self):
        job = self.wiki._claim(150)
        renew, calls, recovered = self.wiki._renew, [0], threading.Event()
        def transient(owned, lease_ms):
            calls[0] += 1
            if calls[0] == 1:
                raise Error(17, "injected renewal timeout")
            result = renew(owned, lease_ms)
            if result:
                recovered.set()
            return result
        with patch.object(self.wiki, "_renew", side_effect=transient), self.wiki._heartbeat(job, 150):
            self.assertTrue(recovered.wait(5))
            self.assertIsNone(self.wiki._claim(150))
        self.assertGreaterEqual(calls[0], 2)

    def test_heartbeat_covers_preparation_before_the_model_call(self):
        preparing, resume, renewed = threading.Event(), threading.Event(), threading.Event()
        sources, renew, first = self.wiki._sources, self.wiki._renew, [True]
        def delayed_sources(view, spec):
            if first[0]:
                first[0] = False
                preparing.set()
                if not resume.wait(5):
                    raise RuntimeError("preparation test did not resume")
            return sources(view, spec)
        def observe(job, lease_ms):
            started = self.clock[0]
            result = renew(job, lease_ms)
            if result and started >= self.base + 300:
                renewed.set()
            return result
        with patch.object(self.wiki, "_sources", side_effect=delayed_sources), \
                patch.object(self.wiki, "_renew", side_effect=observe), ThreadPoolExecutor(1) as pool:
            future = pool.submit(self.wiki.run_once, lease_ms=150)
            try:
                self.assertTrue(preparing.wait(5))
                self.clock[0] += 300
                self.assertTrue(renewed.wait(5))
                self.assertIsNone(self.wiki._claim(150))
            finally:
                resume.set()
            self.assertEqual("done", future.result(5)["state"])
