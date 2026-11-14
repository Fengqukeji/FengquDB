"""Explicit application quotas and errors shared by embedding and HTTP APIs."""

from dataclasses import dataclass


class WikiError(ValueError):
    pass


class ConflictError(WikiError):
    """The caller's observed version is no longer current."""


class BudgetError(WikiError):
    """An application quota would be exceeded; nothing was partially applied."""


@dataclass(frozen=True)
class Limits:
    source_bytes: int = 256 * 1024
    page_bytes: int = 1024 * 1024
    batch_items: int = 64
    batch_bytes: int = 2 * 1024 * 1024
    sources_per_page: int = 16
    entities_per_source: int = 32
    claims_per_source: int = 128
    compile_context_bytes: int = 2 * 1024 * 1024
    answer_bytes: int = 256 * 1024
    attempts: int = 3
    backoff_ms: int = 1000
    workers: int = 8
    jobs_per_run: int = 1000
    answer_cache_items: int = 64
    answer_cache_bytes: int = 2 * 1024 * 1024
    answer_cache_ttl_ms: int = 60000

    def __post_init__(self):
        for name, value in vars(self).items():
            if type(value) is not int or value < 1:
                raise WikiError("quota must be a positive integer: " + name)
        if self.workers > 32 or self.batch_items > 1000 or self.sources_per_page > 256:
            raise WikiError("configured concurrency/batch/source quota exceeds hard bounds")
