"""Validated model-neutral request types, also accepting mapping/Pydantic inputs."""
from dataclasses import dataclass, fields
import re

from .types import MAX_TIME, bounded_string, integer, finite_similarity


@dataclass(frozen=True)
class MemoryNamespace:
    scope: str = "world"
    owner_id: str = ""
    npc_id: str | None = None
    timeline: str = "main"

    def __post_init__(self):
        if self.npc_id is not None:
            if self.owner_id and self.owner_id != self.npc_id:
                raise ValueError("owner_id and npc_id disagree")
            object.__setattr__(self, "owner_id", self.npc_id)
        for name in ("scope", "owner_id", "timeline"):
            value = getattr(self, name)
            if isinstance(value, str):
                value = value.strip()
                object.__setattr__(self, name, value)
            bounded_string(value, name, 160, empty=name == "owner_id")
        object.__setattr__(self, "npc_id", self.owner_id)


@dataclass(frozen=True)
class MemoryWrite(MemoryNamespace):
    text: str = ""
    known_at: int = 0
    expires_at: int | None = None
    title: str = ""
    kind: str = "memory"
    source_id: str = ""
    source_uri: str = ""
    source_revision: str = ""
    fact_key: str = ""
    expected_revision: int | None = None

    def __post_init__(self):
        super().__post_init__()
        for name, limit in (("text", self.text_limit), ("title", 200), ("source_id", 200),
                            ("source_uri", 2000), ("source_revision", 160), ("fact_key", 200)):
            value = getattr(self, name)
            if isinstance(value, str):
                value = value.strip()
                object.__setattr__(self, name, value)
            bounded_string(value, name, limit, empty=name != "text")
        integer(self.known_at, "known_at", MAX_TIME)
        if self.expires_at is not None:
            integer(self.expires_at, "expires_at", MAX_TIME)
            if self.expires_at <= self.known_at:
                raise ValueError("expires_at must be greater than known_at")
        if isinstance(self.kind, str):
            object.__setattr__(self, "kind", self.kind.strip())
        if self.kind not in ("rule", "event", "memory"):
            raise ValueError("invalid memory kind")
        if self.expected_revision is not None:
            integer(self.expected_revision, "expected_revision")
            if not self.expected_revision:
                raise ValueError("expected_revision must be positive")

    @property
    def text_limit(self):
        return 1800


@dataclass(frozen=True)
class MemoryImport(MemoryWrite):
    @property
    def text_limit(self):
        return 60000


@dataclass(frozen=True)
class MemorySearch(MemoryNamespace):
    query: str = ""
    as_of: int = 0
    top_k: int = 5
    min_similarity: float | None = None
    source_id: str = ""
    include_public: bool = True
    mode: str = "dense"
    candidate_k: int = 64

    def __post_init__(self):
        super().__post_init__()
        for name in ("query", "source_id"):
            value = getattr(self, name)
            if isinstance(value, str):
                object.__setattr__(self, name, value.strip())
        bounded_string(self.query, "query", 1800)
        integer(self.as_of, "as_of", MAX_TIME)
        integer(self.top_k, "top_k", 30)
        integer(self.candidate_k, "candidate_k", 256)
        if not self.top_k or self.candidate_k < self.top_k:
            raise ValueError("invalid top_k/candidate_k")
        finite_similarity(self.min_similarity)
        bounded_string(self.source_id, "source_id", 200, empty=True)
        if type(self.include_public) is not bool or self.mode not in ("dense", "lexical", "hybrid"):
            raise ValueError("invalid retrieval policy")


@dataclass(frozen=True)
class MemoryList(MemoryNamespace):
    source_id: str = ""
    fact_key: str = ""
    as_of: int | None = None
    after: str = ""
    limit: int = 50

    def __post_init__(self):
        super().__post_init__()
        for name in ("source_id", "fact_key", "after"):
            value = getattr(self, name)
            if isinstance(value, str):
                object.__setattr__(self, name, value.strip())
        bounded_string(self.source_id, "source_id", 200, empty=True)
        bounded_string(self.fact_key, "fact_key", 200, empty=True)
        if self.as_of is not None:
            integer(self.as_of, "as_of", MAX_TIME)
        if not re.fullmatch(r"(?:[a-f0-9]{32})?", self.after):
            raise ValueError("invalid record cursor")
        integer(self.limit, "limit", 100)
        if not self.limit:
            raise ValueError("limit must be positive")


@dataclass(frozen=True)
class MemoryDelete(MemoryNamespace):
    scope: str = ""
    source_id: str = ""

    def __post_init__(self):
        super().__post_init__()
        if isinstance(self.source_id, str):
            object.__setattr__(self, "source_id", self.source_id.strip())
        bounded_string(self.source_id, "source_id", 200, empty=True)


def coerce(cls, value):
    if isinstance(value, cls):
        return value
    if callable(getattr(value, "model_dump", None)):
        value = value.model_dump()
    if not isinstance(value, dict):
        raise TypeError(f"expected {cls.__name__}, mapping or Pydantic model")
    allowed = {f.name for f in fields(cls)}
    if set(value) - allowed:
        raise ValueError("unknown request fields: " + ",".join(sorted(set(value) - allowed)))
    return cls(**value)


def payload(item):
    return {f.name: getattr(item, f.name) for f in fields(MemoryWrite)
            if f.name not in ("npc_id", "expected_revision")}
