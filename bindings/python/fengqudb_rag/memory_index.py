"""Only shared native Index operations; all tokenization/scoring stays native."""
import hashlib
import json

from ._bindings import ffi, StoreError
from .native import NativeIndexProvider
from .types import RagIntegrityError, RagLimit, RagUnsupported


def partition(scope, owner, timeline="main", generation=1):
    return hashlib.sha256(json.dumps([scope, owner, timeline, generation], ensure_ascii=False,
        separators=(",", ":")).encode()).hexdigest().encode()


def eq(name, value):
    return ffi.Predicate(1, name, (value,))


def all_of(children):
    return ffi.Predicate(9, children=tuple(children))


class MemoryIndex:
    def __init__(self, store, client):
        self.provider = NativeIndexProvider(client, index_space=b"fengqu-rag-memory-v1")
        self.identity = self.provider.attach(store)
        self.client = client
        self.space = self.provider.index_space
        self.token = self.identity.store_token

    @staticmethod
    def metadata(record):
        names = ("scope", "owner_id", "timeline", "known_at", "source_id", "fact_key", "lineage_id",
                 "revision", "projection_version", "is_latest", "namespace_generation", "content_hash")
        result = {name: record[name] for name in names}
        result.update(record_id=record["id"], has_expiry=record["expires_at"] is not None,
                      expires_at=record["expires_at"] or 0, kind=record["kind"])
        return result

    def upsert(self, transaction, record, raw_vector, vector_space, *, expected=0):
        document = ffi.Document(record["id"].encode(), record["projection_version"], record["revision"],
            record["namespace_generation"], texts={"title": (record["title"], 2.0), "body": record["text"]},
            metadata=self.metadata(record), vector_f32_le=raw_vector, vector_space=vector_space,
            source_locator=("memory:" + record["id"]).encode())
        self.client.mutate(transaction, expected_store_token=self.token, index_space=self.space,
            partition=partition(record["scope"], record["owner_id"], record["timeline"], record["namespace_generation"]),
            operation=ffi.Mutation.UPSERT, expected_projection_version=expected, document=document)

    def close_fact(self, transaction, old, updated):
        document = ffi.Document(old["id"].encode(), metadata={
            "has_expiry": updated["expires_at"] is not None, "expires_at": updated["expires_at"] or 0,
            "is_latest": False, "projection_version": updated["projection_version"]})
        self.client.mutate(transaction, expected_store_token=self.token, index_space=self.space,
            partition=partition(old["scope"], old["owner_id"], old["timeline"], old["namespace_generation"]), operation=ffi.Mutation.PATCH,
            id=old["id"].encode(), expected_projection_version=old["projection_version"],
            next_projection_version=updated["projection_version"], lifecycle_generation=old["namespace_generation"], document=document)

    def erase(self, transaction, record):
        self.client.mutate(transaction, expected_store_token=self.token, index_space=self.space,
            partition=partition(record["scope"], record["owner_id"], record["timeline"], record["namespace_generation"]), operation=ffi.Mutation.ERASE,
            id=record["id"].encode(), expected_projection_version=record["projection_version"],
            next_projection_version=record["projection_version"] + 1, lifecycle_generation=record["namespace_generation"])

    def query(self, view, request, owner, state, jobs, *, dense, raw_vector, vector_space,
              limit, budget, guard, remaining, ids=None):
        children = [eq("scope", request.scope), eq("owner_id", owner), eq("timeline", request.timeline),
            eq("namespace_generation", state["generation"]), ffi.Predicate(4, "known_at", (request.as_of,)),
            ffi.Predicate(10, children=(eq("has_expiry", False), ffi.Predicate(5, "expires_at", (request.as_of,))))]
        if request.source_id:
            children.append(eq("source_id", request.source_id))
        for job in jobs:
            selector = job["selector"]
            if selector.get("lineage_id"):
                children.append(ffi.Predicate(11, children=(eq("lineage_id", selector["lineage_id"]),)))
            elif selector.get("source_id"):
                children.append(ffi.Predicate(11, children=(eq("source_id", selector["source_id"]),)))
            else:
                return None  # Namespace is completely hidden by its durable purge barrier.
        if ids is not None:
            if not ids:
                return None
            children.append(ffi.Predicate(7, "record_id", tuple(ids)))
        for name in ("posting_budget", "candidate_budget", "scan_budget"):
            if remaining[name] <= 0:
                raise RagLimit("combined retrieval work budget exhausted")
        kwargs = dict(index_space=self.space, partition=partition(request.scope, owner, request.timeline, state["generation"]),
            operation=ffi.Query.SEARCH, expected_store_token=self.token, limit=limit,
            where=all_of(children), allow_partial=False, control=ffi.QueryControl(guard.stopped),
            timeout_ms=max(1, int(guard.remaining() * 1000)), memory_budget=budget.memory_budget,
            **remaining)
        if dense:
            kwargs.update(mode=1, vector_f32_le=raw_vector, vector_space=vector_space,
                          min_similarity=request.min_similarity)
        else:
            kwargs.update(mode=0, text=request.query, statistics_policy=1)
        try:
            result = self.client.execute(view, **kwargs)
        except StoreError:
            guard.check()
            raise
        except ffi.WireError as exc:
            if not dense:
                raise RagUnsupported("shared Index wrapper must support statistics_policy=1 predicate-filtered BM25") from exc
            raise
        NativeIndexProvider.require_complete(result, view.native_identity().snapshot_version, limit)
        for key, statistic in (("posting_budget", "postings_visited"), ("candidate_budget", "candidates"),
                               ("scan_budget", "records_scanned")):
            used = result.get(statistic)
            if type(used) is not int or used < 0 or used > remaining[key]:
                raise RagIntegrityError("native work accounting exceeds requested budget")
            remaining[key] -= used
        return result
