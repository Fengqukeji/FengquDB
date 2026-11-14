"""Test-only exhaustive BM25 oracle; excluded from all SDK/runtime packages."""
import hashlib
import heapq
import math
import re
import unicodedata
from collections import Counter

from llmwiki.index_provider import IndexDefinition

TOKENIZER_VERSION = 1
DEFAULT_DEFINITION = IndexDefinition("python-bm25", 1, TOKENIZER_VERSION)


def tokens(text):
    text = unicodedata.normalize("NFKC", text).casefold()
    for match in re.finditer(r"[\u3400-\u9fff]+|(?:(?![\u3400-\u9fff])[^\W_])+", text, re.UNICODE):
        value = match.group()
        if "\u3400" <= value[0] <= "\u9fff":
            yield from value
            yield from (value[i:i + 2] for i in range(len(value) - 1))
        else:
            yield value[:128]


def term_key(term):
    return hashlib.sha256(term.encode()).hexdigest()[:32]


def remove_document(tx, doc_id):
    old = tx.get("doc/" + doc_id)
    if old is None:
        return
    stats = tx.get("search/stats", {"count": 0, "length": 0})
    for term in tx.get("terms/" + doc_id, []):
        tx.delete("post/" + term + "/" + doc_id)
        count = tx.get("df/" + term, 0) - 1
        if count:
            tx.put("df/" + term, count)
        else:
            tx.delete("df/" + term)
    tx.delete("doc/" + doc_id)
    tx.delete("terms/" + doc_id)
    tx.delete("body/" + doc_id)
    tx.put("search/stats", {"count": stats["count"] - 1, "length": stats["length"] - old["length"]})


def index_document(tx, doc_id, document):
    remove_document(tx, doc_id)
    frequencies = Counter(term_key(t) for t in tokens(document["title"] + "\n" + document["text"]))
    length = max(sum(frequencies.values()), 1)
    document = dict(document, length=length)
    body = document.pop("text")
    for term, frequency in frequencies.items():
        tx.put("post/" + term + "/" + doc_id, [frequency, length])
        tx.put("df/" + term, tx.get("df/" + term, 0) + 1)
    tx.put("doc/" + doc_id, document)
    tx.put("terms/" + doc_id, list(frequencies))
    tx.put("body/" + doc_id, body)
    stats = tx.get("search/stats", {"count": 0, "length": 0})
    tx.put("search/stats", {"count": stats["count"] + 1, "length": stats["length"] + length})


def search(view, query, *, limit=10, posting_budget=12000, candidate_budget=1024):
    stats = view.get("search/stats", {"count": 0, "length": 0})
    terms = list(dict.fromkeys(term_key(t) for t in tokens(query)))
    reasons = ["query_term_limit"] if len(terms) > 32 else []
    terms = terms[:32]
    dfs = view.get_many(["df/" + t for t in terms])
    ranked = sorted((df, term) for term, df in zip(terms, dfs) if df)
    scores, visited = {}, 0
    avg = stats["length"] / max(stats["count"], 1)
    for term_index, (df, term) in enumerate(ranked):
        idf = math.log(1 + (stats["count"] - df + 0.5) / (df + 0.5))
        for key, (frequency, length) in view.scan("post/" + term + "/"):
            if visited >= posting_budget:
                if "posting_limit" not in reasons:
                    reasons.append("posting_limit")
                break
            visited += 1
            doc_id = key.rsplit("/", 1)[1]
            if doc_id not in scores and len(scores) >= candidate_budget:
                if "candidate_limit" not in reasons:
                    reasons.append("candidate_limit")
                continue
            score = idf * frequency * 2.2 / (frequency + 1.2 * (0.25 + 0.75 * length / max(avg, 1)))
            scores[doc_id] = scores.get(doc_id, 0) + score
        if visited >= posting_budget:
            if term_index + 1 < len(ranked) and "posting_limit" not in reasons:
                reasons.append("posting_limit")
            break
    # Fetch candidates in one native batch; do not scan the whole corpus.
    candidates = heapq.nlargest(candidate_budget, scores, key=lambda key: (scores[key], key))
    documents = view.get_many(["doc/" + doc_id for doc_id in candidates])
    page_ids = [d["page_id"] for d in documents if d and d["kind"] == "page"]
    pages = dict(zip(page_ids, view.get_many(["state/" + key for key in page_ids])))
    results = []
    for doc_id, document in zip(candidates, documents):
        if not document:
            continue
        if document["kind"] == "page":
            page = pages.get(document["page_id"])
            if not page or page["stale"]:
                continue
        results.append({k: v for k, v in document.items() if k not in ("terms", "length")} |
                       {"id": doc_id, "score": round(scores[doc_id], 6)})
    # Rank with unrounded scores; presentation rounding must not change Top K.
    results.sort(key=lambda d: (-scores[d["id"]], d["id"]))
    total_hits = len(results)
    results = results[:limit]
    for result, body in zip(results, view.get_many(["body/" + d["id"] for d in results])):
        result["text"] = (body or "")[:4000]
        result["text_truncated"] = len(body or "") > 4000
    truncated = bool(reasons)
    return {"results": results, "truncated": truncated, "postings_visited": visited,
            "candidates": len(candidates), "ranking_kind": "approximate" if truncated else "exact",
            "termination": reasons[-1] if reasons else "finished", "degraded_reasons": reasons,
            "index_coverage": "complete", "snapshot_version": None, "applied_sequence": None,
            "total_hits": {"value": total_hits, "relation": "lower_bound" if truncated else "exact"},
            "empty_result_confirmed": not truncated and total_hits == 0,
            "match_evidence": "has_hits" if total_hits else "not_established" if truncated else "proven_none"}


class ReferenceIndexProvider:
    """Compatibility/reference adapter, not a second implementation of """

    definition = DEFAULT_DEFINITION
    atomic_updates = True

    def upsert(self, tx, document_id, document):
        index_document(tx, document_id, document)

    def delete(self, tx, document_id):
        remove_document(tx, document_id)

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

    def search(self, view, query, *, limit=10, posting_budget=12000, candidate_budget=1024, **options):
        if options:
            raise ValueError("advanced queries require the shared NativeIndex provider")
        return search(view, query, limit=limit, posting_budget=posting_budget, candidate_budget=candidate_budget)
