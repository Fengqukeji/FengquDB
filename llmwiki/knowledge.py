"""Grounded entity/claim graph; all retrieval remains in the shared Index."""

import hashlib
import math
import unicodedata

from .limits import BudgetError, ConflictError, WikiError
from .storage import encode


def key(value):
    return hashlib.sha256(encode(value)).hexdigest()


def normalized(value):
    return unicodedata.normalize("NFKC", value).strip().casefold()


def string(value, name, maximum=512):
    if not isinstance(value, str) or not value.strip() or "\0" in value or len(value.encode()) > maximum:
        raise WikiError("invalid " + name)
    return value.strip()


def validate_knowledge(payload, sources, limits):
    """Bind quotes to exact codepoint ranges of immutable, versioned sources."""
    if payload is None:
        payload = {}
    if not isinstance(payload, dict) or set(payload) - {"entities", "claims"}:
        raise WikiError("knowledge requires entities and claims")
    entities, claims = payload.get("entities", []), payload.get("claims", [])
    if not isinstance(entities, list) or not isinstance(claims, list):
        raise WikiError("entities and claims must be arrays")
    if len(entities) > limits.entities_per_source or len(claims) > limits.claims_per_source:
        raise BudgetError("knowledge extraction quota exceeded")
    names, output = {}, []
    for entity in entities:
        if not isinstance(entity, dict) or set(entity) - {"key", "type", "name", "aliases"}:
            raise WikiError("invalid entity")
        ref = string(entity.get("key"), "entity key", 128)
        name, kind = string(entity.get("name"), "entity name"), string(entity.get("type", "concept"), "entity type", 128)
        aliases = entity.get("aliases", [])
        if not isinstance(aliases, list) or len(aliases) > 16 or ref in names:
            raise WikiError("duplicate entity key or invalid aliases")
        aliases = sorted(set(string(a, "alias") for a in aliases))
        eid = key([normalized(kind), normalized(name)])
        item = {"id": eid, "name": name, "type": kind, "aliases": aliases}
        names[ref] = item
        output.append(item)
    by_label = {s["label"]: s for s in sources}
    facts = []
    for claim in claims:
        if not isinstance(claim, dict) or set(claim) - {"entity", "predicate", "value", "source", "quote", "start"}:
            raise WikiError("invalid claim")
        entity_ref, source_ref = claim.get("entity"), claim.get("source", "S1")
        if not isinstance(entity_ref, str) or not isinstance(source_ref, str):
            raise WikiError("claim entity/source references must be strings")
        entity = names.get(entity_ref)
        source = by_label.get(source_ref)
        if entity is None or source is None:
            raise WikiError("claim requires a declared entity and source")
        predicate = string(claim.get("predicate"), "predicate", 256)
        value = claim.get("value")
        if (type(value) not in (str, int, bool, float) or
                isinstance(value, str) and (not value.strip() or len(value.encode()) > 2048) or
                isinstance(value, float) and not math.isfinite(value) or len(encode(value)) > 2048):
            raise WikiError("claim value requires a bounded scalar")
        quote = claim.get("quote")
        string(quote, "claim quote", 8192)  # Validate without changing exact whitespace/offsets.
        start = claim.get("start")
        if start is None:
            start = source["content"].find(quote)
            if start >= 0 and source["content"].find(quote, start + 1) >= 0:
                raise WikiError("ambiguous quote requires an explicit start offset")
        if type(start) is not int or start < 0 or source["content"][start:start + len(quote)] != quote:
            raise WikiError("claim quote does not match its source")
        fact = {"entity_id": entity["id"], "predicate": predicate, "value": value,
                "source_id": source["source_id"], "revision": source["revision"],
                "source_version": source["version"], "quote": quote, "start": start, "end": start + len(quote)}
        fact["group_id"] = key([entity["id"], normalized(predicate)])
        fact["id"] = key(fact)
        facts.append(fact)
    return {"entities": sorted({e["id"]: e for e in output}.values(), key=lambda e: e["id"]),
            "claims": sorted({c["id"]: c for c in facts}.values(), key=lambda c: c["id"])}


