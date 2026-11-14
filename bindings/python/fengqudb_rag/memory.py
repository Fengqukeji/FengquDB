"""Account-isolated, model-pluggable MemoryStore with native retrieval and governance."""
from collections import OrderedDict
from contextlib import contextmanager
from dataclasses import asdict
import hashlib
import json
import math
from pathlib import Path
import threading

from ._bindings import Store, StoreError, ffi

from .embedding import EmbeddingRuntime
from .models import MemoryWrite, MemoryImport, MemorySearch, MemoryList, MemoryDelete, coerce, payload
from .memory_index import MemoryIndex
from .repository import ROOT, Repository, encode, digest, load, save, take, rkey, nkey, public
from .context import pack_context
from .types import (QueryBudget, RequestGuard, MAX_VERSION, integer, bounded_string,
    RagError, RagConflict, RagNotFound, RagIntegrityError, RagStaleRead, RagUnsupported,
    RagCancelled, RagDeadline, RagLimit, RagRecoveryRequired)


class _Account:
    def __init__(self, store, index, runtime):
        self.store, self.index = store, index
        self.repo = Repository(store, index, runtime.contract.fingerprint.encode())
        self.lock = threading.RLock()
        self.active = 0
        self.poisoned = False

    @contextmanager
    def errors(self):
        if self.poisoned:
            raise RagRecoveryRequired("close/reopen MemoryStore before reading uncertain state")
        try:
            yield
        except StoreError as exc:
            if exc.code in (10, 11):
                self.poisoned = True
                raise RagRecoveryRequired("commit/recovery outcome uncertain; reopen to recover") from exc
            kind = {1: RagNotFound, 5: RagIntegrityError, 7: RagConflict, 9: RagLimit,
                    17: RagDeadline, 19: RagCancelled}.get(exc.code, RagError)
            raise kind(str(exc)) from exc

    @contextmanager
    def gated(self, guard):
        while not self.lock.acquire(timeout=min(0.05, guard.remaining())):
            guard.check()
        try:
            guard.check()
            yield
        finally:
            self.lock.release()


