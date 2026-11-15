"""Transactional source history, dependency graph and recoverable compilation."""

import hashlib
import re
import time
import uuid
import threading
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from dataclasses import asdict
from pathlib import Path

from .text import chunks
from .index_provider import (IndexDefinition,
                             normalize_search_result, search_quality)
from .native_index_provider import NativeIndexProvider
from .providers import ExtractiveProvider, PROMPT_VERSION, ProviderError
from .storage import Error, Repository, canonical, encode
from .limits import WikiError, ConflictError, BudgetError, Limits
from .knowledge import KnowledgeMixin, validate_knowledge

SCHEMA_VERSION = 2


def digest(value):
    return hashlib.sha256(value if isinstance(value, bytes) else value.encode()).hexdigest()


def now_ms():
    return time.time_ns() // 1000000


def text(value, name, maximum=512):
    if not isinstance(value, str) or not value.strip() or len(value.encode()) > maximum or "\0" in value:
        raise WikiError("invalid " + name)
    return value


def slug(value):
    if not isinstance(value, str) or not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,95}", value):
        raise WikiError("page id must contain 1..96 lowercase ASCII letters, digits, hyphens or underscores")
    return value


def source_id(value):
    if not isinstance(value, str) or not re.fullmatch(r"[a-f0-9]{64}", value):
        raise WikiError("invalid source id")
    return value


def bounded_limit(value, maximum=200):
    if type(value) is not int or not 1 <= value <= maximum:
        raise WikiError("limit must be between 1 and " + str(maximum))
    return value


