"""Domain types for the bounded source/chunk path; no retrieval algorithm."""
from dataclasses import dataclass, field
import math
import time

MAX_TIME = 9_000_000_000_000_000
MAX_VERSION = (1 << 63) - 1


class RagError(RuntimeError):
    pass


class RagConflict(RagError):
    pass


class RagNotFound(RagError):
    pass


class RagUnsupported(RagError):
    pass


class RagIntegrityError(RagError):
    pass


class RagStaleRead(RagError):
    pass


class RagCancelled(RagError):
    pass


class RagDeadline(RagError):
    pass


class RagLimit(RagError):
    pass


class RagRecoveryRequired(RagError):
    pass


def bounded_string(value, name, maximum, *, empty=False):
    if not isinstance(value, str) or (not empty and not value.strip()) or "\0" in value:
        raise ValueError(f"invalid {name}")
    if len(value) > maximum or len(value.encode("utf-8")) > maximum * 4:
        raise ValueError(f"{name} exceeds budget")
    return value


def integer(value, name, maximum=MAX_VERSION):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError(f"{name} must be an integer in [0,{maximum}]")
    return value


@dataclass(frozen=True)
class Chunk:
    text: str
    title: str = ""
    known_at: int = 0
    expires_at: int | None = None
    kind: str = "memory"

    def __post_init__(self):
        bounded_string(self.text, "text", 1800)
        bounded_string(self.title, "title", 200, empty=True)
        integer(self.known_at, "known_at", MAX_TIME)
        if self.expires_at is not None:
            integer(self.expires_at, "expires_at", MAX_TIME)
            if self.expires_at <= self.known_at:
                raise ValueError("expires_at must be greater than known_at")
        if self.kind not in ("memory", "event", "rule"):
            raise ValueError("unsupported chunk kind")


@dataclass(frozen=True)
class Namespace:
    scope: str
    owner_id: str
    timeline: str
    clock_id: str
    partition: bytes
    _owner: object = field(repr=False, compare=False)


@dataclass(frozen=True)
class SourceTicket:
    namespace: Namespace
    source_id: str
    chunks: tuple[Chunk, ...]
    source_revision_label: str
    expected_revision: int
    namespace_generation: int
    source_generation: int
    operation_id: str
    request_digest: str
    content_digest: str
    unchanged: bool
    replay: bool
    _owner: object = field(repr=False, compare=False)


@dataclass(frozen=True)
class QueryBudget:
    posting_budget: int = 12000
    candidate_budget: int = 1024
    scan_budget: int = 100000
    memory_budget: int = 16 * 1024 * 1024
    timeout_ms: int = 5000
    result_bytes: int = 1024 * 1024

    def __post_init__(self):
        for name, maximum in (("posting_budget", 100_000_000), ("candidate_budget", 1_000_000),
                              ("scan_budget", 10_000_000), ("memory_budget", 64 * 1024 * 1024),
                              ("timeout_ms", 600_000), ("result_bytes", 16 * 1024 * 1024)):
            value = getattr(self, name)
            integer(value, name, maximum)
            if not value:
                raise ValueError(f"{name} must be positive")


class RequestGuard:
    def __init__(self, budget, cancelled=None):
        if cancelled is not None and not callable(cancelled):
            raise TypeError("cancelled must be callable")
        self.cancelled = cancelled
        self.deadline = time.monotonic() + budget.timeout_ms / 1000

    def check(self):
        if self.cancelled is not None:
            try:
                stopped = self.cancelled()
            except BaseException as exc:
                raise RagCancelled("cancellation callback failed") from exc
            if stopped:
                raise RagCancelled("request cancelled")
        if time.monotonic() >= self.deadline:
            raise RagDeadline("request deadline exceeded")

    def stopped(self):
        self.check()
        return False

    def remaining(self):
        self.check()
        return max(0.0, self.deadline - time.monotonic())


def finite_similarity(value):
    if value is not None and (type(value) not in (int, float) or not math.isfinite(value) or not -1 <= value <= 1):
        raise ValueError("min_similarity must be finite and in [-1,1]")
