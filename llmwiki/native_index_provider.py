"""Domain-owned projections and materialization over the shared native index."""

import hashlib
from functools import wraps

try:
    from ..bindings.python.fengqudb_index import NativeIndex, QueryControl
    from ..bindings.python.fengqudb_index_wire import (
        Document, Mutation, Phrase, Predicate, Query, UInt, decode_result, encode_mutation, encode_query, vector)
except ImportError:
    try:
        from bindings.python.fengqudb_index import NativeIndex, QueryControl
        from bindings.python.fengqudb_index_wire import (
            Document, Mutation, Phrase, Predicate, Query, UInt, decode_result, encode_mutation, encode_query, vector)
    except ImportError:
        from fengqudb_index import NativeIndex, QueryControl
        from fengqudb_index_wire import (
            Document, Mutation, Phrase, Predicate, Query, UInt, decode_result, encode_mutation, encode_query, vector)

from .index_provider import IndexDefinition, normalize_search_result
from .storage import encode
from .limits import ConflictError


class NativeProjectionError(RuntimeError):
    pass


def _hash(value):
    return hashlib.sha256(value if isinstance(value, bytes) else encode(value)).hexdigest()


def atomic_stage(method):
    @wraps(method)
    def checked(self, tx, *args, **kwargs):
        try:
            return method(self, tx, *args, **kwargs)
        except BaseException:
            NativeIndex.cancel_failed_stage(tx.handle)
            raise
    return checked