class Wiki(KnowledgeMixin):
    _hash_id = staticmethod(source_id)

    def __init__(self, path, *, library=None, namespace="default", provider=None, index_provider=None,
                 compiler=None, answerer=None, limits=None):
        text(namespace, "namespace", 128)
        self.index = index_provider if index_provider is not None else NativeIndexProvider()
        if getattr(self.index, "atomic_updates", None) is not True:
            raise WikiError("this wiki requires an index provider with same-transaction atomic updates")
        if not isinstance(getattr(self.index, "definition", None), IndexDefinition):
            raise WikiError("index provider requires an IndexDefinition")
        self.index_definition = self.index.definition.descriptor()
        self.repo = Repository(path, "wiki/" + digest(namespace)[:32] + "/", library)
        self.provider = provider or ExtractiveProvider()
        self.compiler, self.answerer = compiler, answerer
        self.limits = limits or Limits()
        if not isinstance(self.limits, Limits):
            self.repo.close()
            raise WikiError("limits must be Limits")
        self._stopping = threading.Event()
        self._activity = threading.Condition()
        self._active = 0
        self._cache_lock = threading.Lock()
        self._answer_cache = OrderedDict()
        self.namespace = namespace
        try:
            def initialize(tx):
                schema = tx.get("schema")
                expected = {"version": SCHEMA_VERSION, "tokenizer": self.index.definition.tokenizer_version}
                if schema is not None and schema != expected:
                    raise WikiError("unsupported wiki schema/tokenizer; use a fresh namespace (no migration supplied)")
                if schema is None:
                    tx.put("schema", expected)
                installed = tx.get("index/provider")
                if installed is not None and installed != self.index_definition:
                    raise WikiError("index provider changed; use a fresh namespace")
                if installed is None:
                    if schema is not None:
                        raise WikiError("missing index provider identity; use a fresh namespace")
                    tx.put("index/provider", self.index_definition)
            self.repo.write(initialize)
        except BaseException:
            self.repo.close()
            raise

    def close(self):
        self._stopping.set()
        with self._activity:
            self._activity.wait_for(lambda: self._active == 0)
        self.repo.close()

    @contextmanager
    def _operation(self):
        with self._activity:
            if self._stopping.is_set():
                raise WikiError("wiki is closing")
            self._active += 1
        try:
            yield
        finally:
            with self._activity:
                self._active -= 1
                self._activity.notify_all()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    @staticmethod
    def _audit(tx, action, entity):
        sequence = tx.get("audit/sequence", 0) + 1
        tx.put("audit/sequence", sequence)
        tx.put("event/%016x" % sequence, {"time_ms": now_ms(), "action": action, "entity": entity})
        tx.put("runtime/knowledge-epoch", tx.get("runtime/knowledge-epoch", 0) + 1)

    @staticmethod
    def _queue_key(job):
        return "queue/%016x/%s" % (job["due_ms"], job["page_id"])

    def _schedule(self, tx, page_id):
        scheduled = getattr(tx, "_wiki_scheduled", None)
        if scheduled is None:
            scheduled = tx._wiki_scheduled = set()
        if page_id in scheduled:
            return
        scheduled.add(page_id)
        old = tx.get("job/" + page_id)
        if old and old["state"] in ("pending", "running"):
            tx.delete(self._queue_key(old))
        generation = (old or {}).get("generation", 0) + 1
        job = {"page_id": page_id, "state": "pending", "attempts": 0, "generation": generation,
               "due_ms": now_ms(), "token": None, "error": None}
        spec = tx.get("spec/" + page_id, {})
        if spec.get("mode") == "human":
            extract = spec.get("kind") == "source" and any(
                tx.get("source/" + sid, {}).get("knowledge_pending") for sid in spec["source_ids"])
            if not extract:
                job["state"] = "review_required"
        self._set_job(tx, job)
        if job["state"] == "pending":
            tx.put(self._queue_key(job), page_id)
        page = tx.get("page/" + page_id)
        if page:
            if not page["stale"]:
                page["stale"] = True
                tx.put("page/" + page_id, page)
            self.index.set_page_state(tx, page_id, {"stale": True, "revision": page["revision"],
                                                   "build_generation": generation})

    @staticmethod
    def _set_job(tx, job):
        previous = tx.get("job/" + job["page_id"])
        counts = tx.get("runtime/job-counts", {})
        if previous:
            counts[previous["state"]] = counts.get(previous["state"], 0) - 1
        counts[job["state"]] = counts.get(job["state"], 0) + 1
        tx.put("runtime/job-counts", {k: v for k, v in counts.items() if v})
        tx.put("job/" + job["page_id"], job)

    def _define(self, tx, page_id, title, source_ids, links, kind):
        old = tx.get("spec/" + page_id)
        if old:
            for sid in old["source_ids"]:
                tx.delete("dep/" + sid + "/" + page_id)
        spec = {"id": page_id, "title": title, "source_ids": source_ids, "links": links,
                "kind": kind, "generation": (old or {}).get("generation", 0) + 1,
                "mode": (old or {}).get("mode", "compiled")}
        for sid in source_ids:
            tx.put("dep/" + sid + "/" + page_id, True)
        tx.put("spec/" + page_id, spec)
        self._schedule(tx, page_id)
        return spec

    def _prepare_source(self, uri, content, *, title=None, annotations=None,
                        expected_version=None, expected_revision=None):
        text(uri, "uri", 2048)
        if isinstance(content, str) and len(content.encode()) > self.limits.source_bytes:
            raise BudgetError("source byte quota exceeded")
        text(content, "source content", self.limits.source_bytes)
        title = text(title or uri, "title", 512)
        sid = digest(uri)
        revision = digest(canonical({"uri": uri, "title": title, "content": content, "annotations": annotations}))
        if expected_version is not None and (type(expected_version) is not int or expected_version < 0):
            raise WikiError("expected_version must be a nonnegative integer")
        if expected_revision is not None:
            source_id(expected_revision)
        # Validate before admission as well as when binding the actual source version.
        if annotations is not None:
            validate_knowledge(annotations, [{"label": "S1", "source_id": sid, "revision": revision,
                "version": 0, "content": content}], self.limits)
        return {"uri": uri, "content": content, "title": title, "id": sid, "revision": revision,
                "annotations": annotations, "expected_version": expected_version, "expected_revision": expected_revision}

    @staticmethod
    def _check_source_version(previous, expected_version, expected_revision):
        if expected_version is not None and (type(expected_version) is not int or expected_version < 0):
            raise WikiError("expected_version must be a nonnegative integer")
        if expected_revision is not None:
            source_id(expected_revision)
        if expected_version is not None and (previous or {}).get("version", 0) != expected_version:
            raise ConflictError("source version changed")
        if expected_revision is not None and (previous or {}).get("revision") != expected_revision:
            raise ConflictError("source revision changed")

    def _ingest(self, tx, prepared):
        uri, content, title, sid, revision = (prepared[k] for k in ("uri", "content", "title", "id", "revision"))
        annotations = prepared["annotations"]
        previous = tx.get("source/" + sid)
        self._check_source_version(previous, prepared["expected_version"], prepared["expected_revision"])
        if previous and not previous["deleted"] and previous["revision"] == revision:
            return dict(previous, changed=False)
        planned = list(chunks(content))
        new_ids = {"c-" + sid + "-%04x" % index for index in range(len(planned))}
        for doc_id in (previous or {}).get("chunks", []):
            if doc_id not in new_ids:
                self.index.delete(tx, doc_id)
        chunk_ids = []
        publication_generation = (previous or {}).get("version", 0) + 1
        for index, (start, end, body) in enumerate(planned):
            doc_id = "c-" + sid + "-%04x" % index
            chunk_ids.append(doc_id)
            self.index.upsert(tx, doc_id, {"kind": "chunk", "source_id": sid, "revision": revision,
                "title": title, "text": body, "source_publication_generation": publication_generation,
                "citations": [{"source_id": sid, "revision": revision, "source_version": publication_generation,
                "start": start, "end": end, "uri": uri, "title": title}]})
        source = {"id": sid, "uri": uri, "title": title, "revision": revision,
                  "content_hash": digest(content), "deleted": False, "updated_ms": now_ms(), "chunks": chunk_ids,
                  "publication_generation": publication_generation, "version": publication_generation,
                  "knowledge_pending": annotations is None, "annotations": annotations is not None}
        # The same source+revision key always has exactly the same immutable value.
        raw_key = "raw/" + sid + "/" + revision
        if tx.get(raw_key) is None:
            tx.put(raw_key, {"source_id": sid, "revision": revision, "uri": uri, "title": title, "content": content,
                             "annotations": annotations})
        tx.put("source/" + sid, source)
        tx.put("source-history/" + sid + "/%016x" % source["version"], source)
        self._invalidate_knowledge(tx, sid)
        if annotations is not None:
            knowledge = validate_knowledge(annotations, [{"label": "S1", "source_id": sid,
                "revision": revision, "version": publication_generation, "content": content}], self.limits)
            self._replace_knowledge(tx, sid, knowledge)
        own_page = "source-" + sid[:24]
        self._define(tx, own_page, title, [sid], [], "source")
        for key, _ in list(tx.scan("dep/" + sid + "/")):
            page_id = key.rsplit("/", 1)[1]
            if page_id != own_page:
                self._schedule(tx, page_id)
        self._audit(tx, "source.upsert", sid)
        return dict(source, changed=True)

    def ingest(self, uri, content, *, title=None, annotations=None, expected_version=None, expected_revision=None):
        prepared = self._prepare_source(uri, content, title=title, annotations=annotations,
            expected_version=expected_version, expected_revision=expected_revision)
        return self.repo.write(lambda tx: self._ingest(tx, prepared))

    def _idempotent(self, tx, request_key, fingerprint, operation):
        if request_key is None:
            return operation()
        text(request_key, "request key", 128)
        key = "request/" + digest(request_key)
        existing = tx.get(key)
        if existing:
            if existing["digest"] != fingerprint:
                raise ConflictError("request key was already used with different input")
            return existing["result"]
        result = operation()
        tx.put(key, {"digest": fingerprint, "result": result})
        return result

    def ingest_many(self, items, *, request_key=None):
        if not isinstance(items, list) or not items or len(items) > self.limits.batch_items:
            raise BudgetError("batch item quota exceeded")
        if len(encode(items)) > self.limits.batch_bytes:
            raise BudgetError("batch byte quota exceeded")
        allowed = {"uri", "content", "title", "annotations", "expected_version", "expected_revision"}
        if any(not isinstance(item, dict) or set(item) - allowed for item in items):
            raise WikiError("invalid source batch item")
        prepared = [self._prepare_source(**item) for item in items]
        if len({item["id"] for item in prepared}) != len(prepared):
            raise WikiError("duplicate source URI in batch")
        return self.repo.write(lambda tx: self._idempotent(tx, request_key, digest(canonical(["ingest", prepared])),
            lambda: {"items": [self._ingest(tx, item) for item in prepared]}))

    def define_page(self, page_id, title, source_ids, *, links=None):
        slug(page_id)
        if page_id.startswith(("source-", "entity-")):
            raise WikiError("source- and entity- prefixes are reserved")
        text(title, "title", 512)
        if not isinstance(source_ids, list) or not 1 <= len(source_ids) <= self.limits.sources_per_page:
            raise BudgetError("page source quota exceeded")
        source_ids = sorted(set(source_id(sid) for sid in source_ids))
        if links is not None and (not isinstance(links, list) or len(links) > 64):
            raise WikiError("links must be a list of at most 64 page ids")
        links = sorted(set(slug(link) for link in (links or [])))
        if page_id in links:
            raise WikiError("a page cannot link to itself")

        def operation(tx):
            for sid in source_ids:
                source = tx.get("source/" + sid)
                if not source or source["deleted"]:
                    raise WikiError("source is missing or deleted: " + sid)
            spec = self._define(tx, page_id, title, source_ids, links, "topic")
            self._audit(tx, "page.define", page_id)
            return spec
        return self.repo.write(operation)

    def delete_source(self, sid, *, expected_version=None, expected_revision=None):
        source_id(sid)
        return self.repo.write(lambda tx: self._delete_source(tx, sid, expected_version, expected_revision))

    def _delete_source(self, tx, sid, expected_version=None, expected_revision=None):
        source = tx.get("source/" + sid)
        self._check_source_version(source, expected_version, expected_revision)
        if not source or source["deleted"]:
            return False
        for doc_id in source["chunks"]:
            self.index.delete(tx, doc_id)
        source.update(deleted=True, chunks=[], updated_ms=now_ms(), version=source["version"] + 1,
                      publication_generation=source["version"] + 1, knowledge_pending=False)
        tx.put("source/" + sid, source)
        tx.put("source-history/" + sid + "/%016x" % source["version"], source)
        self._invalidate_knowledge(tx, sid, deleted=True)
        for key, _ in list(tx.scan("dep/" + sid + "/")):
            self._schedule(tx, key.rsplit("/", 1)[1])
        self._audit(tx, "source.delete", sid)
        return True

    def delete_sources(self, items, *, request_key=None):
        if not isinstance(items, list) or not items or len(items) > self.limits.batch_items:
            raise BudgetError("batch item quota exceeded")
        if any(not isinstance(i, dict) or set(i) - {"id", "expected_version", "expected_revision"} for i in items):
            raise WikiError("invalid delete batch item")
        ids = [source_id(i.get("id")) for i in items]
        if len(set(ids)) != len(ids):
            raise WikiError("duplicate source id")
        return self.repo.write(lambda tx: self._idempotent(tx, request_key, digest(canonical(["delete", items])),
            lambda: {"items": [{"id": i["id"], "deleted": self._delete_source(tx, i["id"],
                i.get("expected_version"), i.get("expected_revision"))} for i in items]}))

    def source(self, sid, *, revision=None, version=None):
        source_id(sid)
        with self.repo.snapshot() as view:
            current = view.get("source/" + sid)
            if current is None:
                return None
            if version is not None and (type(version) is not int or version < 1):
                raise WikiError("invalid source version")
            selected = current if version is None else view.get("source-history/" + sid + "/%016x" % version)
            if selected is None:
                return None
            if version is not None and revision is not None and selected["revision"] != revision:
                raise WikiError("source revision/version disagree")
            revision = revision or selected["revision"]
            if not isinstance(revision, str) or not re.fullmatch(r"[a-f0-9]{64}", revision):
                raise WikiError("invalid revision")
            raw = view.get("raw/" + sid + "/" + revision)
            return dict(raw, deleted=selected["deleted"], current_revision=current["revision"], current_version=current["version"],
                        version=selected["version"], knowledge_pending=selected["knowledge_pending"]) if raw else None

    def page(self, page_id, *, revision=None):
        slug(page_id)
        if revision is not None and (not isinstance(revision, str) or not re.fullmatch(r"[a-f0-9]{64}", revision)):
            raise WikiError("invalid revision")
        with self.repo.snapshot() as view:
            page = view.get("history/" + page_id + "/" + revision if revision else "page/" + page_id)
            if page:
                page["backlinks"] = ([key.rsplit("/", 1)[1] for key, _ in view.scan("back/" + page_id + "/")]
                                     if revision is None else [])
                page["archived"] = revision is not None
            return page

    def listing(self, kind, *, limit=100, after=""):
        if kind not in ("source", "page", "job", "spec", "event", "entity", "claim", "conflict"):
            raise WikiError("invalid collection")
        bounded_limit(limit)
        prefix = kind + "/"
        if not isinstance(after, str) or (after and (not after.startswith(prefix) or len(after) > 256)):
            raise WikiError("invalid cursor")
        with self.repo.snapshot() as view:
            rows = list(view.scan(prefix, after=after, limit=limit + 1))
            return {"items": [value for _, value in rows[:limit]],
                    "next_cursor": rows[limit - 1][0] if len(rows) > limit else None}

    def search(self, query, *, limit=10, posting_budget=12000, candidate_budget=1024, **options):
        if not (query == "" and (options.get("mode") in (1, 2) or options.get("phrases"))):
            text(query, "query", 2048)
        bounded_limit(limit, 50)
        bounded_limit(posting_budget, 100000)
        bounded_limit(candidate_budget, 4096)
        with self.repo.snapshot() as view:
            return normalize_search_result(self.index.search(view, query, limit=limit,
                posting_budget=posting_budget, candidate_budget=candidate_budget, **options))

    def _claim(self, lease_ms):
        def operation(tx):
            # Admission/lock waits and transaction retries must not consume a
            # lease whose owner has not received the claim yet.
            stamp = now_ms()
            # Due-time ordered durable queue; no scan of completed job history.
            for key, page_id in tx.scan("queue/", limit=64):
                if int(key.split("/")[1], 16) > stamp:
                    return None
                job = tx.get("job/" + page_id)
                tx.delete(key)
                if not job or job["state"] not in ("pending", "running") or self._queue_key(job) != key:
                    continue
                if job["attempts"] >= self.limits.attempts:
                    job.update(state="failed", token=None, error="attempt budget exhausted")
                    self._set_job(tx, job)
                    continue
                job.update(state="running", token=uuid.uuid4().hex, due_ms=now_ms() + lease_ms,
                           attempts=job["attempts"] + 1)
                self._set_job(tx, job)
                tx.put(self._queue_key(job), page_id)
                return job
            return None
        return self.repo.write(operation)

    @staticmethod
    def _sources(view, spec):
        sources = []
        for index, sid in enumerate(spec["source_ids"], 1):
            source = view.get("source/" + sid)
            if not source or source["deleted"]:
                continue
            raw = view.get("raw/" + sid + "/" + source["revision"])
            sources.append(dict(raw, label="S" + str(index), version=source["version"],
                                knowledge_pending=source["knowledge_pending"]))
        return sources

    def _validate_artifact(self, artifact, sources, links):
        if not isinstance(artifact, dict):
            raise ProviderError("compile output must be an object")
        markdown, used, actual_links = artifact.get("markdown"), artifact.get("used_sources"), artifact.get("links")
        if not isinstance(markdown, str) or not markdown.strip() or len(markdown.encode()) > self.limits.page_bytes:
            raise ProviderError("invalid or oversized compiled Markdown")
        allowed = {s["label"] for s in sources}
        if not isinstance(used, list) or not used or any(not isinstance(item, str) or item not in allowed for item in used):
            raise ProviderError("compile output contains missing or unknown source references")
        # Quoted raw material may itself contain reference-like text or wiki links.
        authored = re.sub(r"(?m)^>.*$", "", markdown)
        references = set(re.findall(r"\[(S\d+)\]", authored))
        if references != set(used):
            raise ProviderError("inline source references and used_sources must agree")
        if not isinstance(actual_links, list) or any(not isinstance(link, str) or link not in links for link in actual_links):
            raise ProviderError("compile output contains unknown page links")
        body_links = set(re.findall(r"\[\[([^\]\n]+)\]\]", authored))
        if body_links != set(actual_links):
            raise ProviderError("inline wiki links and links metadata must agree")
        return {"markdown": markdown, "used_sources": sorted(set(used)), "links": sorted(set(actual_links)),
                "entities": artifact.get("entities", []), "claims": artifact.get("claims", [])}

    def run_once(self, *, lease_ms=180000):
        with self._operation():
            return self._run_once(lease_ms=lease_ms)

    def _renew(self, job, lease_ms):
        def operation(tx):
            current = tx.get("job/" + job["page_id"])
            # Expiry permits a competing claim; it does not itself replace the
            # token. A delayed heartbeat can renew until a claimant wins this
            # same transactional fence, just like publication checks below.
            if not current or current["token"] != job["token"] or current["state"] != "running":
                return False
            tx.delete(self._queue_key(current))
            current["due_ms"] = now_ms() + lease_ms
            self._set_job(tx, current)
            tx.put(self._queue_key(current), current["page_id"])
            return True
        return self.repo.write(operation)

    @contextmanager
    def _heartbeat(self, job, lease_ms):
        finished = threading.Event()
        interval = max(0.01, min(30, lease_ms / 3000))
        def renew():
            # Start earlier if durable claim admission already used part of
            # the lease. Fast jobs still avoid an unnecessary renewal write.
            delay = min(interval, max(0, (job["due_ms"] - now_ms()) / 3000))
            while not finished.wait(delay):
                try:
                    if not self._renew(job, lease_ms):
                        return
                except Error as exc:
                    # A transient write failure does not prove token loss.
                    # Retry at the bounded heartbeat cadence; a replacement
                    # token will make the next successful check return False.
                    if exc.code not in (4, 7, 17):  # I/O, conflict, timeout
                        return
                except Exception:
                    return
                delay = interval
        thread = threading.Thread(target=renew, name="wiki-lease", daemon=True)
        thread.start()
        try:
            yield
        finally:
            finished.set()
            thread.join()

    def _run_once(self, *, lease_ms):
        if type(lease_ms) is not int or not 1 <= lease_ms <= 3600000:
            raise WikiError("lease_ms must be between 1 and 3600000")
        job = self._claim(lease_ms)
        if not job:
            return None
        with self._heartbeat(job, lease_ms):
            return self._execute_job(job)

    def _execute_job(self, job):
        page_id = job["page_id"]
        compiler = self.compiler or self.provider
        try:
            with self.repo.snapshot() as view:
                spec = view.get("spec/" + page_id)
                if spec is None:
                    raise WikiError("missing page specification")
                sources = self._sources(view, spec)
                waiting = spec["kind"] == "entity" and any(s["knowledge_pending"] for s in sources)
                context = self._entity_context(view, spec)
                fingerprint = digest(encode({"spec": spec, "sources": [(s["source_id"], s["revision"], s["version"]) for s in sources],
                                             "provider": compiler.identity, "prompt": PROMPT_VERSION, "knowledge": context}))
                artifact = view.get("artifact/" + fingerprint)
                artifact_loaded = artifact is not None
            if waiting:
                def defer(tx):
                    current = tx.get("job/" + page_id)
                    if not current or current["token"] != job["token"]:
                        return {"page_id": page_id, "state": "superseded"}
                    tx.delete(self._queue_key(current))
                    current.update(state="waiting_sources", token=None, error=None)
                    self._set_job(tx, current)
                    return {"page_id": page_id, "state": "waiting_sources"}
                return self.repo.write(defer)
            if len(encode(sources)) + len(encode(context)) > self.limits.compile_context_bytes:
                raise BudgetError("compile context byte quota exceeded; split the page or sources")
            knowledge = None
            if sources:
                if artifact is None:
                    model_sources = [{k: v for k, v in s.items() if k != "knowledge_pending"} for s in sources]
                    if context:
                        model_sources[0]["verified_knowledge"] = context
                    artifact = compiler.compile(spec["title"], model_sources, spec["links"])
                    artifact = self._validate_artifact(artifact, sources, spec["links"])
                else:
                    artifact = self._validate_artifact(artifact, sources, spec["links"])
                if spec["kind"] == "source":
                    try:
                        knowledge = validate_knowledge({"entities": artifact["entities"], "claims": artifact["claims"]}, sources, self.limits)
                    except WikiError as exc:
                        raise ProviderError(str(exc)) from None
                # Cache the provider artifact before appending deterministic graph sections.
                if not artifact_loaded:
                    def cache(tx):
                        existing = tx.get("artifact/" + fingerprint)
                        if existing is not None:
                            return existing
                        tx.put("artifact/" + fingerprint, artifact)
                        return artifact
                    artifact = self.repo.write(cache)
                    artifact = self._validate_artifact(artifact, sources, spec["links"])
                    if spec["kind"] == "source":
                        try:
                            knowledge = validate_knowledge({"entities": artifact["entities"], "claims": artifact["claims"]}, sources, self.limits)
                        except WikiError as exc:
                            raise ProviderError(str(exc)) from None
                if context:
                    labels = {s["source_id"]: s["label"] for s in sources}
                    sections = [artifact["markdown"], "## Verified claims"]
                    for claim in context["claims"]:
                        label = labels[claim["source_id"]]
                        sections.append("- Evidence [%s]\n%s" % (label, "\n".join(
                            "> " + line for line in (claim["predicate"] + ": " + str(claim["value"])).splitlines())))
                        artifact["used_sources"].append(label)
                    for conflict in context["conflicts"]:
                        sections.append("- Conflict %s (%s)\n%s" % (conflict["id"], conflict["state"],
                            "\n".join("> " + line for line in conflict["predicate"].splitlines())))
                        if conflict["resolution"]:
                            sections.append("Human decision:\n" + "\n".join("> " + line for line in conflict["resolution"]["note"].splitlines()))
                    artifact = self._validate_artifact(dict(artifact, markdown="\n\n".join(sections)), sources, spec["links"])

            def publish(tx):
                current = tx.get("job/" + page_id)
                if not current or current["token"] != job["token"] or current["state"] != "running":
                    return {"page_id": page_id, "state": "superseded"}
                latest_spec = tx.get("spec/" + page_id)
                latest_sources = self._sources(tx, latest_spec)
                if latest_spec != spec or [(s["source_id"], s["revision"], s["version"]) for s in latest_sources] != [
                        (s["source_id"], s["revision"], s["version"]) for s in sources] or self._entity_context(tx, spec) != context:
                    self._schedule(tx, page_id)
                    return {"page_id": page_id, "state": "superseded"}
                old = tx.get("page/" + page_id)
                if spec.get("mode") == "human":
                    if sources and spec["kind"] == "source" and not tx.get("source/" + sources[0]["source_id"])["annotations"]:
                        self._replace_knowledge(tx, sources[0]["source_id"], knowledge)
                    tx.delete(self._queue_key(current))
                    state = "review_required" if not old or old["stale"] else "done"
                    current.update(state=state, token=None, error=None)
                    self._set_job(tx, current)
                    self._audit(tx, "page.review_required", page_id)
                    return {"page_id": page_id, "state": state}
                if old:
                    for link in old["links"]:
                        tx.delete("back/" + link + "/" + page_id)
                if not sources:
                    self.index.delete(tx, "p-" + page_id)
                    tx.delete("page/" + page_id)
                    self.index.set_page_state(tx, page_id, None)
                    state = "retired"
                else:
                    citations = [{"label": s["label"], "source_id": s["source_id"], "revision": s["revision"],
                                  "source_version": s["version"],
                                  "uri": s["uri"], "title": s["title"], "start": 0, "end": len(s["content"])}
                                 for s in sources if s["label"] in artifact["used_sources"]]
                    page = {"id": page_id, "title": spec["title"], "kind": spec["kind"],
                            "markdown": artifact["markdown"], "citations": citations, "links": artifact["links"],
                            "revision": fingerprint, "stale": False, "updated_ms": now_ms(),
                            "provider": compiler.identity, "missing_sources": len(spec["source_ids"]) - len(sources),
                            "version": (old or {}).get("version", 0) + 1, "mode": "compiled", "knowledge": context}
                    tx.put("page/" + page_id, page)
                    self.index.set_page_state(tx, page_id, {"stale": False, "revision": fingerprint,
                                                           "build_generation": current["generation"]})
                    history_key = "history/" + page_id + "/" + fingerprint
                    if tx.get(history_key) is None:
                        tx.put(history_key, page)
                    for link in page["links"]:
                        tx.put("back/" + link + "/" + page_id, True)
                    self.index.upsert(tx, "p-" + page_id, {"kind": "page", "page_id": page_id,
                        "title": page["title"], "text": page["markdown"], "revision": fingerprint, "citations": citations})
                    state = "done"
                    if spec["kind"] == "source":
                        sid = sources[0]["source_id"]
                        if not tx.get("source/" + sid)["annotations"]:
                            self._replace_knowledge(tx, sid, knowledge)
                tx.delete(self._queue_key(current))
                current.update(state=state, token=None, error=None)
                self._set_job(tx, current)
                self._audit(tx, "page." + state, page_id)
                return {"page_id": page_id, "state": state}
            return self.repo.write(publish)
        except Exception as exc:
            # A lost/unknown durable commit is not retried blindly here. The lease is recoverable.
            def fail(tx):
                current = tx.get("job/" + page_id)
                if not current or current["token"] != job["token"] or current["state"] != "running":
                    return {"page_id": page_id, "state": "superseded"}
                tx.delete(self._queue_key(current))
                state = "failed" if current["attempts"] >= self.limits.attempts else "pending"
                current.update(state=state, token=None, error=type(exc).__name__,
                               failure_category=("budget" if isinstance(exc, BudgetError) else "provider" if isinstance(exc, ProviderError) else "storage_or_internal"),
                               recovery="split input or adjust quota" if isinstance(exc, BudgetError) else "check provider or storage; retry explicitly",
                               due_ms=now_ms() + self.limits.backoff_ms * 2 ** current["attempts"])
                self._set_job(tx, current)
                if state == "pending":
                    tx.put(self._queue_key(current), page_id)
                self._audit(tx, "job.error", page_id)
                return {"page_id": page_id, "state": state, "error": current["error"]}
            return self.repo.write(fail)

    def run_jobs(self, *, limit=100, workers=1, timeout_ms=60000, cancelled=None):
        bounded_limit(limit, self.limits.jobs_per_run)
        bounded_limit(workers, self.limits.workers)
        bounded_limit(timeout_ms, 3600000)
        deadline = time.monotonic() + timeout_ms / 1000
        lock, results, reserved = threading.Lock(), [], [0]
        def work():
            while not self._stopping.is_set() and time.monotonic() < deadline and not (cancelled and cancelled()):
                with lock:
                    if reserved[0] >= limit:
                        return
                    reserved[0] += 1
                try:
                    item = self.run_once()
                except WikiError:
                    if self._stopping.is_set():
                        return
                    raise
                if item is None:
                    return
                with lock:
                    results.append(item)
        with ThreadPoolExecutor(max_workers=workers, thread_name_prefix="wiki-job") as pool:
            futures = [pool.submit(work) for _ in range(workers)]
            for future in futures:
                future.result()
        return {"items": results, "deadline_reached": time.monotonic() >= deadline,
                "cancelled": bool(cancelled and cancelled()) or self._stopping.is_set()}

    def retry(self, page_id):
        slug(page_id)
        def operation(tx):
            if tx.get("spec/" + page_id) is None:
                raise WikiError("unknown page")
            self._schedule(tx, page_id)
            return tx.get("job/" + page_id)
        return self.repo.write(operation)

    def revise_page(self, page_id, markdown, *, expected_revision, expected_version, actor, links=None,
                    expected_source_versions=None):
        slug(page_id)
        text(actor, "actor", 128)
        text(markdown, "markdown", self.limits.page_bytes)
        source_id(expected_revision)
        def operation(tx):
            page, spec = tx.get("page/" + page_id), tx.get("spec/" + page_id)
            if (not page or page["revision"] != expected_revision or type(expected_version) is not int or
                    page["version"] != expected_version):
                raise ConflictError("page version changed")
            sources = self._sources(tx, spec)
            versions = {s["source_id"]: s["version"] for s in sources}
            if expected_source_versions is not None:
                if (not isinstance(expected_source_versions, dict) or
                        any(type(v) is not int for v in expected_source_versions.values()) or
                        expected_source_versions != versions):
                    raise ConflictError("source versions changed during editing")
            elif page["stale"]:
                raise ConflictError("stale page requires explicit expected_source_versions after reviewing its current sources")
            artifact = self._validate_artifact({"markdown": markdown,
                "used_sources": sorted(set(re.findall(r"\[(S\d+)\]", re.sub(r"(?m)^>.*$", "", markdown)))),
                "links": links if links is not None else page["links"]}, sources, spec["links"])
            citations = [{"label": s["label"], "source_id": s["source_id"], "revision": s["revision"],
                "source_version": s["version"], "uri": s["uri"], "title": s["title"],
                "start": 0, "end": len(s["content"])} for s in sources if s["label"] in artifact["used_sources"]]
            spec.update(mode="human", generation=spec["generation"] + 1)
            tx.put("spec/" + page_id, spec)
            self._schedule(tx, page_id)  # Fence any model call, then publish the human revision.
            job = tx.get("job/" + page_id)
            extract = spec["kind"] == "source" and any(s["knowledge_pending"] for s in sources)
            if not extract:
                tx.delete(self._queue_key(job))
            job.update(state="pending" if extract else "done", token=None)
            self._set_job(tx, job)
            for link in page["links"]:
                tx.delete("back/" + link + "/" + page_id)
            page.update(markdown=markdown, links=artifact["links"], citations=citations, mode="human", actor=actor,
                        parent_revision=page["revision"], version=page["version"] + 1, stale=False, updated_ms=now_ms(),
                        provider="human", knowledge=self._entity_context(tx, spec))
            page["revision"] = digest(encode(page))
            tx.put("page/" + page_id, page)
            tx.put("history/" + page_id + "/" + page["revision"], page)
            self.index.set_page_state(tx, page_id, {"stale": False, "revision": page["revision"],
                "build_generation": job["generation"]})
            self.index.upsert(tx, "p-" + page_id, {"kind": "page", "page_id": page_id, "title": page["title"],
                "text": markdown, "revision": page["revision"], "citations": citations})
            for link in page["links"]:
                tx.put("back/" + link + "/" + page_id, True)
            self._audit(tx, "page.revise", page_id)
            return page
        return self.repo.write(operation)

    def release_page(self, page_id, *, expected_revision, expected_version, actor):
        slug(page_id)
        text(actor, "actor", 128)
        def operation(tx):
            page, spec = tx.get("page/" + page_id), tx.get("spec/" + page_id)
            if (not page or page["revision"] != expected_revision or type(expected_version) is not int or
                    page["version"] != expected_version):
                raise ConflictError("page version changed")
            spec.update(mode="compiled", generation=spec["generation"] + 1)
            tx.put("spec/" + page_id, spec)
            self._schedule(tx, page_id)
            self._audit(tx, "page.release", page_id)
            return tx.get("job/" + page_id)
        return self.repo.write(operation)

    def ask(self, question, *, limit=6, context_chars=24000, **search_options):
        with self._operation():
            return self._ask(question, limit=limit, context_chars=context_chars, **search_options)

    def _ask(self, question, *, limit, context_chars, **search_options):
        text(question, "question", 4096)
        bounded_limit(limit, 20)
        bounded_limit(context_chars, 60000)
        # Retrieval and evidence extraction use one consistent snapshot.
        with self.repo.snapshot() as view:
            found = normalize_search_result(self.index.search(view,
                "" if search_options.get("mode") in (1, 2) else question, limit=limit, **search_options))
            evidence, remaining, seen = [], context_chars, set()
            for item in found["results"]:
                if remaining <= 0:
                    break
                provenance = tuple(sorted((c["source_id"], c["revision"], c["start"], c["end"]) for c in item["citations"]))
                if provenance in seen:
                    continue
                seen.add(provenance)
                body = item["text"][:remaining]
                if not body:
                    continue
                remaining -= len(body)
                evidence.append({"label": "E" + str(len(evidence) + 1), "text": body,
                                 "title": item["title"], "citations": item["citations"], "document_id": item["id"]})
            conflicts, remaining, conflict_truncated = self._answer_conflicts(view, evidence, remaining)
            evidence.extend(conflicts)
            observed_version = view.handle.native_identity().snapshot_version
            epoch = view.get("runtime/knowledge-epoch", 0)
        if not evidence:
            message = ("No supporting evidence found." if found["empty_result_confirmed"] else
                       "No evidence returned; absence of matching evidence has not been confirmed.")
            return {"answer": message, "evidence": [], "citation_validated": False,
                    "truncated": found["truncated"], **search_quality(found)}
        answerer = self.answerer or self.provider
        cache_key = digest(encode([answerer.identity, question, evidence, epoch]))
        with self._cache_lock:
            cached = self._answer_cache.get(cache_key)
            if cached and cached[0] > now_ms():
                answer = cached[1]
                self._answer_cache.move_to_end(cache_key)
            else:
                answer = None
        if answer is None:
            answer = answerer.answer(question, evidence)
        allowed = {e["label"] for e in evidence}
        used = set(re.findall(r"\[(E\d+)\]", answer)) if isinstance(answer, str) else set()
        if not isinstance(answer, str) or not used or not used <= allowed or len(answer.encode()) > self.limits.answer_bytes:
            raise ProviderError("answer contains missing or unknown evidence references")
        # Mark evidence that changed during a slow model call; never label it current silently.
        with self.repo.snapshot() as view:
            stale = any(not (current := view.get("source/" + citation["source_id"])) or current["deleted"] or
                        current["revision"] != citation["revision"] or current["version"] != citation.get("source_version")
                        for e in evidence for citation in e["citations"])
            for item in found["results"]:
                current = self.index.get_document(view, item["id"])
                stale |= current is None or current["revision"] != item["revision"]
                if current is not None:
                    stale |= any(current.get(name) != item[name] for name in
                                 ("projection_version", "lifecycle_generation", "publication_generation") if name in item)
                if item["kind"] == "page":
                    page = self.index.get_page_state(view, item["page_id"])
                    stale |= page is None or page["stale"]
            stale |= view.get("runtime/knowledge-epoch", 0) != epoch
        if stale:
            return {"answer": "", "evidence": [], "citation_validated": False, "stale": True, "retryable": True,
                    "truncated": found["truncated"], **search_quality(found)}
        with self._cache_lock:
            self._answer_cache[cache_key] = (now_ms() + self.limits.answer_cache_ttl_ms, answer, len(answer.encode()))
            stamp = now_ms()
            for key in list(self._answer_cache):
                if self._answer_cache[key][0] <= stamp:
                    del self._answer_cache[key]
            while (len(self._answer_cache) > self.limits.answer_cache_items or
                   sum(item[2] for item in self._answer_cache.values()) > self.limits.answer_cache_bytes):
                self._answer_cache.popitem(last=False)
        return {"answer": answer, "evidence": evidence, "citation_validated": True, "stale": stale,
                "observed_database_version": observed_version,
                "conflict_context_truncated": conflict_truncated,
                "truncated": found["truncated"] or remaining == 0 or conflict_truncated or any(d["text_truncated"] for d in found["results"]),
                **search_quality(found)}

    def status(self):
        with self.repo.snapshot() as view:
            states = view.get("runtime/job-counts", {})
            stats = self.index.statistics(view)
        info = self.repo.store.info()
        runtime = self.repo.store.runtime_info()
        return {"namespace": self.namespace, "schema": SCHEMA_VERSION, "provider": self.provider.identity,
                "compiler": (self.compiler or self.provider).identity, "answerer": (self.answerer or self.provider).identity,
                "limits": asdict(self.limits), "index_provider": self.index_definition, "search": stats, "jobs": states, "database": {"version": info.version,
                "durable_version": info.durable_version, "file_bytes": info.file_bytes},
                "maintenance": {"running": bool(runtime.running), "last_status": runtime.last_status,
                                "compactions": runtime.compactions}}

    def lint(self):
        issues = []
        with self.repo.snapshot() as view:
            for _, page in view.scan("page/"):
                if page["stale"]:
                    issues.append({"page_id": page["id"], "code": "stale"})
                for link in page["links"]:
                    if view.get("page/" + link) is None:
                        issues.append({"page_id": page["id"], "code": "dangling-link", "target": link})
                for citation in page["citations"]:
                    current = view.get("source/" + citation["source_id"])
                    if not current or current["deleted"] or current["revision"] != citation["revision"] or current["version"] != citation.get("source_version"):
                        issues.append({"page_id": page["id"], "code": "outdated-source", "source_id": citation["source_id"]})
                    if view.get("raw/" + citation["source_id"] + "/" + citation["revision"]) is None:
                        issues.append({"page_id": page["id"], "code": "missing-raw"})
        return {"issues": issues, "ok": not issues}

    def export(self, destination):
        root = Path(destination).resolve()
        # A fresh destination prevents stale exports and accidental overwrites.
        root.mkdir(parents=True, exist_ok=False)
        (root / "raw").mkdir()
        (root / "wiki").mkdir()
        pages, source_count = [], 0
        with self.repo.snapshot() as view:
            for _, raw in view.scan("raw/"):
                name = raw["source_id"] + "-" + raw["revision"] + ".md"
                (root / "raw" / name).write_text(raw["content"], encoding="utf-8")
                (root / "raw" / (name[:-3] + ".json")).write_bytes(encode({k: v for k, v in raw.items() if k != "content"}))
                source_count += 1
            for _, page in view.scan("page/"):
                # Include stale state visibly; it remains inspectable, never silently current.
                body = ("> STALE: rebuild required.\n\n" if page["stale"] else "") + page["markdown"]
                body = re.sub(r"\[\[([a-z0-9_-]+)\]\]", lambda m: "[" + m[1] + "](" + m[1] + ".md)", body)
                body += "\n\n## Source revisions\n\n"
                for c in page["citations"]:
                    body += "- [%s](../raw/%s-%s.md) — %s\n" % (c["label"], c["source_id"], c["revision"], c["title"].replace("\n", " "))
                (root / "wiki" / (page["id"] + ".md")).write_text(body, encoding="utf-8")
                pages.append(page)
            (root / "manifest.json").write_bytes(encode({"schema": SCHEMA_VERSION, "namespace": self.namespace,
                "sources": [value for _, value in view.scan("source/")], "pages": pages,
                "specs": [value for _, value in view.scan("spec/")],
                "entities": [value for _, value in view.scan("entity/")],
                "claims": [value for _, value in view.scan("claim/")],
                "conflicts": [value for _, value in view.scan("conflict/")]}))
            with (root / "log.jsonl").open("wb") as output:
                for _, event in view.scan("event/"):
                    output.write(encode(event) + b"\n")
        (root / "index.md").write_text("# Wiki\n\n" + "\n".join(
            "- [%s](wiki/%s.md)%s" % (p["title"].replace("\n", " "), p["id"], " (stale)" if p["stale"] else "") for p in pages), encoding="utf-8")
        return {"destination": str(root), "pages": len(pages), "raw_revisions": source_count}

    def backup(self, destination):
        return {"snapshot_version": self.repo.store.backup(destination).snapshot_version}

    def enable_maintenance(self):
        # Process-local native maintenance policy, independent of the write durability policy.
        # Use the SDK defaults for optional maintenance I/O quotas. Finite
        # budgets can put Core into recovery-required when reclamation exhausts them.
        self.repo.store.configure(background=True, interval_ms=1000, job_timeout_ms=1000)

    def compact(self):
        result = self.repo.store.compact(timeout_ms=30000)
        return {"source_file_bytes": result.source_file_bytes, "output_file_bytes": result.output_file_bytes,
                "snapshot_version": result.snapshot_version, "final_version": result.final_version}

    def verify(self):
        result = self.repo.store.verify(timeout_ms=30000)
        return {"snapshot_version": result.snapshot_version, "stored_keys": result.stored_keys,
                "stored_value_bytes": result.stored_value_bytes}

    def maintain_index(self, *, operation, **options):
        maintain = getattr(self.index, "maintain", None)
        if maintain is None:
            raise WikiError("maintenance requires the shared NativeIndex provider")
        for name in ("after_term", "after_id", "after_key"):
            if name in options and isinstance(options[name], str):
                options[name] = bytes.fromhex(options[name])
        if isinstance(options.get("vector_space"), str):
            options["vector_space"] = options["vector_space"].encode("utf-8")
        result = self.repo.write(lambda tx: maintain(tx, operation=operation, **options))
        return {k: v.hex() if isinstance(v, bytes) else v for k, v in result.items()}

    def set_vectors(self, items, *, vector_space):
        """Publish host-computed embeddings with projection CAS; no network call inside a transaction."""
        setter = getattr(self.index, "set_vector", None)
        if setter is None:
            raise WikiError("vectors require the shared NativeIndex provider")
        if not isinstance(items, list) or not items or len(items) > self.limits.batch_items or len(encode(items)) > self.limits.batch_bytes:
            raise BudgetError("vector batch quota exceeded")
        if any(not isinstance(i, dict) or set(i) != {"id", "vector", "expected_projection_version"} for i in items):
            raise WikiError("invalid vector batch item")
        ids = [text(i["id"], "document id", 1024) for i in items]
        if len(set(ids)) != len(ids):
            raise WikiError("duplicate vector target")
        def operation(tx):
            result = {"items": [{"id": i["id"], "projection_version": setter(tx, i["id"], i["vector"],
                vector_space, i["expected_projection_version"])} for i in items]}
            self._audit(tx, "vectors.publish", digest(vector_space))
            return result
        return self.repo.write(operation)