class MemoryStore:
    """WordGame-compatible methods plus retrieve/context/scoped access.

    Each account owns one local Store; all its owners share it. Embedding is
    injected and externally owned, no model download/process/remote DB here.
    New stores only: legacy zvec migration is intentionally absent.
    """
    def __init__(self, root, *, embedding, core_library=None, index_library=None, index=None,
                 clock_id="logical-tick-v1", max_accounts=8, store_options=None,
                 embedding_cache_bytes=4 * 1024 * 1024, max_batch=1024):
        bounded_string(clock_id, "clock_id", 160)
        if type(max_accounts) is not int or not 1 <= max_accounts <= 1024:
            raise ValueError("invalid account handle budget")
        if type(max_batch) is not int or not 1 <= max_batch <= 4096:
            raise ValueError("invalid batch limit")
        self.root = Path(root).resolve()
        self.runtime = EmbeddingRuntime(embedding, cache_bytes=embedding_cache_bytes)
        self.core_library = core_library
        self.client = index or ffi.NativeIndex(index_library)
        if not isinstance(self.client, ffi.NativeIndex):
            raise TypeError("index must be shared NativeIndex")
        self.clock_id, self.max_accounts, self.max_batch = clock_id, max_accounts, max_batch
        # One atomic 1024-item batch stages canonical rows and native Index
        # directories together. Reserve bounded delta space for those keys;
        # keep Core durability and its 50 ms / 1 MiB write policy unchanged.
        self.store_options = dict(cache_bytes=16 * 1024 * 1024, delta_bytes=16 * 1024 * 1024)
        self.store_options.update(store_options or {})
        if set(self.store_options) & {"library", "create", "name"}:
            raise ValueError("Store ownership/path options cannot be overridden")
        self._accounts = OrderedDict()
        self._condition = threading.Condition(threading.RLock())
        self._threads = {}
        self._closing = self._closed = False

    @staticmethod
    def _account_id(account_id):
        bounded_string(account_id, "account_id", 1000)
        return hashlib.sha256(account_id.encode()).hexdigest()

    def _open(self, account_id):
        directory = self.root / self._account_id(account_id)
        # Refuse silently creating a replacement alongside old zvec data.
        if any((directory / name).exists() for name in ("manifest.json", "collection", "collection-v2", "pending-write.json")):
            raise RagUnsupported("legacy RAG directory: use an explicitly fresh root; no migration or deletion is performed")
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / "memory"
        store = Store(path, library=self.core_library, create=True, **self.store_options)
        try:
            index = MemoryIndex(store, self.client)
            account = _Account(store, index, self.runtime)
            expected = dict(schema=1, kind="memory", account_hash=self._account_id(account_id),
                clock_id=self.clock_id, embedding=asdict(self.runtime.contract),
                vector_space=self.runtime.contract.fingerprint)
            tx = store.transaction()
            try:
                old = load(tx, ROOT + b"meta")
                if old is None:
                    if store.info().stored_keys:
                        raise RagIntegrityError("RAG metadata missing from a nonempty Store")
                    save(tx, ROOT + b"meta", expected)
                    save(tx, ROOT + b"count", 0)
                    tx.commit(force_reliable=True)
                elif old != expected:
                    raise RagIntegrityError("RAG format/account/clock/Embedding identity mismatch")
            finally:
                tx.close()
            with account.errors():
                account.repo.recover()
            return account
        except BaseException:
            store.close()
            raise

    @contextmanager
    def _account(self, account_id):
        self._account_id(account_id)
        self.runtime.check_contract()
        thread = threading.get_ident()
        with self._condition:
            if self._closing:
                raise RuntimeError("MemoryStore is closing/closed")
            account = self._accounts.get(account_id)
            if account is None:
                if len(self._accounts) >= self.max_accounts:
                    victim = next((key for key, value in self._accounts.items() if not value.active), None)
                    if victim is None:
                        raise RagLimit("all cached account handles are in use")
                    self._accounts.pop(victim).store.close()
                try:
                    account = self._open(account_id)
                except StoreError as exc:
                    raise RagError("account Store could not be opened: " + str(exc)) from exc
                self._accounts[account_id] = account
            self._accounts.move_to_end(account_id)
            account.active += 1
            self._threads[thread] = self._threads.get(thread, 0) + 1
        try:
            with account.errors():
                yield account
        finally:
            with self._condition:
                account.active -= 1
                self._threads[thread] -= 1
                if not self._threads[thread]:
                    del self._threads[thread]
                self._condition.notify_all()

    def close(self):
        with self._condition:
            if self._threads.get(threading.get_ident()):
                raise RuntimeError("cannot close MemoryStore inside an active operation")
            if self._closing:
                self._condition.wait_for(lambda: self._closed)
                return
            self._closing = True
            self._condition.wait_for(lambda: not any(a.active for a in self._accounts.values()))
            accounts = list(self._accounts.values())
            self._accounts.clear()
        failure = None
        try:
            for account in accounts:
                try:
                    account.store.close()
                except BaseException as exc:
                    if failure is None:
                        failure = exc
        finally:
            with self._condition:
                self._closed = True
                self._condition.notify_all()
        if failure is not None:
            raise failure

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    @staticmethod
    def _budget(value):
        value = value or QueryBudget(candidate_budget=100000, scan_budget=250000)
        if not isinstance(value, QueryBudget):
            raise TypeError("budget must be QueryBudget")
        return value

    def put(self, account_id, items, *, record_id=None, budget=None, cancelled=None):
        if not isinstance(items, (list, tuple)) or len(items) > self.max_batch:
            raise RagLimit("write batch exceeds configured limit")
        if not items:
            return []
        items = [coerce(MemoryWrite, item) for item in items]
        if record_id is not None and len(items) != 1:
            raise ValueError("record update requires exactly one item")
        if record_id is not None:
            rkey(record_id)
        if sum(len(item.text.encode()) for item in items) > 4 * 1024 * 1024:
            raise RagLimit("batch text exceeds 4 MiB")
        guard = RequestGuard(self._budget(budget), cancelled)
        with self._account(account_id) as account:
            with account.gated(guard):
                prepared = account.repo.prepare(items, record_id)
                # Entirely duplicate requests return before invoking the Provider.
                if record_id and prepared["initial"]["_dedupe"] == digest(payload(items[0])).decode():
                    return [public(prepared["initial"])]
                if not record_id and all(prepared["existing"].values()):
                    return [public(prepared["existing"][digest(payload(item)).decode()]) for item in items]
            texts = list(dict.fromkeys(item.text for item in items if item.text not in prepared["raw"] and
                (record_id or prepared["existing"][digest(payload(item)).decode()] is None)))
            values = self.runtime.embed(texts, guard=guard)
            vectors = dict(prepared["raw"])
            vectors.update(zip(texts, values))
            with account.gated(guard):
                return account.repo.commit(items, prepared, vectors, record_id)

    def import_text(self, account_id, item, *, budget=None, cancelled=None):
        item = coerce(MemoryImport, item)
        if item.fact_key:
            raise ValueError("a multi-chunk import cannot use one fact_key")
        values = payload(item)
        values["source_id"] = item.source_id or "import:" + hashlib.sha256(item.text.encode()).hexdigest()
        parts, start = [], 0
        while start < len(item.text):
            end = min(start + 1200, len(item.text))
            if end < len(item.text):
                boundary = max(item.text.rfind("\n", start + 600, end), item.text.rfind("。", start + 600, end))
                if boundary >= 0:
                    end = boundary + 1
            segment = item.text[start:end].strip()
            if segment:
                parts.append(MemoryWrite(**dict(values, text=segment)))
            if end == len(item.text):
                break
            start = end - 100
        return self.put(account_id, parts, budget=budget, cancelled=cancelled)

    def get(self, account_id, record_id):
        with self._account(account_id) as account, account.lock, account.store.snapshot() as view:
            return public(account.repo.record(view, record_id))

    def list(self, account_id, selector):
        selector = coerce(MemoryList, selector)
        with self._account(account_id) as account, account.lock, account.store.snapshot() as view:
            return account.repo.listing(view, selector)

    def history(self, account_id, record_id, *, after_revision=0, limit=50):
        integer(after_revision, "after_revision")
        integer(limit, "limit", 100)
        if not limit:
            raise ValueError("history limit must be positive")
        with self._account(account_id) as account, account.lock, account.store.snapshot() as view:
            return account.repo.history(view, record_id, after_revision, limit)

    def _purge(self, account, selector, retire, guard):
        with account.gated(guard):
            job = account.repo.start_purge(selector, retire=retire)
        while True:
            with account.gated(guard):
                done, deleted = account.repo.purge_step(job)
            if done:
                return {"deleted": deleted}

    def delete(self, account_id, record_id, *, budget=None, cancelled=None):
        guard = RequestGuard(self._budget(budget), cancelled)
        with self._account(account_id) as account:
            with account.gated(guard), account.store.snapshot() as view:
                record = account.repo.record(view, record_id)
                selector = {name: record[name] for name in ("scope", "owner_id", "timeline", "lineage_id")}
            self._purge(account, selector, False, guard)

    def delete_matching(self, account_id, selector, *, budget=None, cancelled=None):
        selector = coerce(MemoryDelete, selector)
        values = {name: getattr(selector, name) for name in ("scope", "owner_id", "timeline", "source_id")}
        guard = RequestGuard(self._budget(budget), cancelled)
        with self._account(account_id) as account:
            return self._purge(account, values, False, guard)

    def retire_namespace(self, account_id, selector, *, budget=None, cancelled=None):
        selector = coerce(MemoryDelete, selector)
        if selector.source_id:
            raise ValueError("namespace retirement cannot be restricted to a source")
        guard = RequestGuard(self._budget(budget), cancelled)
        values = {name: getattr(selector, name) for name in ("scope", "owner_id", "timeline", "source_id")}
        with self._account(account_id) as account:
            return self._purge(account, values, True, guard)

    def recover(self, account_id, *, budget=None, cancelled=None):
        guard = RequestGuard(self._budget(budget), cancelled)
        with self._account(account_id) as account, account.gated(guard):
            return {"completed_jobs": account.repo.recover(guard)}

    def stats(self, account_id):
        with self._account(account_id) as account, account.lock, account.store.snapshot() as view:
            return {"records": load(view, ROOT + b"count", 0), "storage": "fengqudb",
                    "pending_cleanup": len(take(view, ROOT + b"J/", 65)), "cached_accounts": len(self._accounts)}

    def embedding_status(self):
        self.runtime.check_contract()
        method = getattr(self.runtime.provider, "status", None)
        return {"contract": asdict(self.runtime.contract), "runtime": method() if callable(method) else {"state": "external"}}

    def warmup(self, *, budget=None, cancelled=None):
        guard = RequestGuard(self._budget(budget), cancelled)
        guard.check()
        method = getattr(self.runtime.provider, "warmup", None)
        if not callable(method):
            raise RagUnsupported("Embedding Provider does not expose warmup")
        result = method()
        self.runtime.check_contract()
        guard.check()
        return result

    def backup(self, account_id, destination):
        with self._account(account_id) as account, account.lock:
            return account.store.backup(destination)

    @staticmethod
    def _validate_projection(account, view, hit, request, owner, state):
        document = hit["document"]
        try:
            record_id = document["id"].decode("ascii")
            record = account.repo.record(view, record_id)
            if (record["scope"], record["owner_id"], record["timeline"]) != (request.scope, owner, request.timeline):
                raise RagIntegrityError("native result crossed authorized namespace")
            if not account.repo.time_visible(record, request.as_of) or (request.source_id and record["source_id"] != request.source_id):
                raise RagIntegrityError("native result violates source/time predicate")
            if (document["projection_version"] != record["projection_version"] or document["domain_revision"] != record["revision"] or
                document["lifecycle_generation"] != state["generation"] or document["deleted"] or
                document["vector_space"] != account.repo.vector_space or
                document["source_locator"] != ("memory:" + record_id).encode() or
                document["texts"].get("body", (None,))[0] != record["text"] or
                document["texts"].get("title", (None,))[0] != record["title"] or
                record["content_hash"] != hashlib.sha256(record["text"].encode()).hexdigest()):
                raise RagIntegrityError("canonical record and native projection differ")
            for key, value in account.index.metadata(record).items():
                actual = document["metadata"].get(key)
                if actual != value or type(actual) is not type(value):
                    raise RagIntegrityError("canonical metadata and native projection differ")
            return record
        except (KeyError, ValueError, TypeError, AttributeError) as exc:
            raise RagIntegrityError("invalid native memory reference") from exc

    def retrieve(self, account_id, request, *, budget=None, cancelled=None, reranker=None, _transform=None):
        request = coerce(MemorySearch, request)
        guard = RequestGuard(self._budget(budget), cancelled)
        budget = self._budget(budget)
        with self._account(account_id) as account:
            raw = self.runtime.embed([request.query], query=True, guard=guard)[0] if request.mode != "lexical" or request.min_similarity is not None else b""
            with account.store.snapshot() as view:
                own_state = account.repo.namespace(view, request.scope, request.owner_id, request.timeline)
                owners = [request.owner_id]
                if request.owner_id and request.include_public:
                    owners.append("")
                if not own_state["active"]:
                    owners = []
                remaining = dict(posting_budget=budget.posting_budget, candidate_budget=budget.candidate_budget, scan_budget=budget.scan_budget)
                streams, candidates, similarities = [], {}, {}
                lexical_exhaustive = True
                states = {}
                for owner in owners:
                    guard.check()
                    state = account.repo.namespace(view, request.scope, owner, request.timeline)
                    if not state["active"]:
                        continue
                    states[owner] = state
                    jobs = account.repo.jobs(view, state)
                    limit = request.top_k if request.mode == "dense" else request.candidate_k
                    for dense in ([True] if request.mode == "dense" else [False] if request.mode == "lexical" else [True, False]):
                        result = account.index.query(view, request, owner, state, jobs, dense=dense,
                            raw_vector=raw, vector_space=account.repo.vector_space, limit=limit,
                            budget=budget, guard=guard, remaining=remaining)
                        if result is None:
                            continue
                        if not dense and (result.get("total_hits") is None or result["total_hits"] > len(result["hits"])):
                            lexical_exhaustive = False
                        rows = []
                        for hit in result["hits"]:
                            record = self._validate_projection(account, view, hit, request, owner, state)
                            rid = record["id"]
                            candidates[rid] = record
                            rows.append(rid)
                            if dense:
                                value = hit.get("dense_similarity")
                                if type(value) not in (float, int) or not -1 <= value <= 1:
                                    raise RagIntegrityError("native cosine score invalid")
                                similarities[rid] = value
                        streams.append(rows)
                had_candidates = bool(candidates)
                if (request.mode == "hybrid" or request.min_similarity is not None) and candidates:
                    # Obtain cosine for lexical-only candidates using the native engine,
                    # including global min_similarity filtering; never calculate distance here.
                    if not raw:
                        raw = self.runtime.embed([request.query], query=True, guard=guard)[0]
                    for owner, state in states.items():
                        missing = [rid for rid, record in candidates.items() if record["owner_id"] == owner and rid not in similarities]
                        if not missing:
                            continue
                        result = account.index.query(view, request, owner, state, account.repo.jobs(view, state), dense=True,
                            raw_vector=raw, vector_space=account.repo.vector_space, limit=len(missing),
                            budget=budget, guard=guard, remaining=remaining, ids=missing)
                        if result:
                            for hit in result["hits"]:
                                record = self._validate_projection(account, view, hit, request, owner, state)
                                value = hit.get("dense_similarity")
                                if type(value) not in (float, int) or not -1 <= value <= 1:
                                    raise RagIntegrityError("native cosine score invalid")
                                similarities[record["id"]] = value
                    if request.min_similarity is not None:
                        candidates = {rid: record for rid, record in candidates.items() if rid in similarities and similarities[rid] >= request.min_similarity}
                if request.mode == "dense":
                    ordered = sorted(candidates, key=lambda rid: (-similarities[rid], rid))
                    scores = similarities
                else:
                    scores = {}
                    for stream in streams:
                        for rank, rid in enumerate(stream, 1):
                            if rid in candidates:
                                scores[rid] = scores.get(rid, 0.0) + 1.0 / (60 + rank)
                    ordered = sorted(scores, key=lambda rid: (-scores[rid], rid))
                if reranker is not None:
                    if not callable(reranker):
                        raise TypeError("reranker must be callable")
                    eligible = ordered[:request.candidate_k]
                    ranked = list(reranker(request.query, [public(candidates[rid]) for rid in eligible]))
                    guard.check()
                    reranked, rerank_scores = [], {}
                    for position, entry in enumerate(ranked):
                        rid, value = entry if isinstance(entry, (tuple, list)) and len(entry) == 2 else (entry, float(len(ranked) - position))
                        if rid not in eligible or rid in rerank_scores or type(value) not in (int, float) or not math.isfinite(value):
                            raise RagIntegrityError("reranker introduced unknown/duplicate IDs or invalid scores")
                        reranked.append(rid)
                        rerank_scores[rid] = value
                    ordered = reranked
                    scores = rerank_scores
                selected_ids = ordered[:request.top_k]
                if request.mode == "lexical" and request.min_similarity is not None and had_candidates and not selected_ids and not lexical_exhaustive:
                    raise RagLimit("bounded lexical candidates do not prove absence above cosine threshold")
                records = [candidates[rid] for rid in selected_ids]
                items = []
                for record in records:
                    row = public(record)
                    row.update(similarity=similarities.get(record["id"]), score=scores[record["id"]],
                               score_kind="reranker" if reranker else "cosine" if request.mode == "dense" else "rrf", retrieval_mode=request.mode)
                    items.append(row)
                if len(encode(items)) > budget.result_bytes:
                    raise RagLimit("memory response byte budget exceeded")
                transformed = _transform(items) if _transform else None
                guard.check()
                with account.gated(guard), account.store.snapshot() as current:
                    # Lifecycle/revision barriers are checked again after any caller packing callback.
                    for record in records:
                        try:
                            latest = account.repo.record(current, record["id"])
                        except RagNotFound as exc:
                            raise RagStaleRead("memory was removed before publication") from exc
                        if latest != record:
                            raise RagStaleRead("memory version changed before publication")
                    guard.check()
                    response = dict(items=items, snapshot_version=view.native_identity().snapshot_version,
                        ranking_kind="reranked_candidates" if reranker else "exact" if request.mode == "dense" else "rrf_over_exact_candidates",
                        termination="finished", index_coverage="complete", mode=request.mode,
                        match_evidence="has_hits" if items else "postprocessed_empty" if reranker and had_candidates else "proven_none",
                        bounded_candidate_fusion=request.mode != "dense",
                        work={name: getattr(budget, name) - value for name, value in remaining.items()})
                    if _transform:
                        response["context"] = transformed
                    if len(encode(response)) > budget.result_bytes:
                        raise RagLimit("complete retrieval response exceeds byte budget")
                    return response

    def search(self, account_id, request, *, budget=None, cancelled=None, reranker=None):
        return self.retrieve(account_id, request, budget=budget, cancelled=cancelled, reranker=reranker)["items"]

    def context(self, account_id, request, *, budget=None, cancelled=None, reranker=None, **packing):
        return self.retrieve(account_id, request, budget=budget, cancelled=cancelled, reranker=reranker,
            _transform=lambda rows: pack_context(rows, **packing))

    def scoped(self, account_id, *, scope, owner_id="", timeline="main"):
        return ScopedMemoryStore(self, account_id, scope, owner_id, timeline)


