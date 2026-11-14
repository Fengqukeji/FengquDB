"""Transactional retrieval contract and quality metadata; NativeIndex owns search."""

import re
from dataclasses import asdict, dataclass
from typing import Protocol


@dataclass(frozen=True)
class IndexDefinition:
    name: str
    format_version: int
    tokenizer_version: int

    def __post_init__(self):
        if not isinstance(self.name, str) or not re.fullmatch(r"[a-z][a-z0-9._-]{0,63}", self.name):
            raise ValueError("invalid index provider name")
        for version in (self.format_version, self.tokenizer_version):
            if type(version) is not int or not 1 <= version < 2 ** 32:
                raise ValueError("invalid index format/tokenizer version")

    def descriptor(self):
        return asdict(self)


class IndexProvider(Protocol):
    """Use the supplied transaction/snapshot; never open or commit another Store."""

    definition: IndexDefinition
    atomic_updates: bool

    def upsert(self, tx, document_id, document): ...
    def delete(self, tx, document_id): ...
    def set_page_state(self, tx, page_id, state): ...
    def get_page_state(self, view, page_id): ...
    def get_document(self, view, document_id): ...
    def statistics(self, view): ...
    def search(self, view, query, *, limit=10, posting_budget=12000, candidate_budget=1024): ...


def normalize_search_result(result):
    """Keep quality, termination and coverage separate; never infer a proved miss."""
    normalized = dict(result)
    normalized.setdefault("ranking_kind", "approximate")
    normalized.setdefault("termination", "unknown")
    normalized.setdefault("index_coverage", "unknown")
    normalized.setdefault("snapshot_version", None)
    normalized.setdefault("applied_sequence", None)
    normalized.setdefault("total_hits", {"value": None, "relation": "unknown"})
    normalized.setdefault("degraded_reasons", [])
    incomplete = (normalized["termination"] != "finished" or normalized["index_coverage"] != "complete")
    # Approximate ANN ranking can finish without interruption; it is not itself truncation.
    normalized["truncated"] = bool(normalized.get("truncated", False) or incomplete)
    total = normalized["total_hits"]
    legacy_proof = (normalized.get("empty_result_confirmed", False) and
                    total.get("relation") == "exact" and total.get("value") == 0)
    normalized.setdefault("match_evidence", "has_hits" if normalized.get("results") else
                          "proven_none" if legacy_proof else "not_established")
    normalized["empty_result_confirmed"] = bool(
        normalized.get("empty_result_confirmed", False) and not normalized.get("results") and
        not normalized["truncated"] and (normalized["match_evidence"] == "proven_none" or legacy_proof))
    return normalized


def search_quality(result):
    return {name: result[name] for name in ("ranking_kind", "termination", "index_coverage", "snapshot_version",
        "applied_sequence", "total_hits", "degraded_reasons", "empty_result_confirmed", "match_evidence")}
