"""Transactional retrieval port; the existing Python index remains the default."""

import re
from dataclasses import asdict, dataclass
from typing import Protocol

from . import retrieval


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


DEFAULT_DEFINITION = IndexDefinition("python-bm25", 1, retrieval.TOKENIZER_VERSION)


class PythonIndexProvider:
    """Compatibility/reference adapter, not a second implementation of retrieval."""

    definition = DEFAULT_DEFINITION
    atomic_updates = True

    def upsert(self, tx, document_id, document):
        retrieval.index_document(tx, document_id, document)

    def delete(self, tx, document_id):
        retrieval.remove_document(tx, document_id)

    def set_page_state(self, tx, page_id, state):
        if state is None:
            tx.delete("state/" + page_id)
        else:
            tx.put("state/" + page_id, state)

    def get_page_state(self, view, page_id):
        return view.get("state/" + page_id)

    def get_document(self, view, document_id):
        return view.get("doc/" + document_id)

    def statistics(self, view):
        return view.get("search/stats", {"count": 0, "length": 0})

    def search(self, view, query, *, limit=10, posting_budget=12000, candidate_budget=1024):
        return retrieval.search(view, query, limit=limit, posting_budget=posting_budget, candidate_budget=candidate_budget)
