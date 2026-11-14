"""Bounded authoritative source/chunk transactions using a single shared Store."""
from contextlib import contextmanager
from dataclasses import asdict
import hashlib
import json
import threading
import weakref

from ._bindings import StoreError

from .native import NativeIndexProvider
from .types import (Chunk, Namespace, QueryBudget, SourceTicket, RequestGuard, MAX_TIME, MAX_VERSION,
    RagError, RagConflict, RagNotFound, RagUnsupported, RagIntegrityError, RagStaleRead,
    RagCancelled, RagDeadline, RagLimit, RagRecoveryRequired, bounded_string, integer, finite_similarity)

_ROOT = b"fqrag-source-v1/"
_GATES = weakref.WeakValueDictionary()
_GATES_LOCK = threading.Lock()


class _Gate:
    def __init__(self):
        self.lock = threading.RLock()


def _encode(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def _digest(value):
    return hashlib.sha256(_encode(value)).hexdigest()


def _hash_text(value):
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _decode(data, kind):
    if data is None:
        return None
    try:
        value = json.loads(data)
        if not isinstance(value, dict) or value.get("schema") != 1 or value.get("_type") != kind:
            raise ValueError("wrong record schema")
        return value
    except (ValueError, UnicodeError, TypeError) as exc:
        raise RagIntegrityError("invalid authoritative RAG record") from exc


class NativeRagStore:
    """One trusted tenant on an existing Store; never owns/closes that Store.

    Supports bounded replace_source, source retirement, same-snapshot exact
    retrieval and final revocation checks. It is not the WordGame MemoryStore
    compatibility facade or a fact-version/history implementation.
    """
    def __init__(self, store, index, *, tenant_id, vector_space=None, dimension=None, max_chunks=64):
        bounded_string(tenant_id, "tenant_id", 200)
        if not isinstance(index, NativeIndexProvider):
            raise TypeError("index must be NativeIndexProvider")
        integer(max_chunks, "max_chunks", 128)
        if max_chunks == 0:
            raise ValueError("max_chunks must be positive")
        if vector_space is None:
            if dimension is not None:
                raise ValueError("lexical collection cannot declare vector dimensions")
        elif not isinstance(vector_space, bytes) or not 1 <= len(vector_space) <= 512 or type(dimension) is not int or not 1 <= dimension <= 4096:
            raise ValueError("invalid vector space contract")
        self.store, self.index, self.tenant_id = store, index, tenant_id
        self.vector_space, self.dimension, self.max_chunks = vector_space, dimension, max_chunks
        self.identity = index.attach(store)
        self._uncertain = False
        with _GATES_LOCK:
            gate_key = (index.core_address, self.identity.store_token)
            self._gate = _GATES.get(gate_key)
            if self._gate is None:
                self._gate = _Gate()
                _GATES[gate_key] = self._gate
        expected = dict(schema=1, _type="tenant", tenant_id=tenant_id,
            index_space=index.index_space.hex(), vector_space=vector_space.hex() if vector_space else None,
            dimension=dimension)
        with self._gate.lock, self._errors():
            transaction = store.transaction()
            try:
                previous = _decode(transaction.get(_ROOT + b"tenant"), "tenant")
                if previous is None:
                    transaction.put(_ROOT + b"tenant", _encode(expected))
                    transaction.commit(force_reliable=True)
                elif previous != expected:
                    raise RagConflict("Store already has another RAG tenant or collection contract")
            finally:
                transaction.close()

    @contextmanager
    def _errors(self):
        try:
            yield
        except StoreError as exc:
            if exc.code in (10, 11):
                self._uncertain = True
                raise RagRecoveryRequired("commit/recovery state is unknown; reopen the Store and inspect the operation receipt") from exc
            mapping = {1: RagNotFound, 7: RagConflict, 9: RagLimit, 17: RagDeadline, 19: RagCancelled}
            raise mapping.get(exc.code, RagError)(str(exc)) from exc

    def _check(self):
        if self._uncertain:
            raise RagRecoveryRequired("reopen the Store before further RAG access")

    @contextmanager
    def _query_gate(self, guard):
        # Do not let waiting on a writer defeat query cancellation/deadlines.
        while not self._gate.lock.acquire(timeout=min(0.05, guard.remaining())):
            guard.check()
        try:
            guard.check()
            yield
        finally:
            self._gate.lock.release()

    def _namespace(self, namespace):
        self._check()
        if not isinstance(namespace, Namespace) or namespace._owner is not self:
            raise RagConflict("namespace is not authorized by this RAG instance")

    @staticmethod
    def _ns_key(namespace):
        return _ROOT + b"N/" + namespace.partition

    @staticmethod
    def _source_key(namespace, source_id):
        return _ROOT + b"S/" + namespace.partition + b"/" + _hash_text(source_id).encode("ascii")

    @staticmethod
    def _chunk_key(namespace, record_id):
        return _ROOT + b"C/" + namespace.partition + b"/" + record_id.encode("ascii")

    @staticmethod
    def _operation_key(namespace, operation_id):
        return _ROOT + b"O/" + namespace.partition + b"/" + _hash_text(operation_id).encode("ascii")

    def _state(self, view, namespace):
        state = _decode(view.get(self._ns_key(namespace)), "namespace")
        if state is None or state.get("identity") != [self.tenant_id, namespace.scope, namespace.owner_id, namespace.timeline]:
            raise RagIntegrityError("namespace identity missing or mismatched")
        if state.get("clock_id") != namespace.clock_id:
            raise RagConflict("namespace clock contract differs")
        if not state.get("active"):
            raise RagConflict("namespace is retired")
        return state

    def _source(self, view, namespace, source_id):
        source = _decode(view.get(self._source_key(namespace, source_id)), "source")
        if source is not None and source.get("source_id") != source_id:
            raise RagIntegrityError("source identity hash collision or corruption")
        return source

    def namespace(self, scope, owner_id="", *, timeline="main", clock_id="logical-tick-v1"):
        """Only trusted host code may mint handles; strings are not authentication."""
        self._check()
        for value, name, maximum, empty in ((scope, "scope", 160, False), (owner_id, "owner_id", 160, True),
                                           (timeline, "timeline", 160, False), (clock_id, "clock_id", 160, False)):
            bounded_string(value, name, maximum, empty=empty)
        identity = [self.tenant_id, scope, owner_id, timeline]
        namespace = Namespace(scope, owner_id, timeline, clock_id, _digest(identity).encode("ascii"), self)
        with self._gate.lock, self._errors():
            transaction = self.store.transaction()
            try:
                previous = _decode(transaction.get(self._ns_key(namespace)), "namespace")
                if previous is None:
                    transaction.put(self._ns_key(namespace), _encode(dict(schema=1, _type="namespace",
                        identity=identity, clock_id=clock_id, generation=1, active=True)))
                    transaction.commit(force_reliable=True)
                else:
                    self._state(transaction, namespace)
            finally:
                transaction.close()
        return namespace

    def prepare_source(self, namespace, source_id, chunks, *, expected_revision, operation_id,
                       source_revision_label=""):
        """Capture CAS/lifecycle before external embedding, with persistent dedupe."""
        self._namespace(namespace)
        bounded_string(source_id, "source_id", 200)
        bounded_string(operation_id, "operation_id", 200)
        bounded_string(source_revision_label, "source_revision_label", 160, empty=True)
        integer(expected_revision, "expected_revision")
        if not isinstance(chunks, (tuple, list)) or not 1 <= len(chunks) <= self.max_chunks or any(not isinstance(c, Chunk) for c in chunks):
            raise RagLimit("source requires a bounded nonempty list of Chunk objects")
        chunks = tuple(chunks)
        if sum(len(c.text.encode("utf-8")) for c in chunks) > 512 * 1024:
            raise RagLimit("source text exceeds 512 KiB budget")
        content_digest = _digest(dict(source_id=source_id, label=source_revision_label,
                                     chunks=[asdict(c) for c in chunks]))
        request_digest = _digest(["replace", source_id, expected_revision, content_digest])
        with self._gate.lock, self._errors(), self.store.snapshot() as snapshot:
            state = self._state(snapshot, namespace)
            source = self._source(snapshot, namespace, source_id)
            receipt = _decode(snapshot.get(self._operation_key(namespace, operation_id)), "operation")
            replay = receipt is not None
            if replay:
                if receipt.get("request_digest") != request_digest:
                    raise RagConflict("operation ID was used with another request")
                self._require_live_receipt(source, state, receipt)
            elif source is not None and source.get("deleted"):
                raise RagUnsupported("retired source IDs cannot be recreated; use a new source identity")
            elif (source["revision"] if source else 0) != expected_revision:
                raise RagConflict("stale source revision")
            return SourceTicket(namespace, source_id, chunks, source_revision_label, expected_revision,
                state["generation"], source["generation"] if source else 1, operation_id,
                request_digest, content_digest, bool(source and source.get("content_digest") == content_digest), replay, self)

    @staticmethod
    def _require_live_receipt(source, state, receipt):
        if (source is None or source.get("deleted") or source["revision"] != receipt.get("source_revision") or
            source["generation"] != receipt.get("source_generation") or state["generation"] != receipt.get("namespace_generation")):
            raise RagConflict("operation was applied but its source has since changed or retired")

    def _chunks(self, view, namespace, source):
        ids = source.get("chunk_ids")
        if not isinstance(ids, list) or len(ids) > 128:
            raise RagIntegrityError("invalid authoritative source chunk directory")
        records = []
        for record_id in ids:
            record = _decode(view.get(self._chunk_key(namespace, record_id)), "chunk")
            if record is None or record.get("source_id") != source["source_id"] or record.get("source_version") != source["revision"]:
                raise RagIntegrityError("source/chunk publication mismatch")
            records.append(record)
        return records

    @staticmethod
    def _public_source(source, *, replay=False):
        return dict(source_id=source["source_id"], revision=source["revision"],
            source_revision_label=source["source_revision_label"], generation=source["generation"],
            chunk_ids=list(source["chunk_ids"]), deleted=source["deleted"], replay=replay)

    def commit_source(self, ticket, *, vectors=None, vector_space=None):
        self._check()
        if not isinstance(ticket, SourceTicket) or ticket._owner is not self:
            raise RagConflict("ticket belongs to another RAG instance")
        namespace = ticket.namespace
        self._namespace(namespace)
        raw_vectors = None
        if not ticket.unchanged and not ticket.replay:
            if self.vector_space is None:
                if vectors is not None or vector_space is not None:
                    raise ValueError("lexical collection does not accept vectors")
                raw_vectors = [b""] * len(ticket.chunks)
            else:
                if vector_space != self.vector_space:
                    raise RagConflict("embedding batch vector space differs from collection")
                if not isinstance(vectors, (tuple, list)) or len(vectors) != len(ticket.chunks):
                    raise ValueError("one complete vector per chunk is required")
                raw_vectors = [self.index.vector(v, self.dimension) for v in vectors]
        with self._gate.lock, self._errors():
            transaction = self.store.transaction()
            try:
                state = self._state(transaction, namespace)
                if state["generation"] != ticket.namespace_generation:
                    raise RagConflict("namespace changed during embedding")
                source = self._source(transaction, namespace, ticket.source_id)
                receipt_key = self._operation_key(namespace, ticket.operation_id)
                receipt = _decode(transaction.get(receipt_key), "operation")
                if receipt is not None:
                    if receipt.get("request_digest") != ticket.request_digest:
                        raise RagConflict("operation ID conflicts")
                    self._require_live_receipt(source, state, receipt)
                    return self._public_source(source, replay=True)
                if (source["revision"] if source else 0) != ticket.expected_revision or (
                    source is not None and (source["deleted"] or source["generation"] != ticket.source_generation)):
                    raise RagConflict("source changed or retired during embedding")
                unchanged = source is not None and source.get("content_digest") == ticket.content_digest
                if not unchanged:
                    if raw_vectors is None:
                        raise RagConflict("prepared no-op no longer matches the source")
                    revision = (source["revision"] if source else 0) + 1
                    if revision > MAX_VERSION:
                        raise RagLimit("source revision exhausted")
                    previous_records = self._chunks(transaction, namespace, source) if source else []
                    new_records = []
                    for position, chunk in enumerate(ticket.chunks):
                        record_id = _digest([namespace.partition.hex(), ticket.source_id, revision, position])[:32]
                        record = dict(schema=1, _type="chunk", id=record_id,
                            scope=namespace.scope, owner_id=namespace.owner_id, timeline=namespace.timeline, clock_id=namespace.clock_id,
                            source_id=ticket.source_id, source_version=revision, source_revision_label=ticket.source_revision_label,
                            source_generation=ticket.source_generation, namespace_generation=state["generation"], projection_version=1,
                            content_hash=_hash_text(chunk.text), canonical_key=self._chunk_key(namespace, record_id).decode("ascii"),
                            **asdict(chunk))
                        transaction.put(self._chunk_key(namespace, record_id), _encode(record))
                        self.index.stage_chunk(transaction, namespace, record, raw_vectors[position], self.vector_space or b"")
                        new_records.append(record)
                    for record in previous_records:
                        self.index.erase_chunk(transaction, namespace, record)
                        transaction.delete(self._chunk_key(namespace, record["id"]))
                    source = dict(schema=1, _type="source", source_id=ticket.source_id, revision=revision,
                        generation=ticket.source_generation, namespace_generation=state["generation"],
                        content_digest=ticket.content_digest, source_revision_label=ticket.source_revision_label,
                        deleted=False, chunk_ids=[r["id"] for r in new_records])
                    transaction.put(self._source_key(namespace, ticket.source_id), _encode(source))
                receipt = dict(schema=1, _type="operation", operation="replace", request_digest=ticket.request_digest,
                    source_id=ticket.source_id, source_revision=source["revision"], source_generation=source["generation"],
                    namespace_generation=state["generation"])
                transaction.put(receipt_key, _encode(receipt))
                transaction.commit(force_reliable=True)
                return self._public_source(source)
            finally:
                transaction.close()

    def replace_source(self, namespace, source_id, chunks, *, expected_revision, operation_id,
                       source_revision_label="", vectors=None, vector_space=None):
        ticket = self.prepare_source(namespace, source_id, chunks, expected_revision=expected_revision,
            operation_id=operation_id, source_revision_label=source_revision_label)
        return self.commit_source(ticket, vectors=vectors, vector_space=vector_space)

    def delete_source(self, namespace, source_id, *, expected_revision, operation_id):
        self._namespace(namespace)
        bounded_string(source_id, "source_id", 200)
        bounded_string(operation_id, "operation_id", 200)
        integer(expected_revision, "expected_revision")
        request_digest = _digest(["delete", source_id, expected_revision])
        with self._gate.lock, self._errors():
            transaction = self.store.transaction()
            try:
                state = self._state(transaction, namespace)
                source = self._source(transaction, namespace, source_id)
                receipt_key = self._operation_key(namespace, operation_id)
                receipt = _decode(transaction.get(receipt_key), "operation")
                if receipt is not None:
                    if receipt.get("request_digest") != request_digest or source is None or not source["deleted"] or source["revision"] != receipt.get("source_revision"):
                        raise RagConflict("delete operation receipt conflicts")
                    return self._public_source(source, replay=True)
                if source is None:
                    raise RagNotFound("source does not exist")
                if source["deleted"] or source["revision"] != expected_revision:
                    raise RagConflict("source revision differs or source is already retired")
                if max(source["revision"], source["generation"]) >= MAX_VERSION:
                    raise RagLimit("source lifecycle counter exhausted")
                for record in self._chunks(transaction, namespace, source):
                    self.index.erase_chunk(transaction, namespace, record)
                    transaction.delete(self._chunk_key(namespace, record["id"]))
                source.update(revision=source["revision"] + 1, generation=source["generation"] + 1,
                              chunk_ids=[], deleted=True)
                transaction.put(self._source_key(namespace, source_id), _encode(source))
                transaction.put(receipt_key, _encode(dict(schema=1, _type="operation", operation="delete",
                    request_digest=request_digest, source_id=source_id, source_revision=source["revision"],
                    source_generation=source["generation"], namespace_generation=state["generation"])))
                transaction.commit(force_reliable=True)
                return self._public_source(source)
            finally:
                transaction.close()

    def get_source(self, namespace, source_id, *, include_deleted=False):
        self._namespace(namespace)
        bounded_string(source_id, "source_id", 200)
        if type(include_deleted) is not bool:
            raise ValueError("include_deleted must be bool")
        with self._gate.lock, self._errors(), self.store.snapshot() as snapshot:
            self._state(snapshot, namespace)
            source = self._source(snapshot, namespace, source_id)
            if source is None or (source["deleted"] and not include_deleted):
                raise RagNotFound("source does not exist or is retired")
            return self._public_source(source)

    def operation_status(self, namespace, operation_id):
        self._namespace(namespace)
        bounded_string(operation_id, "operation_id", 200)
        with self._gate.lock, self._errors(), self.store.snapshot() as snapshot:
            state = self._state(snapshot, namespace)
            receipt = _decode(snapshot.get(self._operation_key(namespace, operation_id)), "operation")
            if receipt is None:
                return {"status": "not_found"}
            source = self._source(snapshot, namespace, receipt["source_id"])
            status = "retired" if source is not None and source["deleted"] else "superseded"
            if (source is not None and not source["deleted"] and source["revision"] == receipt["source_revision"] and
                source["generation"] == receipt["source_generation"] and state["generation"] == receipt["namespace_generation"]):
                status = "applied_current"
            return dict(status=status, operation=receipt["operation"], source_id=receipt["source_id"],
                        source_revision=receipt["source_revision"], request_digest=receipt["request_digest"])

    def _materialize(self, snapshot, namespace, hit, as_of, source_id, mode):
        document = hit.get("document")
        try:
            record_id = document["id"].decode("ascii")
            if len(record_id) != 32 or any(c not in "0123456789abcdef" for c in record_id):
                raise ValueError("invalid record id")
            record = _decode(snapshot.get(self._chunk_key(namespace, record_id)), "chunk")
            if record is None:
                raise ValueError("missing canonical chunk")
            source = self._source(snapshot, namespace, record["source_id"])
            if (source is None or source["deleted"] or record_id not in source["chunk_ids"] or
                source["revision"] != record["source_version"] or source["generation"] != record["source_generation"]):
                raise ValueError("source publication mismatch")
            expected = dict(scope=namespace.scope, owner_id=namespace.owner_id, timeline=namespace.timeline, clock_id=namespace.clock_id)
            if any(record.get(k) != v for k, v in expected.items()):
                raise ValueError("unauthorized namespace")
            if source_id is not None and record["source_id"] != source_id:
                raise ValueError("unauthorized source")
            if not record["known_at"] <= as_of or (record["expires_at"] is not None and as_of >= record["expires_at"]):
                raise ValueError("chunk is not time-visible")
            if (record["content_hash"] != _hash_text(record["text"]) or
                record["projection_version"] != document["projection_version"] or
                source["revision"] != document["domain_revision"] or source["revision"] != document["source_revision"] or
                source["generation"] != document["lifecycle_generation"] or document["deleted"] or
                document["source_locator"] != self._chunk_key(namespace, record_id) or
                document["texts"].get("body", (None,))[0] != record["text"] or
                document["texts"].get("title", (None,))[0] != record["title"]):
                raise ValueError("Index projection differs from authoritative chunk")
            for field in ("scope", "owner_id", "timeline", "clock_id", "known_at", "source_id", "source_version",
                          "source_generation", "namespace_generation", "content_hash"):
                actual = document["metadata"].get(field)
                if actual != record[field] or type(actual) is not type(record[field]):
                    raise ValueError("Index metadata differs from authoritative chunk")
            for field, expected_value in (("has_expiry", record["expires_at"] is not None),
                                          ("expires_at", record["expires_at"] or 0),
                                          ("kind", record["kind"]), ("searchable", True)):
                actual = document["metadata"].get(field)
                if actual != expected_value or type(actual) is not type(expected_value):
                    raise ValueError("Index visibility metadata differs from authoritative chunk")
            if document["vector_space"] != (self.vector_space or b""):
                raise ValueError("Index vector space differs")
            result = {k: v for k, v in record.items() if k not in ("schema", "_type", "canonical_key")}
            result["score"] = hit["score"]
            if mode == "dense":
                similarity = hit.get("dense_similarity")
                finite_similarity(similarity)
                if similarity is None:
                    raise ValueError("dense result omitted cosine similarity")
                result["similarity"] = similarity
            return record, result
        except (KeyError, TypeError, ValueError, AttributeError, UnicodeError) as exc:
            raise RagIntegrityError("invalid or unauthorized Index projection") from exc

    def search(self, namespace, *, as_of, query_vector=None, query_space=None, text="", mode="dense",
               source_id=None, top_k=5, min_similarity=None, strict_statistics=True, include_public=False,
               budget=None, cancelled=None):
        self._namespace(namespace)
        integer(as_of, "as_of", MAX_TIME)
        integer(top_k, "top_k", 30)
        if not top_k:
            raise ValueError("top_k must be positive")
        if source_id is not None:
            bounded_string(source_id, "source_id", 200)
        finite_similarity(min_similarity)
        if type(strict_statistics) is not bool or type(include_public) is not bool:
            raise ValueError("statistics/public policy flags must be bool")
        if include_public:
            raise RagUnsupported("cross-partition owner/public union is not implemented")
        if mode not in ("dense", "lexical"):
            raise RagUnsupported("ANN, hybrid and reranking are not implemented by this path")
        if mode == "lexical":
            if query_vector is not None or query_space is not None or min_similarity is not None:
                raise ValueError("lexical requests cannot include vector fields")
            if strict_statistics:
                raise RagUnsupported("Index v1 does not support strict time-filtered BM25 statistics")
            bounded_string(text, "query text", 1800)
            raw_vector = b""
        else:
            if text:
                raise ValueError("dense exact requests cannot include query text")
            if self.vector_space is None:
                raise RagUnsupported("collection has no vector space")
            if query_space != self.vector_space:
                raise RagConflict("query vector space differs from collection")
            raw_vector = self.index.vector(query_vector, self.dimension)
        budget = budget or QueryBudget()
        if not isinstance(budget, QueryBudget):
            raise TypeError("budget must be QueryBudget")
        guard = RequestGuard(budget, cancelled)
        guard.check()
        with self._errors(), self.store.snapshot() as snapshot:
            with self._query_gate(guard):
                state = self._state(snapshot, namespace)
            try:
                native = self.index.search(snapshot, namespace, as_of=as_of, source_id=source_id, mode=mode,
                    query_vector=raw_vector, vector_space=self.vector_space or b"", text=text, top_k=top_k,
                    min_similarity=min_similarity, budget=budget, guard=guard)
            except StoreError:
                guard.check()
                raise
            guard.check()
            records, items, seen, result_bytes = [], [], set(), 0
            for hit in native["hits"]:
                guard.check()
                record, item = self._materialize(snapshot, namespace, hit, as_of, source_id, mode)
                if record["id"] in seen or record["namespace_generation"] != state["generation"]:
                    raise RagIntegrityError("duplicate hit or namespace generation mismatch")
                if min_similarity is not None and item["similarity"] < min_similarity:
                    raise RagIntegrityError("Index result violates cosine threshold")
                seen.add(record["id"])
                result_bytes += len(_encode(item))
                if result_bytes > budget.result_bytes:
                    raise RagLimit("materialized response exceeds byte budget")
                records.append(record)
                items.append(item)
            with self._query_gate(guard), self.store.snapshot() as current:
                guard.check()
                latest_state = self._state(current, namespace)
                if latest_state["generation"] != state["generation"]:
                    raise RagStaleRead("namespace changed before result publication")
                for record in records:
                    source = self._source(current, namespace, record["source_id"])
                    present = _decode(current.get(self._chunk_key(namespace, record["id"])), "chunk")
                    if (source is None or source["deleted"] or source["revision"] != record["source_version"] or
                        source["generation"] != record["source_generation"] or record["id"] not in source["chunk_ids"] or
                        present != record):
                        raise RagStaleRead("source or chunk changed/revoked before result publication")
                guard.check()
                return dict(items=items, snapshot_version=native["snapshot_version"],
                    mutation_sequence=native["mutation_sequence"], ranking_kind="exact", termination="finished",
                    index_coverage="complete", match_evidence="has_hits" if items else "proven_none",
                    statistics_policy="partition_unfiltered" if mode == "lexical" else None,
                    strategy=native["strategy"])

    def put_fact(self, *args, **kwargs):
        raise RagUnsupported("fact lineage/version history is not implemented in the bounded source path")

    def retire_namespace(self, *args, **kwargs):
        raise RagUnsupported("durable bulk namespace retirement is not implemented")