class KnowledgeMixin:
    def _refresh_groups(self, tx, groups):
        for gid in sorted(groups):
            claims = [tx.get("claim/" + cid) for _, cid in tx.scan("group/" + gid + "/")]
            claims = [c for c in claims if c is not None]
            old = tx.get("conflict/" + gid)
            values = {encode(c["value"]) for c in claims}
            ids = sorted(c["id"] for c in claims)
            if not old and len(values) <= 1:
                continue
            if old and old["claim_ids"] == ids:
                continue
            if old:
                tx.put("conflict-history/" + gid + "/%016x" % old["generation"], old)
            record = {"id": gid, "entity_id": (claims[0] if claims else old)["entity_id"],
                      "predicate": (claims[0] if claims else old)["predicate"], "claim_ids": ids,
                      "generation": (old or {}).get("generation", 0) + 1,
                      "state": "open" if len(values) > 1 else "inactive", "resolution": None}
            tx.put("conflict/" + gid, record)
            pointer = "entity-conflict/" + record["entity_id"] + "/" + gid
            if record["state"] == "inactive":
                tx.delete(pointer)
            else:
                tx.put(pointer, gid)

    def _clear_claims(self, tx, sid):
        groups = set()
        for pointer, cid in list(tx.scan("source-claim/" + sid + "/")):
            claim = tx.get("claim/" + cid)
            if claim:
                tx.put("claim-history/" + cid, claim)
                groups.add(claim["group_id"])
                tx.delete("group/" + claim["group_id"] + "/" + cid)
                tx.delete("claim/" + cid)
            tx.delete(pointer)
        return groups

    def _sync_entities(self, tx, entities):
        for eid in sorted(entities):
            entity = tx.get("entity/" + eid)
            ids = sorted(sid for _, sid in tx.scan("entity-source/" + eid + "/"))
            if len(ids) > self.limits.sources_per_page:
                raise BudgetError("entity exceeds sources_per_page; split the entity scope")
            entity.update(source_ids=ids, state="active" if ids else "retired")
            entity["aliases"] = sorted({alias for sid in ids
                if not tx.get("source/" + sid)["knowledge_pending"]
                for e in tx.get("knowledge/" + sid, {"entities": []})["entities"] if e["id"] == eid
                for alias in e["aliases"]})
            tx.put("entity/" + eid, entity)
            self._define(tx, "entity-" + eid, entity["name"], ids, [], "entity")

    def _invalidate_knowledge(self, tx, sid, *, deleted=False):
        groups = self._clear_claims(tx, sid)
        entities = set()
        for pointer, eid in list(tx.scan("source-entity/" + sid + "/")):
            entities.add(eid)
            if deleted:
                tx.delete(pointer)
                tx.delete("entity-source/" + eid + "/" + sid)
        self._refresh_groups(tx, groups)
        self._sync_entities(tx, entities)

    def _replace_knowledge(self, tx, sid, knowledge):
        groups = self._clear_claims(tx, sid)
        affected = set()
        for pointer, eid in list(tx.scan("source-entity/" + sid + "/")):
            affected.add(eid)
            tx.delete(pointer)
            tx.delete("entity-source/" + eid + "/" + sid)
        for entity in knowledge["entities"]:
            eid = entity["id"]
            old = tx.get("entity/" + eid, {})
            # Source-specific aliases live in knowledge/<sid>, merged by _sync_entities.
            tx.put("entity/" + eid, dict(old, **entity))
            tx.put("source-entity/" + sid + "/" + eid, eid)
            tx.put("entity-source/" + eid + "/" + sid, sid)
            affected.add(eid)
        for claim in knowledge["claims"]:
            tx.put("claim/" + claim["id"], claim)
            tx.put("source-claim/" + sid + "/" + claim["id"], claim["id"])
            tx.put("group/" + claim["group_id"] + "/" + claim["id"], claim["id"])
            groups.add(claim["group_id"])
        source = tx.get("source/" + sid)
        source["knowledge_pending"] = False
        tx.put("source/" + sid, source)
        tx.put("knowledge/" + sid, knowledge)
        self._refresh_groups(tx, groups)
        self._sync_entities(tx, affected)

    def entity(self, eid):
        self._hash_id(eid)
        with self.repo.snapshot() as view:
            entity = view.get("entity/" + eid)
            if entity:
                entity["claims"] = [c for sid in entity["source_ids"] for _, cid in
                    view.scan("source-claim/" + sid + "/") if (c := view.get("claim/" + cid)) and c["entity_id"] == eid]
                entity["conflicts"] = [view.get("conflict/" + gid) for _, gid in view.scan("entity-conflict/" + eid + "/")]
            return entity

    def conflict(self, gid, *, generation=None):
        self._hash_id(gid)
        if generation is not None and (type(generation) is not int or generation < 1):
            raise WikiError("invalid conflict generation")
        with self.repo.snapshot() as view:
            current = view.get("conflict/" + gid)
            result = (current if generation is None or current and current["generation"] == generation else
                      view.get("conflict-history/" + gid + "/%016x" % generation))
            if result:
                result["claims"] = [view.get("claim/" + cid) or view.get("claim-history/" + cid) for cid in result["claim_ids"]]
            return result

    def resolve_conflict(self, gid, claim_ids, *, expected_generation, note, actor):
        self._hash_id(gid)
        note, actor = string(note, "resolution note", 4096), string(actor, "actor", 128)
        if not isinstance(claim_ids, list) or not claim_ids or len(claim_ids) > self.limits.claims_per_source:
            raise WikiError("resolution requires selected claim ids")
        for cid in claim_ids:
            self._hash_id(cid)
        def operation(tx):
            record = tx.get("conflict/" + gid)
            if not record or type(expected_generation) is not int or record["generation"] != expected_generation:
                raise ConflictError("conflict generation changed")
            if record["state"] == "inactive" or not set(claim_ids) <= set(record["claim_ids"]):
                raise WikiError("selected evidence is not current")
            if len({encode(tx.get("claim/" + cid)["value"]) for cid in claim_ids}) != 1:
                raise WikiError("selected claims must agree on a scalar value")
            tx.put("conflict-history/" + gid + "/%016x" % record["generation"], record)
            record.update(state="resolved", generation=record["generation"] + 1,
                          resolution={"claim_ids": sorted(set(claim_ids)), "note": note, "actor": actor})
            tx.put("conflict/" + gid, record)
            self._schedule(tx, "entity-" + record["entity_id"])
            self._audit(tx, "conflict.resolve", gid)
            return record
        return self.repo.write(operation)

    def _entity_context(self, view, spec):
        if spec["kind"] != "entity":
            return None
        eid = spec["id"][7:]
        claims = [c for sid in spec["source_ids"] for _, cid in view.scan("source-claim/" + sid + "/")
                  if (c := view.get("claim/" + cid)) and c["entity_id"] == eid]
        conflicts = [view.get("conflict/" + gid) for _, gid in view.scan("entity-conflict/" + eid + "/")]
        return {"claims": claims, "conflicts": [c for c in conflicts if c and c["state"] != "inactive"]}

    def _answer_conflicts(self, view, evidence, remaining):
        """Attach current conflict decisions even when retrieval selected only raw chunks."""
        source_ids = sorted({c["source_id"] for e in evidence for c in e["citations"]})
        entities, groups, extra, truncated = set(), set(), [], len(source_ids) > 32
        for sid in source_ids[:32]:
            for _, eid in view.scan("source-entity/" + sid + "/", limit=32):
                if eid in entities:
                    continue
                if len(entities) >= 64:
                    truncated = True
                    break
                entities.add(eid)
                for _, gid in view.scan("entity-conflict/" + eid + "/", limit=17):
                    if gid in groups:
                        continue
                    if len(groups) >= 16:
                        truncated = True
                        break
                    groups.add(gid)
                    conflict = view.get("conflict/" + gid)
                    if not conflict or conflict["state"] == "inactive":
                        continue
                    claims = [view.get("claim/" + cid) for cid in conflict["claim_ids"][:32]]
                    claims = [c for c in claims if c is not None]
                    payload = {k: conflict[k] for k in ("id", "entity_id", "predicate", "state", "generation", "resolution")}
                    payload.update(claims=claims, claim_count=len(conflict["claim_ids"]),
                                   evidence_truncated=len(conflict["claim_ids"]) > len(claims))
                    body = encode(payload).decode("utf-8")
                    if len(body) > remaining:
                        truncated = True
                        continue
                    citations = []
                    for claim in claims:
                        raw = view.get("raw/" + claim["source_id"] + "/" + claim["revision"])
                        citations.append({k: claim[k] for k in ("source_id", "revision", "source_version", "start", "end")} |
                                         {"uri": raw["uri"], "title": raw["title"]})
                    if citations:
                        remaining -= len(body)
                        extra.append({"label": "E" + str(len(evidence) + len(extra) + 1), "text": body,
                                      "title": "Current conflict evidence", "citations": citations,
                                      "document_id": "conflict-" + gid})
                        truncated |= payload["evidence_truncated"]
        return extra, remaining, truncated
