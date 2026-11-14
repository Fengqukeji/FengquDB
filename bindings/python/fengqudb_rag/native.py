"""RAG adapter for the shared Wiki-owned FengquIndex wrapper and wire codec."""
import math

from ._bindings import ffi, vector as encode_vector

from .types import RagIntegrityError, RagLimit, RagUnsupported


class NativeIndexProvider:
    """No model, tokenizer, postings, scoring or distance implementation here."""
    def __init__(self, client, *, index_space=b"fengqu-rag-source-v1"):
        if not isinstance(index_space, bytes) or not 1 <= len(index_space) <= 128:
            raise ValueError("index_space must contain 1..128 bytes")
        if not isinstance(client, ffi.NativeIndex):
            raise TypeError("client must be the shared fengqudb_index.NativeIndex")
        self.client, self.index_space = client, index_space
        self.store_token = None
        self.core_address = None

    def attach(self, store):
        with store.borrow_native() as lease:
            identity = ffi.public_bridge_identity(lease)
            if identity.core_bridge_address != self.client.core_bridge_address or identity.core_abi != 2:
                raise RagIntegrityError("Index and RAG Store must use the same loaded Core")
            if self.store_token is not None and (self.store_token != identity.store_token or
                                                 self.core_address != identity.core_bridge_address):
                raise RagIntegrityError("provider already attached to another Store lifetime")
            self.store_token = identity.store_token
            self.core_address = identity.core_bridge_address
            return lease.identity

    @staticmethod
    def vector(values, dimension):
        if isinstance(values, bytes):
            raw = encode_vector(values)
        else:
            try:
                values = tuple(values)
            except TypeError as exc:
                raise ValueError("vector must be a sequence") from exc
            if len(values) != dimension or any(type(x) not in (int, float) or not math.isfinite(x) for x in values):
                raise ValueError("invalid vector dimensions or scalar")
            raw = encode_vector(values)
        if len(raw) != dimension * 4:
            raise ValueError("vector dimensions do not match the collection")
        return raw

    def stage_chunk(self, transaction, namespace, record, vector_data=b"", vector_space=b""):
        metadata = {name: record[name] for name in (
            "scope", "owner_id", "timeline", "clock_id", "known_at", "source_id",
            "source_version", "source_generation", "namespace_generation", "content_hash")}
        metadata.update(has_expiry=record["expires_at"] is not None,
                        expires_at=record["expires_at"] or 0, kind=record["kind"], searchable=True)
        document = ffi.Document(
            id=record["id"].encode("ascii"), projection_version=record["projection_version"],
            domain_revision=record["source_version"], lifecycle_generation=record["source_generation"],
            texts={"title": record["title"], "body": record["text"]}, metadata=metadata,
            vector_f32_le=vector_data, vector_space=vector_space,
            source_locator=record["canonical_key"].encode("ascii"),
            source_revision=record["source_version"])
        self.client.mutate(transaction, expected_store_token=self.store_token,
            index_space=self.index_space, partition=namespace.partition,
            operation=ffi.Mutation.UPSERT, expected_projection_version=0, document=document)

    def erase_chunk(self, transaction, namespace, record):
        self.client.mutate(transaction, expected_store_token=self.store_token,
            index_space=self.index_space, partition=namespace.partition, operation=ffi.Mutation.ERASE,
            id=record["id"].encode("ascii"), expected_projection_version=record["projection_version"],
            next_projection_version=record["projection_version"] + 1,
            lifecycle_generation=record["source_generation"])

    @staticmethod
    def predicate(namespace, as_of, source_id=None):
        eq = lambda key, value: ffi.Predicate(1, key, (value,))
        predicates = [eq("scope", namespace.scope), eq("owner_id", namespace.owner_id),
            eq("timeline", namespace.timeline), eq("clock_id", namespace.clock_id),
            eq("searchable", True), ffi.Predicate(4, "known_at", (as_of,)),
            ffi.Predicate(10, children=(eq("has_expiry", False), ffi.Predicate(5, "expires_at", (as_of,))))]
        if source_id is not None:
            predicates.append(eq("source_id", source_id))
        return ffi.Predicate(9, children=tuple(predicates))

    def search(self, snapshot, namespace, *, as_of, source_id, mode, query_vector, vector_space,
               text, top_k, min_similarity, budget, guard):
        if mode not in ("dense", "lexical"):
            raise RagUnsupported("only shared Index v1 dense exact and explicit partition lexical are available")
        kwargs = dict(index_space=self.index_space, partition=namespace.partition,
            operation=ffi.Query.SEARCH, where=self.predicate(namespace, as_of, source_id), limit=top_k,
            allow_partial=False, expected_store_token=self.store_token,
            posting_budget=budget.posting_budget, candidate_budget=budget.candidate_budget,
            scan_budget=budget.scan_budget, memory_budget=budget.memory_budget, timeout_ms=budget.timeout_ms,
            control=ffi.QueryControl(guard.stopped))
        if mode == "dense":
            # Native v1 explicitly rejects text/match_all_terms in dense Search.
            kwargs.update(mode=1, vector_f32_le=query_vector, vector_space=vector_space,
                          min_similarity=min_similarity)
        else:
            kwargs.update(mode=0, text=text)
        result = self.client.execute(snapshot, **kwargs)
        self.require_complete(result, snapshot.native_identity().snapshot_version, top_k)
        return result

    @staticmethod
    def require_complete(result, snapshot_version, top_k):
        if result.get("termination") != 0 or result.get("index_coverage") != 0:
            raise RagLimit("Index query did not finish with complete snapshot coverage")
        if result.get("ranking_kind") != 0:
            raise RagUnsupported("this path requires the shared Index exact ranking")
        if result.get("snapshot_version") != snapshot_version:
            raise RagIntegrityError("Index result belongs to a different snapshot")
        hits = result.get("hits")
        if not isinstance(hits, list) or len(hits) > top_k:
            raise RagIntegrityError("invalid Index hit list")
        evidence = result.get("match_evidence")
        if evidence != (1 if hits else 2):
            raise RagIntegrityError("Index lacks valid hit/empty-result evidence")
