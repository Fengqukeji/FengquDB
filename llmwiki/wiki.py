"""Transactional source history, dependency graph and recoverable compilation."""

import hashlib
import re
import time
import uuid
from pathlib import Path

from . import retrieval
from .index_provider import (DEFAULT_DEFINITION, IndexDefinition, PythonIndexProvider,
                             normalize_search_result, search_quality)
from .providers import ExtractiveProvider, PROMPT_VERSION, ProviderError
from .storage import Repository, encode

SCHEMA_VERSION = 1
MAX_SOURCE_BYTES = 256 * 1024
MAX_PAGE_BYTES = 1024 * 1024
MAX_SOURCES = 16
MAX_ATTEMPTS = 3


class WikiError(ValueError):
    pass


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


class Wiki:
    def __init__(self, path, *, library=None, namespace="default", provider=None, index_provider=None):
        text(namespace, "namespace", 128)
        self.index = index_provider if index_provider is not None else PythonIndexProvider()
        if getattr(self.index, "atomic_updates", None) is not True:
            raise WikiError("this wiki requires an index provider with same-transaction atomic updates")
        if not isinstance(getattr(self.index, "definition", None), IndexDefinition):
            raise WikiError("index provider requires an IndexDefinition")
        self.index_definition = self.index.definition.descriptor()
        self.repo = Repository(path, "wiki/" + digest(namespace)[:32] + "/", library)
        self.provider = provider or ExtractiveProvider()
        self.namespace = namespace
        try:
            def initialize(tx):
                schema = tx.get("schema")
                expected = {"version": SCHEMA_VERSION, "tokenizer": self.index.definition.tokenizer_version}
                if schema is not None and schema != expected:
                    raise WikiError("unsupported wiki schema/tokenizer; migrate before opening")
                if schema is None:
                    tx.put("schema", expected)
                installed = tx.get("index/provider")
                if installed is not None and installed != self.index_definition:
                    raise WikiError("index provider changed; explicit rebuild/migration required")
                if installed is None:
                    # A legacy nonempty index can only be adopted by its original format.
                    legacy_count = tx.get("search/stats", {}).get("count", 0)
                    if legacy_count and self.index.definition != DEFAULT_DEFINITION:
                        raise WikiError("legacy index requires explicit migration to a different provider")
                    tx.put("index/provider", self.index_definition)
            self.repo.write(initialize)
        except BaseException:
            self.repo.close()
            raise

    def close(self):
        self.repo.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    @staticmethod
    def _audit(tx, action, entity):
        sequence = tx.get("audit/sequence", 0) + 1
        tx.put("audit/sequence", sequence)
        tx.put("event/%016x" % sequence, {"time_ms": now_ms(), "action": action, "entity": entity})

    @staticmethod
    def _queue_key(job):
        return "queue/%016x/%s" % (job["due_ms"], job["page_id"])

    def _schedule(self, tx, page_id):
        old = tx.get("job/" + page_id)
        if old and old["state"] in ("pending", "running"):
            tx.delete(self._queue_key(old))
        generation = (old or {}).get("generation", 0) + 1
        job = {"page_id": page_id, "state": "pending", "attempts": 0, "generation": generation,
               "due_ms": now_ms(), "token": None, "error": None}
        tx.put("job/" + page_id, job)
        tx.put(self._queue_key(job), page_id)
        page = tx.get("page/" + page_id)
        if page:
            if not page["stale"]:
                page["stale"] = True
                tx.put("page/" + page_id, page)
            self.index.set_page_state(tx, page_id, {"stale": True, "revision": page["revision"],
                                                   "build_generation": generation})

    def _define(self, tx, page_id, title, source_ids, links, kind):
        old = tx.get("spec/" + page_id)
        if old:
            for sid in old["source_ids"]:
                tx.delete("dep/" + sid + "/" + page_id)
        spec = {"id": page_id, "title": title, "source_ids": source_ids, "links": links,
                "kind": kind, "generation": (old or {}).get("generation", 0) + 1}
        for sid in source_ids:
            tx.put("dep/" + sid + "/" + page_id, True)
        tx.put("spec/" + page_id, spec)
        self._schedule(tx, page_id)
        return spec

    def ingest(self, uri, content, *, title=None):
        text(uri, "uri", 2048)
        text(content, "source content", MAX_SOURCE_BYTES)
        title = text(title or uri, "title", 512)
        sid = digest(uri)
        revision = digest(encode({"uri": uri, "title": title, "content": content}))
        stamp = now_ms()

        def operation(tx):
            previous = tx.get("source/" + sid)
            if previous and not previous["deleted"] and previous["revision"] == revision:
                return dict(previous, changed=False)
            planned = list(retrieval.chunks(content))
            new_ids = {"c-" + sid + "-%04x" % index for index in range(len(planned))}
            for doc_id in (previous or {}).get("chunks", []):
                if doc_id not in new_ids:
                    self.index.delete(tx, doc_id)
            chunk_ids = []
            publication_generation = (previous or {}).get("publication_generation", 0) + 1
            for index, (start, end, body) in enumerate(planned):
                doc_id = "c-" + sid + "-%04x" % index
                chunk_ids.append(doc_id)
                self.index.upsert(tx, doc_id, {"kind": "chunk", "source_id": sid, "revision": revision,
                    "title": title, "text": body, "source_publication_generation": publication_generation,
                    "citations": [{"source_id": sid, "revision": revision,
                    "start": start, "end": end, "uri": uri, "title": title}]})
            source = {"id": sid, "uri": uri, "title": title, "revision": revision,
                      "content_hash": digest(content), "deleted": False, "updated_ms": stamp, "chunks": chunk_ids,
                      "publication_generation": publication_generation}
            # The same source+revision key always has exactly the same immutable value.
            raw_key = "raw/" + sid + "/" + revision
            if tx.get(raw_key) is None:
                tx.put(raw_key, {"source_id": sid, "revision": revision, "uri": uri, "title": title, "content": content})
            tx.put("source/" + sid, source)
            own_page = "source-" + sid[:24]
            self._define(tx, own_page, title, [sid], [], "source")
            for key, _ in list(tx.scan("dep/" + sid + "/")):
                page_id = key.rsplit("/", 1)[1]
                if page_id != own_page:
                    self._schedule(tx, page_id)
            self._audit(tx, "source.upsert", sid)
            return dict(source, changed=True)
        return self.repo.write(operation)

    def define_page(self, page_id, title, source_ids, *, links=None):
        slug(page_id)
        if page_id.startswith("source-"):
            raise WikiError("source- prefix is reserved")
        text(title, "title", 512)
        if not isinstance(source_ids, list) or not 1 <= len(source_ids) <= MAX_SOURCES:
            raise WikiError("a page requires 1..16 source ids")
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

    def delete_source(self, sid):
        source_id(sid)

        def operation(tx):
            source = tx.get("source/" + sid)
            if not source or source["deleted"]:
                return False
            for doc_id in source["chunks"]:
                self.index.delete(tx, doc_id)
            source.update(deleted=True, chunks=[], updated_ms=now_ms())
            tx.put("source/" + sid, source)
            for key, _ in list(tx.scan("dep/" + sid + "/")):
                self._schedule(tx, key.rsplit("/", 1)[1])
            self._audit(tx, "source.delete", sid)
            return True
        return self.repo.write(operation)

    def source(self, sid, *, revision=None):
        source_id(sid)
        with self.repo.snapshot() as view:
            current = view.get("source/" + sid)
            if current is None:
                return None
            revision = revision or current["revision"]
            if not isinstance(revision, str) or not re.fullmatch(r"[a-f0-9]{64}", revision):
                raise WikiError("invalid revision")
            raw = view.get("raw/" + sid + "/" + revision)
            return dict(raw, deleted=current["deleted"], current_revision=current["revision"]) if raw else None

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
        if kind not in ("source", "page", "job", "spec", "event"):
            raise WikiError("invalid collection")
        bounded_limit(limit)
        prefix = kind + "/"
        if not isinstance(after, str) or (after and (not after.startswith(prefix) or len(after) > 256)):
            raise WikiError("invalid cursor")
        with self.repo.snapshot() as view:
            rows = list(view.scan(prefix, after=after, limit=limit + 1))
            return {"items": [value for _, value in rows[:limit]],
                    "next_cursor": rows[limit - 1][0] if len(rows) > limit else None}

    def search(self, query, *, limit=10, posting_budget=12000, candidate_budget=1024):
        text(query, "query", 2048)
        bounded_limit(limit, 50)
        bounded_limit(posting_budget, 100000)
        bounded_limit(candidate_budget, 4096)
        with self.repo.snapshot() as view:
            return normalize_search_result(self.index.search(view, query, limit=limit,
                posting_budget=posting_budget, candidate_budget=candidate_budget))

    def _claim(self, lease_ms):
        stamp = now_ms()

        def operation(tx):
            # Due-time ordered durable queue; no scan of completed job history.
            for key, page_id in tx.scan("queue/", limit=64):
                if int(key.split("/")[1], 16) > stamp:
                    return None
                job = tx.get("job/" + page_id)
                tx.delete(key)
                if not job or job["state"] not in ("pending", "running") or self._queue_key(job) != key:
                    continue
                if job["attempts"] >= MAX_ATTEMPTS:
                    job.update(state="failed", token=None, error="attempt budget exhausted")
                    tx.put("job/" + page_id, job)
                    continue
                job.update(state="running", token=uuid.uuid4().hex, due_ms=stamp + lease_ms,
                           attempts=job["attempts"] + 1)
                tx.put("job/" + page_id, job)
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
            sources.append(dict(raw, label="S" + str(index)))
        return sources

    @staticmethod
    def _validate_artifact(artifact, sources, links):
        if not isinstance(artifact, dict):
            raise ProviderError("compile output must be an object")
        markdown, used, actual_links = artifact.get("markdown"), artifact.get("used_sources"), artifact.get("links")
        if not isinstance(markdown, str) or not markdown.strip() or len(markdown.encode()) > MAX_PAGE_BYTES:
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
        return {"markdown": markdown, "used_sources": sorted(set(used)), "links": sorted(set(actual_links))}

    def run_once(self, *, lease_ms=180000):
        if type(lease_ms) is not int or not 1 <= lease_ms <= 3600000:
            raise WikiError("lease_ms must be between 1 and 3600000")
        job = self._claim(lease_ms)
        if not job:
            return None
        page_id = job["page_id"]
        try:
            with self.repo.snapshot() as view:
                spec = view.get("spec/" + page_id)
                if spec is None:
                    raise WikiError("missing page specification")
                sources = self._sources(view, spec)
                fingerprint = digest(encode({"spec": spec, "sources": [(s["source_id"], s["revision"]) for s in sources],
                                             "provider": self.provider.identity, "prompt": PROMPT_VERSION}))
                artifact = view.get("artifact/" + fingerprint)
            if sources:
                if artifact is None:
                    artifact = self.provider.compile(spec["title"], sources, spec["links"])
                    artifact = self._validate_artifact(artifact, sources, spec["links"])
                    self.repo.write(lambda tx: tx.put("artifact/" + fingerprint, artifact))
                else:
                    artifact = self._validate_artifact(artifact, sources, spec["links"])

            def publish(tx):
                current = tx.get("job/" + page_id)
                if not current or current["token"] != job["token"] or current["state"] != "running":
                    return {"page_id": page_id, "state": "superseded"}
                latest_spec = tx.get("spec/" + page_id)
                latest_sources = self._sources(tx, latest_spec)
                if latest_spec != spec or [(s["source_id"], s["revision"]) for s in latest_sources] != [
                        (s["source_id"], s["revision"]) for s in sources]:
                    self._schedule(tx, page_id)
                    return {"page_id": page_id, "state": "superseded"}
                old = tx.get("page/" + page_id)
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
                                  "uri": s["uri"], "title": s["title"], "start": 0, "end": len(s["content"])}
                                 for s in sources if s["label"] in artifact["used_sources"]]
                    page = {"id": page_id, "title": spec["title"], "kind": spec["kind"],
                            "markdown": artifact["markdown"], "citations": citations, "links": artifact["links"],
                            "revision": fingerprint, "stale": False, "updated_ms": now_ms(),
                            "provider": self.provider.identity, "missing_sources": len(spec["source_ids"]) - len(sources)}
                    tx.put("page/" + page_id, page)
                    self.index.set_page_state(tx, page_id, {"stale": False, "revision": fingerprint,
                                                           "build_generation": current["generation"]})
                    tx.put("history/" + page_id + "/" + fingerprint, page)
                    for link in page["links"]:
                        tx.put("back/" + link + "/" + page_id, True)
                    self.index.upsert(tx, "p-" + page_id, {"kind": "page", "page_id": page_id,
                        "title": page["title"], "text": page["markdown"], "revision": fingerprint, "citations": citations})
                    state = "done"
                tx.delete(self._queue_key(current))
                current.update(state=state, token=None, error=None)
                tx.put("job/" + page_id, current)
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
                state = "failed" if current["attempts"] >= MAX_ATTEMPTS else "pending"
                current.update(state=state, token=None, error=type(exc).__name__,
                               due_ms=now_ms() + 1000 * 2 ** current["attempts"])
                tx.put("job/" + page_id, current)
                if state == "pending":
                    tx.put(self._queue_key(current), page_id)
                self._audit(tx, "job.error", page_id)
                return {"page_id": page_id, "state": state, "error": current["error"]}
            return self.repo.write(fail)

    def retry(self, page_id):
        slug(page_id)
        def operation(tx):
            if tx.get("spec/" + page_id) is None:
                raise WikiError("unknown page")
            self._schedule(tx, page_id)
            return tx.get("job/" + page_id)
        return self.repo.write(operation)

    def ask(self, question, *, limit=6, context_chars=24000):
        text(question, "question", 4096)
        bounded_limit(limit, 20)
        bounded_limit(context_chars, 60000)
        # Retrieval and evidence extraction use one consistent snapshot.
        with self.repo.snapshot() as view:
            found = normalize_search_result(self.index.search(view, question, limit=limit))
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
            observed_version = self.repo.store.info().version
        if not evidence:
            message = ("No supporting evidence found." if found["empty_result_confirmed"] else
                       "No evidence returned; absence of matching evidence has not been confirmed.")
            return {"answer": message, "evidence": [], "citation_validated": False,
                    "truncated": found["truncated"], **search_quality(found)}
        answer = self.provider.answer(question, evidence)
        allowed = {e["label"] for e in evidence}
        used = set(re.findall(r"\[(E\d+)\]", answer)) if isinstance(answer, str) else set()
        if not isinstance(answer, str) or not used or not used <= allowed:
            raise ProviderError("answer contains missing or unknown evidence references")
        # Mark evidence that changed during a slow model call; never label it current silently.
        with self.repo.snapshot() as view:
            stale = any(not (current := view.get("source/" + citation["source_id"])) or current["deleted"] or
                        current["revision"] != citation["revision"]
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
        return {"answer": answer, "evidence": evidence, "citation_validated": True, "stale": stale,
                "observed_database_version": observed_version,
                "truncated": found["truncated"] or remaining == 0 or any(d["text_truncated"] for d in found["results"]),
                **search_quality(found)}

    def status(self):
        with self.repo.snapshot() as view:
            states = {}
            for _, job in view.scan("job/"):
                states[job["state"]] = states.get(job["state"], 0) + 1
            stats = self.index.statistics(view)
        info = self.repo.store.info()
        runtime = self.repo.store.runtime_info()
        return {"namespace": self.namespace, "schema": SCHEMA_VERSION, "provider": self.provider.identity,
                "index_provider": self.index_definition, "search": stats, "jobs": states, "database": {"version": info.version,
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
                    if not current or current["deleted"] or current["revision"] != citation["revision"]:
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
                "sources": [value for _, value in view.scan("source/")], "pages": pages}))
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
        self.repo.store.configure(background=True, interval_ms=1000, job_timeout_ms=1000,
                                  job_max_read_bytes=512 * 1024 * 1024, job_max_write_bytes=512 * 1024 * 1024)

    def compact(self):
        result = self.repo.store.compact(timeout_ms=30000)
        return {"source_file_bytes": result.source_file_bytes, "output_file_bytes": result.output_file_bytes,
                "snapshot_version": result.snapshot_version, "final_version": result.final_version}

    def verify(self):
        result = self.repo.store.verify(timeout_ms=30000)
        return {"snapshot_version": result.snapshot_version, "stored_keys": result.stored_keys,
                "stored_value_bytes": result.stored_value_bytes}
