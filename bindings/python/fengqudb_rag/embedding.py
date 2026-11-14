"""Pluggable lazy embedding, stable contracts, bounded cache and safe FP32 output."""
from collections import OrderedDict
from dataclasses import asdict, dataclass
import hashlib
import json
import math
import threading

from ._bindings import vector
from .types import RagIntegrityError, RagUnsupported, bounded_string


@dataclass(frozen=True)
class EmbeddingContract:
    identity: str
    dimension: int
    pooling: str = "provider"
    document_transform: str = "document-v1"
    query_transform: str = "query-v1"
    normalization: str = "l2"
    metric: str = "cosine"

    def __post_init__(self):
        for name in ("identity", "pooling", "document_transform", "query_transform"):
            bounded_string(getattr(self, name), name, 2000)
        if type(self.dimension) is not int or not 1 <= self.dimension <= 4096:
            raise ValueError("embedding dimension must be 1..4096")
        if self.normalization != "l2" or self.metric != "cosine":
            raise RagUnsupported("memory v1 requires L2/cosine")

    @property
    def fingerprint(self):
        return hashlib.sha256(json.dumps(asdict(self), sort_keys=True, separators=(",", ":")).encode()).hexdigest()


class CallbackEmbedding:
    """Wrap an existing embed(texts, query=False), e.g. WordGame KaLM.

    Ownership stays with the host. The callback performs query instructions,
    model-byte verification, lazy loading and device fallback exactly once.
    """
    def __init__(self, callback, contract, *, status=None, warmup=None):
        if not callable(callback) or not isinstance(contract, EmbeddingContract):
            raise TypeError("callback and EmbeddingContract required")
        self.callback, self._contract = callback, contract
        self.status_callback, self.warmup_callback = status, warmup
        self._lock = threading.RLock()

    def contract(self):
        return self._contract

    def embed_documents(self, texts):
        with self._lock:
            return self.callback(texts, query=False)

    def embed_queries(self, texts):
        with self._lock:
            return self.callback(texts, query=True)

    def status(self):
        return self.status_callback() if self.status_callback else {"state": "external", "model": self._contract.identity}

    def warmup(self):
        if not self.warmup_callback:
            raise RagUnsupported("host Provider does not expose explicit warmup")
        return self.warmup_callback()


class EmbeddingRuntime:
    def __init__(self, provider, *, cache_bytes=4 * 1024 * 1024, batch_size=16):
        self.provider = provider
        self.contract = provider.contract()
        if not isinstance(self.contract, EmbeddingContract):
            raise TypeError("provider.contract() must return EmbeddingContract")
        self.cache_bytes, self.batch_size = cache_bytes, batch_size
        self._cache, self._bytes, self._lock = OrderedDict(), 0, threading.RLock()

    def check_contract(self):
        if self.provider.contract() != self.contract:
            raise RagIntegrityError("Embedding Provider contract changed; open a new collection")

    def _normalize(self, values):
        values = tuple(values)
        if len(values) != self.contract.dimension or any(type(v) not in (float, int) or not math.isfinite(v) for v in values):
            raise RagIntegrityError("invalid embedding output dimensions or scalar")
        scale = max(abs(v) for v in values)
        if not scale:
            raise RagIntegrityError("zero embedding vector")
        scaled = [v / scale for v in values]
        norm = math.hypot(*scaled)
        return vector([v / norm for v in scaled])

    def embed(self, texts, *, query=False, guard=None):
        self.check_contract()
        unique = list(dict.fromkeys(texts))
        results, missing = {}, []
        keys = {text: hashlib.sha256((self.contract.fingerprint + (":q:" if query else ":d:") + text).encode()).digest() for text in unique}
        with self._lock:
            for text in unique:
                value = self._cache.get(keys[text])
                if value is None:
                    missing.append(text)
                else:
                    self._cache.move_to_end(keys[text])
                    results[text] = value
        method = self.provider.embed_queries if query else self.provider.embed_documents
        for offset in range(0, len(missing), self.batch_size):
            if guard:
                guard.check()
            batch = missing[offset:offset + self.batch_size]
            values = list(method(batch))
            if len(values) != len(batch):
                raise RagIntegrityError("incomplete embedding batch")
            encoded = [self._normalize(value) for value in values]
            self.check_contract()
            if guard:
                guard.check()
            with self._lock:
                for text, value in zip(batch, encoded):
                    results[text] = value
                    key = keys[text]
                    previous = self._cache.pop(key, None)
                    if previous is not None:
                        self._bytes -= len(previous) + 96
                    self._cache[key] = value
                    self._bytes += len(value) + 96
                while self._cache and self._bytes > self.cache_bytes:
                    _, removed = self._cache.popitem(last=False)
                    self._bytes -= len(removed) + 96
        return [results[text] for text in texts]