class NativeIndexProvider:
    """No tokenizer, scorer, vector math or search algorithm lives in this adapter.

    index_space is fixed by the host. A default partition is derived from the
    Wiki namespace prefix. The caller's existing View owns every transaction.
    """

    atomic_updates = True

    def __init__(self, library=None, *, index_space=b"fengqu-wiki-v1", partition=None,
                 client=None, expected_store_token=None, control=None):
        if not isinstance(index_space, bytes) or not 1 <= len(index_space) <= 128:
            raise ValueError("index_space requires 1..128 bytes")
        if partition is not None and (not isinstance(partition, bytes) or not 1 <= len(partition) <= 512):
            raise ValueError("partition requires 1..512 bytes")
        self.client = client if client is not None else NativeIndex(library)
        self.index_space, self.partition = index_space, partition
        self.expected_store_token, self.control = expected_store_token, control
        configuration = {"space": index_space.hex(), "partition": partition.hex() if partition else None,
                         "protocol": 1, "analyzer": "fqidx-utf8-v1"}
        self.definition = IndexDefinition("native-" + _hash(configuration)[:32], 1, 1)

    def _instance(self, view):
        return {"index_space": self.index_space,
                "partition": self.partition or hashlib.sha256(view.prefix).hexdigest().encode("ascii")}

    def _apply(self, tx, **arguments):
        try:
            request = encode_mutation(**self._instance(tx), **arguments)
            self.client.apply(tx.handle, request, expected_store_token=self.expected_store_token)
        except BaseException:
            NativeIndex.cancel_failed_stage(tx.handle)
            raise

    def _query(self, view, *, control=None, **arguments):
        request = encode_query(**self._instance(view), **arguments)
        return decode_result(self.client.query(view.handle, request,
            expected_store_token=self.expected_store_token, control=control or self.control))

    @staticmethod
    def _revision_token(tx, revision):
        # v1 uses uint64 revisions. Preserve full external hashes and allocate
        # stable domain-local tokens instead of truncating a SHA-256 digest.
        key = "native-index/revision/" + revision
        token = tx.get(key)
        if token is None:
            token = tx.get("native-index/revision-sequence", 0) + 1
            if token >= 2 ** 64:
                raise NativeProjectionError("domain revision token space exhausted")
            tx.put(key, token)
            tx.put("native-index/revision-sequence", token)
        return token

    @staticmethod
    def _fingerprint(metadata):
        ignored = {"projection_version", "lifecycle_generation", "domain_revision_token",
                   "source_revision_token", "input_digest"}
        return _hash({key: value for key, value in metadata.items() if key not in ignored})

    @staticmethod
    def _native_metadata(metadata):
        result = {"kind": metadata["kind"], "visible": metadata["visible"],
                  "domain_revision_hash": metadata["revision"], "content_hash": metadata["content_hash"],
                  "projection_digest": bytes.fromhex(metadata["input_digest"])}
        for name in ("source_id", "page_id", "publication_state"):
            if name in metadata:
                result[name] = metadata[name]
        for name in ("source_publication_generation", "publication_generation", "build_generation"):
            if name in metadata:
                result[name] = UInt(metadata[name])
        return result

    @atomic_stage
    def upsert(self, tx, document_id, document):
        if not isinstance(document_id, str) or not 1 <= len(document_id.encode()) <= 1024:
            raise NativeProjectionError("invalid Wiki document id")
        metadata = {key: value for key, value in document.items() if key != "text"}
        body = document["text"]
        metadata["content_hash"] = _hash(body.encode("utf-8"))
        metadata["visible"] = True
        if metadata["kind"] == "chunk":
            reference = metadata["citations"][0]
            locator = {"kind": "raw", "source_id": metadata["source_id"], "revision": metadata["revision"],
                       "start": reference["start"], "end": reference["end"]}
        elif metadata["kind"] == "page":
            state = self.get_page_state(tx, metadata["page_id"])
            if not state or state["stale"]:
                raise NativeProjectionError("page projection requires a ready publication state")
            metadata.update({name: state[name] for name in ("publication_generation", "build_generation")})
            metadata["publication_state"] = "ready"
            locator = {"kind": "page", "page_id": metadata["page_id"], "revision": metadata["revision"]}
        else:
            raise NativeProjectionError("unsupported Wiki document kind")
        metadata["locator"] = locator
        metadata["input_digest"] = self._fingerprint(metadata)
        previous = tx.get("native-index/projection/" + document_id)
        if previous and not previous["deleted"] and previous["input_digest"] == metadata["input_digest"]:
            return
        version = (previous or {}).get("projection_version", 0)
        generation = (previous or {}).get("lifecycle_generation", 1)
        if previous and previous["deleted"]:
            generation += 1
        metadata.update(projection_version=version + 1, lifecycle_generation=generation,
                        domain_revision_token=self._revision_token(tx, metadata["revision"]))
        metadata["source_revision_token"] = (metadata["domain_revision_token"] if metadata["kind"] == "chunk" else 0)
        native = Document(document_id.encode(), metadata["projection_version"], metadata["domain_revision_token"],
            generation, texts={"title": (metadata["title"], 2.0), "body": body},
            metadata=self._native_metadata(metadata), source_locator=encode(locator),
            source_revision=metadata["source_revision_token"], payload=encode({"schema": 1,
                "wiki_document_id": document_id, "revision": metadata["revision"], "content_hash": metadata["content_hash"]}))
        self._apply(tx, operation=Mutation.UPSERT, expected_projection_version=version, document=native)
        tx.put("native-index/document/" + document_id, metadata)
        tx.put("native-index/projection/" + document_id, {"projection_version": version + 1,
            "lifecycle_generation": generation, "input_digest": metadata["input_digest"], "deleted": False})

    @atomic_stage
    def delete(self, tx, document_id):
        previous = tx.get("native-index/projection/" + document_id)
        if previous is None or previous["deleted"]:
            return
        self._apply(tx, operation=Mutation.ERASE, id=document_id.encode(),
                    expected_projection_version=previous["projection_version"],
                    next_projection_version=previous["projection_version"] + 1,
                    lifecycle_generation=previous["lifecycle_generation"])
        tx.delete("native-index/document/" + document_id)
        tx.put("native-index/projection/" + document_id, dict(previous,
            projection_version=previous["projection_version"] + 1, deleted=True))

    @atomic_stage
    def set_vector(self, tx, document_id, values, vector_space, expected_projection_version):
        metadata = self.get_document(tx, document_id)
        if (not metadata or not metadata["visible"] or type(expected_projection_version) is not int or
                metadata["projection_version"] != expected_projection_version):
            raise ConflictError("vector target projection changed or is not visible")
        packed = vector(values)
        if not packed or not isinstance(vector_space, str) or not 1 <= len(vector_space.encode()) <= 512:
            raise ValueError("vector requires a nonempty model/version space and 1..4096 dimensions")
        changed = dict(metadata, vector_hash=_hash(packed), vector_space=vector_space)
        changed["input_digest"] = self._fingerprint(changed)
        if changed["input_digest"] == metadata["input_digest"]:
            return metadata["projection_version"]
        changed["projection_version"] += 1
        locator = metadata["locator"]
        if locator["kind"] == "raw":
            source = tx.get("source/" + locator["source_id"])
            raw = tx.get("raw/" + locator["source_id"] + "/" + locator["revision"])
            if (not source or source["deleted"] or not raw or source["revision"] != locator["revision"] or
                    source["version"] != metadata["citations"][0]["source_version"]):
                raise ConflictError("vector source was revoked")
            body = raw["content"][locator["start"]:locator["end"]]
        else:
            page = tx.get("page/" + locator["page_id"])
            if not page or page["stale"] or page["revision"] != locator["revision"]:
                raise ConflictError("vector page is no longer current")
            body = page["markdown"]
        if _hash(body.encode()) != metadata["content_hash"]:
            raise NativeProjectionError("vector target body hash changed")
        # Native PATCH is metadata-only. Changing an embedding requires full UPSERT;
        # it retains identity/lifecycle and lets the shared Index reuse unchanged postings.
        document = Document(document_id.encode(), changed["projection_version"], metadata["domain_revision_token"],
            metadata["lifecycle_generation"], texts={"title": (metadata["title"], 2.0), "body": body},
            metadata=self._native_metadata(changed), vector_f32_le=packed, vector_space=vector_space.encode(),
            source_locator=encode(locator), source_revision=metadata["source_revision_token"],
            payload=encode({"schema": 1, "wiki_document_id": document_id, "revision": metadata["revision"],
                           "content_hash": metadata["content_hash"]}))
        self._apply(tx, operation=Mutation.UPSERT, document=document,
                    expected_projection_version=expected_projection_version)
        tx.put("native-index/document/" + document_id, changed)
        previous = tx.get("native-index/projection/" + document_id)
        tx.put("native-index/projection/" + document_id, dict(previous,
            projection_version=changed["projection_version"], input_digest=changed["input_digest"]))
        return changed["projection_version"]

    @atomic_stage
    def set_page_state(self, tx, page_id, state):
        key = "native-index/page/" + page_id
        old = tx.get(key, {"publication_generation": 0, "build_generation": 0})
        if state is None:
            tx.put(key, dict(old, stale=True, deleted=True, publication_state="deleted"))
            return
        if state["stale"]:
            current = dict(old, **state, deleted=False, publication_state="dirty")
        else:
            same_publication = (not old.get("stale", True) and not old.get("deleted", False) and
                                old.get("revision") == state["revision"] and
                                old.get("build_generation") == state.get("build_generation", 0))
            current = dict(state, deleted=False, publication_state="ready",
                publication_generation=old["publication_generation"] + int(not same_publication),
                build_generation=state.get("build_generation", 0))
        tx.put(key, current)
        # Ready state is projected by the following upsert, after its body is
        # available. Dirty is immediate metadata-only patch: no retokenization.
        document_id = "p-" + page_id
        metadata = self.get_document(tx, document_id)
        if not state["stale"] or metadata is None:
            return
        changed = dict(metadata, visible=False, publication_state="dirty",
                       publication_generation=current["publication_generation"],
                       build_generation=current.get("build_generation", 0))
        changed["input_digest"] = self._fingerprint(changed)
        if changed["input_digest"] == metadata["input_digest"]:
            return
        previous = tx.get("native-index/projection/" + document_id)
        changed["projection_version"] = previous["projection_version"] + 1
        patch = Document(document_id.encode(), metadata=self._native_metadata(changed), replace_metadata=True)
        self._apply(tx, operation=Mutation.PATCH, id=document_id.encode(), document=patch,
                    expected_projection_version=previous["projection_version"],
                    next_projection_version=changed["projection_version"],
                    lifecycle_generation=previous["lifecycle_generation"])
        tx.put("native-index/document/" + document_id, changed)
        tx.put("native-index/projection/" + document_id, dict(previous,
            projection_version=changed["projection_version"], input_digest=changed["input_digest"]))

    def get_page_state(self, view, page_id):
        state = view.get("native-index/page/" + page_id)
        return None if state and state.get("deleted") else state

    def get_document(self, view, document_id):
        return view.get("native-index/document/" + document_id)

    def statistics(self, view):
        result = self._query(view, operation=Query.STATS)
        return {"count": result["record_count"], "vector_count": result["vector_count"],
                "length": None, "length_unit": "not_exposed_by_native_v1"}

    def _materialize(self, view, hit):
        indexed = hit["document"]
        try:
            document_id = indexed["id"].decode("utf-8")
        except UnicodeError as exc:
            raise NativeProjectionError("native hit has an invalid Wiki document id") from exc
        metadata = self.get_document(view, document_id)
        if (not metadata or indexed["deleted"] or not metadata["visible"] or
                any(indexed[name] != metadata[name] for name in ("projection_version", "lifecycle_generation")) or
                indexed["domain_revision"] != metadata["domain_revision_token"] or
                indexed["source_revision"] != metadata["source_revision_token"] or
                indexed["source_locator"] != encode(metadata["locator"]) or
                indexed["metadata"].get("projection_digest") != bytes.fromhex(metadata["input_digest"]) or
                indexed["metadata"].get("domain_revision_hash") != metadata["revision"] or
                indexed["metadata"].get("content_hash") != metadata["content_hash"]):
            raise NativeProjectionError("native hit and domain projection versions disagree")
        locator = metadata["locator"]
        if locator["kind"] == "raw":
            current = view.get("source/" + locator["source_id"])
            raw = view.get("raw/" + locator["source_id"] + "/" + locator["revision"])
            if (not current or current["deleted"] or current["revision"] != locator["revision"] or not raw or
                    current["version"] != metadata["citations"][0]["source_version"]):
                raise NativeProjectionError("native hit points to a revoked source")
            start, end = locator["start"], locator["end"]
            if type(start) is not int or type(end) is not int or not 0 <= start <= end <= len(raw["content"]):
                raise NativeProjectionError("native hit has an invalid raw locator")
            body = raw["content"][start:end]
        else:
            page = view.get("page/" + locator["page_id"])
            state = self.get_page_state(view, locator["page_id"])
            if (not page or page["stale"] or page["revision"] != locator["revision"] or not state or state["stale"] or
                    state["publication_generation"] != metadata["publication_generation"]):
                raise NativeProjectionError("native hit points to a dirty or retired page")
            body = page["markdown"]
        if _hash(body.encode("utf-8")) != metadata["content_hash"]:
            raise NativeProjectionError("domain body hash does not match the native projection")
        private = {"locator", "input_digest", "domain_revision_token", "source_revision_token", "content_hash", "visible"}
        positions = []
        for position in hit.get("positions", []):
            field = position["field"]
            original = metadata["title"] if field == "title" else body if field == "body" else None
            if original is None:
                raise NativeProjectionError("native position references an unknown field")
            original = original.encode("utf-8")
            begin, end, snippet_begin = position["begin"], position["end"], position["snippet_begin"]
            snippet = position["snippet"]
            if (not 0 <= begin <= end <= len(original) or not 0 <= snippet_begin <= len(original) or
                    original[snippet_begin:snippet_begin + len(snippet)] != snippet):
                raise NativeProjectionError("native positions disagree with the same-version domain text")
            try:
                # Validate UTF-8 boundaries for matches too; exposed offsets remain byte offsets.
                original[:begin].decode("utf-8")
                original[begin:end].decode("utf-8")
                rendered = snippet.decode("utf-8")
            except UnicodeError as exc:
                raise NativeProjectionError("native position splits a UTF-8 character") from exc
            positions.append(dict(position, snippet=rendered, offset_unit="utf8_bytes"))
        return {key: value for key, value in metadata.items() if key not in private} | {
            "id": document_id, "score": hit["score"], "dense_similarity": hit["dense_similarity"],
            "matched_fields": hit["matched_fields"], "positions": positions, "details": hit.get("details", []),
            "text": body[:4000], "text_truncated": len(body) > 4000}

    @staticmethod
    def _filters(filters):
        if filters is None:
            return None
        if not isinstance(filters, dict) or len(filters) > 4:
            raise ValueError("filters require a bounded object")
        predicates = []
        for field, value in filters.items():
            if field not in ("kind", "source_id", "page_id", "publication_state"):
                raise ValueError("unsupported business filter")
            values = value if isinstance(value, list) else [value]
            if not 1 <= len(values) <= 64 or any(not isinstance(v, str) or not v or len(v.encode()) > 128 for v in values):
                raise ValueError("filter requires bounded string values")
            predicates.append(Predicate(7 if isinstance(value, list) else 1, field, tuple(values)))
        return Predicate(9, children=tuple(predicates)) if predicates else None

    def search(self, view, query, *, limit=10, posting_budget=12000, candidate_budget=1024, where=None,
               filters=None, scan_budget=100000, memory_budget=32 * 1024 * 1024, timeout_ms=30000, cancelled=None,
               phrases=(), prefix_last=False, include_positions=False, snippet_bytes=None, include_explain=False,
               term_budget=None, statistics_policy=0, mode=0, vector_f32_le=b"", vector_space=b"",
               min_similarity=None, ef_search=None, match_all_terms=False, excluded_terms=(), scoring_fields=()):
        visible = Predicate(1, "visible", (True,))
        for name, value, maximum in (("scan_budget", scan_budget, 1000000),
            ("memory_budget", memory_budget, 512 * 1024 * 1024), ("timeout_ms", timeout_ms, 300000),
            ("posting_budget", posting_budget, 100000), ("candidate_budget", candidate_budget, 4096)):
            if type(value) is not int or not 1 <= value <= maximum:
                raise ValueError(name + " exceeds the Wiki query bounds")
        if isinstance(vector_space, str):
            vector_space = vector_space.encode("utf-8")
        predicates = [p for p in (visible, where, self._filters(filters)) if p is not None]
        predicate = Predicate(9, children=tuple(predicates)) if len(predicates) > 1 else visible
        if not isinstance(phrases, (list, tuple)) or len(phrases) > 128:
            raise ValueError("phrases require a bounded array")
        phrases = tuple(Phrase(**p) if isinstance(p, dict) else Phrase(p) if isinstance(p, str) else p for p in phrases)
        control = QueryControl(cancelled) if cancelled is not None else None
        result = self._query(view, operation=Query.SEARCH, text=query, limit=limit, where=predicate,
            posting_budget=posting_budget, candidate_budget=candidate_budget, allow_partial=True, control=control,
            scan_budget=scan_budget, memory_budget=memory_budget, timeout_ms=timeout_ms, phrases=phrases,
            prefix_last=prefix_last, include_positions=include_positions, snippet_bytes=snippet_bytes,
            include_explain=include_explain, term_budget=term_budget, statistics_policy=statistics_policy,
            mode=mode, vector_f32_le=vector_f32_le, vector_space=vector_space, min_similarity=min_similarity,
            ef_search=ef_search, match_all_terms=match_all_terms, excluded_terms=excluded_terms, scoring_fields=scoring_fields)
        terminations = {0: "finished", 1: "posting_limit", 2: "candidate_limit", 3: "scan_limit",
                        4: "memory_limit", 5: "deadline", 6: "cancelled", 7: "term_limit"}
        if result["ranking_kind"] not in (0, 1) or result["index_coverage"] != 0 or result["termination"] not in terminations:
            raise NativeProjectionError("unsupported native v1 result status")
        if result["analyzer"] and result["analyzer"] != "fqidx-utf8-v1":
            raise NativeProjectionError("native Analyzer differs from the configured index space")
        termination = terminations[result["termination"]]
        evidence = {0: "not_established", 1: "has_hits", 2: "proven_none"}.get(result["match_evidence"])
        if evidence is None or (result["hits"] and evidence == "proven_none"):
            raise NativeProjectionError("native match evidence disagrees with hits")
        total = result["total_hits"]
        return normalize_search_result({"results": [self._materialize(view, hit) for hit in result["hits"]],
            "ranking_kind": "exact" if result["ranking_kind"] == 0 else "approximate", "termination": termination,
            "index_coverage": "complete", "snapshot_version": result["snapshot_version"],
            "applied_sequence": result["mutation_sequence"], "postings_visited": result["postings_visited"],
            "candidates": result["candidates"], "records_scanned": result["records_scanned"],
            "strategy": result["strategy"], "analyzer": result["analyzer"], "match_evidence": evidence,
            **{k: result.get(k, 0) for k in ("terms_visited", "statistics_scanned", "blocks_visited", "graph_visited", "layout_generation")},
            "total_hits": {"value": total, "relation": "exact" if total is not None else "unknown"},
            "truncated": termination != "finished", "degraded_reasons": [] if termination == "finished" else [termination],
            "empty_result_confirmed": evidence == "proven_none"})

    @atomic_stage
    def maintain(self, tx, **options):
        return self.client.maintain(tx.handle, **self._instance(tx), **options,
            expected_store_token=self.expected_store_token, control=self.control)