class ScopedMemoryStore:
    """Trusted host-minted capability; account/namespace cannot be overridden."""
    def __init__(self, store, account_id, scope, owner, timeline):
        model = MemoryDelete(scope=scope, owner_id=owner, timeline=timeline)
        self.store, self.account_id = store, account_id
        self.namespace = dict(scope=model.scope, owner_id=model.owner_id, timeline=model.timeline)

    def _request(self, cls, value):
        if callable(getattr(value, "model_dump", None)):
            value = value.model_dump()
        elif not isinstance(value, dict):
            value = asdict(value)
        value = dict(value)
        if "npc_id" in value and value["npc_id"] is not None and value["npc_id"] != self.namespace["owner_id"]:
            raise RagNotFound("owner is outside authorized namespace")
        for key, expected in self.namespace.items():
            if key in value and value[key] != expected:
                raise RagNotFound("request is outside authorized namespace")
            value[key] = expected
        return coerce(cls, value)

    def get(self, record_id):
        record = self.store.get(self.account_id, record_id)
        if any(record[k] != v for k, v in self.namespace.items()):
            raise RagNotFound("memory is outside authorized namespace")
        return record

    def put(self, items, **options):
        if options.get("record_id"):
            self.get(options["record_id"])
        return self.store.put(self.account_id, [self._request(MemoryWrite, item) for item in items], **options)

    def search(self, request, **options):
        request = self._request(MemorySearch, request)
        # A scoped NPC does not implicitly inherit public documents.
        if request.include_public:
            raise RagConflict("scoped search must explicitly set include_public=False")
        return self.store.search(self.account_id, request, **options)

    def history(self, record_id, **options):
        self.get(record_id)
        return self.store.history(self.account_id, record_id, **options)

    def delete(self, record_id, **options):
        self.get(record_id)
        return self.store.delete(self.account_id, record_id, **options)

    def delete_matching(self, selector, **options):
        return self.store.delete_matching(self.account_id, self._request(MemoryDelete, selector), **options)

    def list(self, selector):
        return self.store.list(self.account_id, self._request(MemoryList, selector))

    def import_text(self, item, **options):
        return self.store.import_text(self.account_id, self._request(MemoryImport, item), **options)

    def context(self, request, **options):
        request = self._request(MemorySearch, request)
        if request.include_public:
            raise RagConflict("scoped context requires include_public=False")
        return self.store.context(self.account_id, request, **options)
