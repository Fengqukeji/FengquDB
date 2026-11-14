"""Budgeted evidence packing; the repository revalidates references after packing."""
import json

from .types import RagLimit


def pack_context(records, *, max_chars=6000, max_tokens=None, token_counter=None,
                 exclude_sources=(), predicate=None):
    if type(max_chars) is not int or not 0 <= max_chars <= 1_000_000:
        raise ValueError("invalid context character budget")
    if max_tokens is not None and (type(max_tokens) is not int or max_tokens < 0 or not callable(token_counter)):
        raise ValueError("token budget requires an explicit token counter")
    if predicate is not None and not callable(predicate):
        raise TypeError("context predicate must be callable")
    excluded = set(exclude_sources)
    selected, citations, parts = [], [], []
    used = 0
    skipped = 0
    used_tokens = 0 if max_tokens is not None else None
    for record in records:
        if record["source_id"] in excluded or (predicate is not None and not predicate(dict(record))):
            skipped += 1
            continue
        # JSON quoting prevents record text from being confused with our citation boundary.
        part = json.dumps({"citation": record["id"] + "@" + str(record["revision"]),
                           "source_id": record["source_id"], "text": record["text"]}, ensure_ascii=False)
        addition = ("\n" if parts else "") + part
        if used + len(addition) > max_chars:
            skipped += 1
            continue
        proposed = "\n".join(parts + [part])
        if max_tokens is not None:
            tokens = token_counter(proposed)
            if type(tokens) is not int or tokens < 0:
                raise RagLimit("invalid token counter result")
            if tokens > max_tokens:
                skipped += 1
                continue
        parts.append(part)
        if max_tokens is not None:
            used_tokens = tokens
        used += len(addition)
        selected.append(dict(record))
        citations.append({key: record[key] for key in ("id", "revision", "projection_version", "source_id",
                           "source_uri", "source_revision", "content_hash", "known_at")})
    return {"text": "\n".join(parts), "items": selected, "citations": citations,
            "characters": used, "skipped": skipped, "truncated": skipped > 0,
            "tokens": used_tokens}
