"""Transactional memory/fact/audit directories and resumable lifecycle cleanup."""
from dataclasses import asdict
import hashlib
import json
import re
import time
import uuid

from .models import payload
from .types import MAX_VERSION, RagConflict, RagIntegrityError, RagNotFound, RagLimit

ROOT = b"fqrag-memory-v1/"


def encode(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def digest(value):
    return hashlib.sha256(encode(value)).hexdigest().encode()


def prefix_end(prefix):
    return prefix[:-1] + bytes((prefix[-1] + 1,))


def load(view, key, default=None):
    data = view.get(key)
    if data is None:
        return default
    try:
        return json.loads(data)
    except (ValueError, UnicodeError) as exc:
        raise RagIntegrityError("corrupt RAG domain record") from exc


def save(tx, key, value):
    tx.put(key, encode(value))


def take(view, prefix, limit, *, after=b"", reverse=False):
    result = []
    with view.scan(prefix + after, prefix_end(prefix), reverse=reverse) as cursor:
        for key, value, *_ in cursor:
            result.append((key, value))
            if len(result) >= limit:
                break
    return result


def nsid(scope, owner, timeline="main"):
    return digest([scope, owner, timeline])


def rkey(record_id):
    if not isinstance(record_id, str) or not re.fullmatch(r"[a-f0-9]{32}", record_id):
        raise ValueError("invalid memory record ID")
    return ROOT + b"R/" + record_id.encode()


def nkey(scope, owner, timeline="main"):
    return ROOT + b"N/" + nsid(scope, owner, timeline)


def public(record):
    return {k: v for k, v in record.items() if not k.startswith("_")}


def selected(record, selector):
    return (all(record.get(k) == selector[k] for k in ("scope", "owner_id", "timeline")) and
        (not selector.get("source_id") or record["source_id"] == selector["source_id"]) and
        (not selector.get("lineage_id") or record["lineage_id"] == selector["lineage_id"]))


class Repository:
    def __init__(self, store, index, vector_space):
        self.store, self.index, self.vector_space = store, index, vector_space

    @staticmethod
    def namespace(view, scope, owner, timeline="main"):
        state = load(view, nkey(scope, owner, timeline))
        if state is None:
            return dict(scope=scope, owner_id=owner, timeline=timeline, generation=1, epoch=0, active=True, jobs=[])
        if any(state.get(k) != v for k, v in (("scope", scope), ("owner_id", owner), ("timeline", timeline))):
            raise RagIntegrityError("namespace identity differs")
        return state

    @staticmethod
    def jobs(view, state):
        result = []
        for job_id in state["jobs"]:
            job = load(view, ROOT + b"J/" + job_id.encode())
            if job is None:
                raise RagIntegrityError("missing durable cleanup task")
            result.append(job)
        return result

    def blocked(self, view, record):
        state = self.namespace(view, record["scope"], record["owner_id"], record["timeline"])
        return (not state["active"] or state["generation"] != record["namespace_generation"] or
                any(selected(record, j["selector"]) for j in self.jobs(view, state)))

    def record(self, view, record_id, *, visible=True):
        record = load(view, rkey(record_id))
        if record is None or (visible and self.blocked(view, record)):
            raise RagNotFound("memory does not exist or is being removed")
        if record.get("id") != record_id or record.get("_schema") != 1:
            raise RagIntegrityError("invalid canonical memory identity")
        if not isinstance(record.get("text"), str) or record.get("content_hash") != hashlib.sha256(record["text"].encode()).hexdigest():
            raise RagIntegrityError("canonical memory content hash differs")
        return record

    @staticmethod
    def directory(record, name, suffix=b""):
        space = nsid(record["scope"], record["owner_id"], record["timeline"])
        if name == b"D":
            return ROOT + b"D/" + space + b"/" + suffix
        if name == b"S":
            return ROOT + b"S/" + space + b"/" + digest(record["source_id"]) + b"/" + suffix
        if name == b"B":
            return ROOT + b"B/" + space + b"/" + digest(record["fact_key"]) + b"/" + suffix
        if name == b"T":
            return ROOT + b"T/" + space + b"/" + digest(record["fact_key"]) + b"/" + suffix
        if name == b"L":
            return ROOT + b"L/" + record["lineage_id"].encode() + b"/" + suffix
        raise ValueError("unknown directory")

    @staticmethod
    def fact_key(item):
        return ROOT + b"F/" + nsid(item["scope"], item["owner_id"], item["timeline"]) + b"/" + digest(item["fact_key"])

    @staticmethod
    def dedupe_key(item, fingerprint):
        return ROOT + b"E/" + nsid(item["scope"], item["owner_id"], item["timeline"]) + b"/" + fingerprint.encode()

    def duplicate(self, view, item, fingerprint):
        record_id = load(view, self.dedupe_key(item, fingerprint))
        if record_id is None:
            return None
        record = self.record(view, record_id)
        if record["_dedupe"] != fingerprint:
            raise RagIntegrityError("dedupe identity differs")
        return record

    def prepare(self, items, record_id=None):
        tx = self.store.transaction()
        try:
            initial, epochs, existing, raw = None, {}, {}, {}
            metadata_changed = False
            for item in items:
                key = nkey(item.scope, item.owner_id, item.timeline)
                state = self.namespace(tx, item.scope, item.owner_id, item.timeline)
                if not state["active"] or state["jobs"]:
                    raise RagConflict("namespace is retired or has unfinished cleanup")
                if tx.get(key) is None:
                    save(tx, key, state)
                    metadata_changed = True
                epochs[key] = (state["epoch"], state["generation"])
                values = payload(item)
                fingerprint = digest(values).decode()
                existing[fingerprint] = self.duplicate(tx, values, fingerprint) if not record_id else None
            if record_id:
                item = items[0]
                initial = self.record(tx, record_id)
                if item.expected_revision is not None and initial["revision"] != item.expected_revision:
                    raise RagConflict("stale memory revision")
                for key in ("scope", "owner_id", "timeline", "source_id", "fact_key"):
                    if getattr(item, key) != initial[key]:
                        raise RagConflict("edit cannot change memory identity or source")
                if item.fact_key and not initial["is_latest"]:
                    raise RagConflict("closed fact version cannot be edited")
                if item.fact_key and (item.known_at, item.expires_at) != (initial["known_at"], initial["expires_at"]):
                    raise RagConflict("fact periods change through new versions")
                if item.text == initial["text"]:
                    raw[item.text] = tx.get(ROOT + b"Q/" + record_id.encode())
                    if raw[item.text] is None:
                        raise RagIntegrityError("canonical vector missing")
            if metadata_changed:
                tx.commit(force_reliable=True)
            return dict(initial=initial, epochs=epochs, existing=existing, raw=raw)
        finally:
            tx.close()

    def _allocate(self, tx, lineage_id):
        key = ROOT + b"C/" + lineage_id.encode()
        next_revision = load(tx, key, 1)
        if next_revision >= MAX_VERSION:
            raise RagLimit("lineage revision exhausted")
        save(tx, key, next_revision + 1)
        return next_revision

    def _audit(self, tx, record):
        revision = f"{record['revision']:016x}".encode()
        key = ROOT + b"A/" + record["lineage_id"].encode() + b"/" + revision
        save(tx, key, {"record_id": record["id"]})
        tx.put(ROOT + b"V/" + record["id"].encode() + b"/" + revision, key)

    def _latest_fact(self, tx, values):
        entries = take(tx, self.directory(values, b"T"), 1, reverse=True)
        if not entries:
            return None
        return self.record(tx, entries[0][1].decode(), visible=False)

    def commit(self, items, prepared, vectors, record_id=None):
        tx = self.store.transaction()
        try:
            for key, captured in prepared["epochs"].items():
                current = load(tx, key)
                if current is None or not current["active"] or current["jobs"] or (current["epoch"], current["generation"]) != captured:
                    raise RagConflict("namespace changed during embedding")
            if record_id:
                current = self.record(tx, record_id)
                old = prepared["initial"]
                if (current["revision"], current["projection_version"], current["is_latest"]) != (
                    old["revision"], old["projection_version"], old["is_latest"]):
                    raise RagConflict("memory changed during embedding")
            results, changed = [], False
            for item in items:
                values = payload(item)
                fingerprint = digest(values).decode()
                duplicate = self.duplicate(tx, values, fingerprint)
                if not record_id and duplicate is not None:
                    results.append(duplicate["id"])
                    continue
                if record_id and fingerprint == prepared["initial"]["_dedupe"]:
                    results.append(record_id)
                    continue
                if duplicate is not None and duplicate["id"] != record_id:
                    raise RagConflict("edit would duplicate another record")
                raw_vector = vectors.get(item.text)
                if raw_vector is None:
                    raise RagIntegrityError("embedding batch does not cover staged text")
                rid = record_id or uuid.uuid4().hex
                now = time.time_ns() // 1_000_000
                state = self.namespace(tx, item.scope, item.owner_id, item.timeline)
                record = dict(values, id=rid, npc_id=item.owner_id, content_hash=hashlib.sha256(item.text.encode()).hexdigest(),
                    lineage_id=rid, supersedes="", revision=1, edit_count=0, is_latest=True, created_at=now,
                    updated_at=now, projection_version=1, namespace_generation=state["generation"], _dedupe=fingerprint, _schema=1)
                expected_projection = 0
                if record_id:
                    previous = self.record(tx, rid)
                    if previous["projection_version"] >= MAX_VERSION:
                        raise RagLimit("memory projection version exhausted")
                    previous_audit = ROOT + b"A/" + previous["lineage_id"].encode() + b"/" + f"{previous['revision']:016x}".encode()
                    save(tx, previous_audit, {"record_id": rid, "snapshot": public(previous)})
                    record.update(lineage_id=previous["lineage_id"], supersedes=previous["supersedes"],
                        revision=self._allocate(tx, previous["lineage_id"]), edit_count=previous["edit_count"] + 1,
                        is_latest=previous["is_latest"], created_at=previous["created_at"],
                        projection_version=previous["projection_version"] + 1)
                    expected_projection = previous["projection_version"]
                    tx.delete(self.dedupe_key(previous, previous["_dedupe"]))
                elif item.fact_key:
                    previous = self._latest_fact(tx, values)
                    if previous is not None:
                        if item.known_at <= previous["known_at"]:
                            raise RagConflict("fact versions must have strictly increasing known_at")
                        updated = dict(previous, is_latest=False,
                            expires_at=min(previous["expires_at"] if previous["expires_at"] is not None else MAX_VERSION, item.known_at))
                        if updated != previous:
                            if previous["projection_version"] >= MAX_VERSION:
                                raise RagLimit("fact projection version exhausted")
                            updated["projection_version"] += 1
                            save(tx, rkey(previous["id"]), updated)
                            self.index.close_fact(tx, previous, updated)
                        record.update(lineage_id=previous["lineage_id"], supersedes=previous["id"])
                    record["revision"] = self._allocate(tx, record["lineage_id"])
                    save(tx, self.fact_key(values), {"lineage_id": record["lineage_id"], "head_id": rid})
                else:
                    record["revision"] = self._allocate(tx, record["lineage_id"])
                save(tx, rkey(rid), record)
                tx.put(ROOT + b"Q/" + rid.encode(), raw_vector)
                self.index.upsert(tx, record, raw_vector, self.vector_space, expected=expected_projection)
                save(tx, self.dedupe_key(record, fingerprint), rid)
                for directory in (b"D", b"S", b"L"):
                    tx.put(self.directory(record, directory, rid.encode()), rid.encode())
                if item.fact_key:
                    tx.put(self.directory(record, b"B", rid.encode()), rid.encode())
                    tx.put(self.directory(record, b"T", f"{record['known_at']:016x}/".encode() + rid.encode()), rid.encode())
                self._audit(tx, record)
                if not record_id:
                    save(tx, ROOT + b"count", load(tx, ROOT + b"count", 0) + 1)
                results.append(rid)
                changed = True
            output = [public(self.record(tx, rid)) for rid in results]
            if changed:
                tx.commit(force_reliable=True)
            return output
        finally:
            tx.close()

    def listing(self, view, selector, *, scan_limit=100000):
        values = asdict(selector)
        directory = b"S" if selector.source_id else b"B" if selector.fact_key else b"D"
        prefix = self.directory(values, directory)
        result, scanned = [], 0
        after = selector.after.encode() + b"\x00" if selector.after else b""
        with view.scan(prefix + after, prefix_end(prefix)) as cursor:
            for _, raw, *_ in cursor:
                scanned += 1
                if scanned > scan_limit:
                    raise RagLimit("management browse scan budget exhausted")
                record = self.record(view, raw.decode(), visible=False)
                if (record["scope"], record["owner_id"], record["timeline"]) != (selector.scope, selector.owner_id, selector.timeline) or (
                    selector.source_id and record["source_id"] != selector.source_id):
                    raise RagIntegrityError("management directory crossed selector boundary")
                if self.blocked(view, record) or (selector.fact_key and record["fact_key"] != selector.fact_key):
                    continue
                if selector.as_of is not None and not self.time_visible(record, selector.as_of):
                    continue
                result.append(public(record))
                if len(result) > selector.limit:
                    break
        return {"items": result[:selector.limit], "next_after": result[selector.limit - 1]["id"] if len(result) > selector.limit else ""}

    def history(self, view, record_id, after_revision, limit):
        record = self.record(view, record_id)
        prefix = ROOT + b"A/" + record["lineage_id"].encode() + b"/"
        result = []
        with view.scan(prefix + f"{after_revision + 1:016x}".encode(), prefix_end(prefix)) as cursor:
            scanned = 0
            for key, value, *_ in cursor:
                scanned += 1
                if scanned > 100000:
                    raise RagLimit("history scan budget exhausted")
                item = json.loads(value)
                row = item.get("snapshot")
                if row is None:
                    row = public(self.record(view, item["record_id"], visible=False))
                if self.blocked(view, row):
                    continue
                if row["lineage_id"] != record["lineage_id"] or any(row[name] != record[name] for name in ("scope", "owner_id", "timeline")):
                    raise RagIntegrityError("audit entry crossed lineage/namespace boundary")
                if row["revision"] != int(key.rsplit(b"/", 1)[1], 16):
                    raise RagIntegrityError("audit cursor and record revision disagree")
                result.append(row)
                if len(result) > limit:
                    break
        return {"items": result[:limit], "next_revision": result[limit - 1]["revision"] if len(result) > limit else None}

    @staticmethod
    def time_visible(record, when):
        return record["known_at"] <= when and (record["expires_at"] is None or when < record["expires_at"])

    def start_purge(self, selector, *, retire=False):
        job_id = digest([selector, retire]).decode()
        tx = self.store.transaction()
        try:
            existing = load(tx, ROOT + b"J/" + job_id.encode())
            if existing is not None:
                return job_id
            if len(take(tx, ROOT + b"J/", 65)) >= 64:
                raise RagLimit("too many unfinished lifecycle tasks")
            state = self.namespace(tx, selector["scope"], selector["owner_id"], selector["timeline"])
            if len(state["jobs"]) >= 16 or state["epoch"] >= MAX_VERSION:
                raise RagLimit("namespace cleanup budget exhausted")
            state["epoch"] += 1
            state["jobs"].append(job_id)
            save(tx, nkey(state["scope"], state["owner_id"], state["timeline"]), state)
            save(tx, ROOT + b"J/" + job_id.encode(), dict(selector=selector, retire=retire, deleted=0, current=None))
            tx.commit(force_reliable=True)
            return job_id
        finally:
            tx.close()

    def _delete_record(self, tx, record):
        rid = record["id"]
        self.index.erase(tx, record)
        for directory in (b"D", b"S", b"L"):
            tx.delete(self.directory(record, directory, rid.encode()))
        if record["fact_key"]:
            tx.delete(self.directory(record, b"B", rid.encode()))
            tx.delete(self.directory(record, b"T", f"{record['known_at']:016x}/".encode() + rid.encode()))
            latest = self._latest_fact(tx, record)
            if latest is None:
                tx.delete(self.fact_key(record))
            else:
                fact = load(tx, self.fact_key(record), {"lineage_id": latest["lineage_id"], "head_id": None})
                if fact["head_id"] == rid:
                    fact["head_id"] = None
                save(tx, self.fact_key(record), fact)
        tx.delete(self.dedupe_key(record, record["_dedupe"]))
        tx.delete(ROOT + b"Q/" + rid.encode())
        tx.delete(rkey(rid))
        save(tx, ROOT + b"count", load(tx, ROOT + b"count", 0) - 1)
        if not take(tx, self.directory(record, b"L"), 1):
            tx.delete(ROOT + b"C/" + record["lineage_id"].encode())

    def purge_step(self, job_id, *, batch_size=128, history_batch=64):
        tx = self.store.transaction()
        try:
            key = ROOT + b"J/" + job_id.encode()
            job = load(tx, key)
            if job is None:
                return True, 0
            selector = job["selector"]
            state = self.namespace(tx, selector["scope"], selector["owner_id"], selector["timeline"])
            if job_id not in state["jobs"]:
                raise RagIntegrityError("cleanup task has no namespace barrier")
            directory = b"L" if selector.get("lineage_id") else b"S" if selector.get("source_id") else b"D"
            prefix = self.directory(selector, directory)
            done = False
            for _ in range(batch_size):
                rid = job["current"]
                if rid is None:
                    candidates = take(tx, prefix, 1)
                    if not candidates:
                        done = True
                        break
                    rid = candidates[0][1].decode()
                record = self.record(tx, rid, visible=False)
                if not selected(record, selector):
                    raise RagIntegrityError("cleanup directory selected another owner/source")
                revisions = take(tx, ROOT + b"V/" + rid.encode() + b"/", history_batch)
                for revision_key, audit_key in revisions:
                    tx.delete(audit_key)
                    tx.delete(revision_key)
                if take(tx, ROOT + b"V/" + rid.encode() + b"/", 1):
                    job["current"] = rid
                    break
                self._delete_record(tx, record)
                job["deleted"] += 1
                job["current"] = None
            if not done and job["current"] is None and not take(tx, prefix, 1):
                done = True
            if done:
                state["jobs"].remove(job_id)
                if not selector.get("source_id") and not selector.get("lineage_id"):
                    if state["generation"] >= MAX_VERSION:
                        raise RagLimit("namespace generation exhausted")
                    state["generation"] += 1
                    if job["retire"]:
                        state["active"] = False
                save(tx, nkey(state["scope"], state["owner_id"], state["timeline"]), state)
                tx.delete(key)
            else:
                save(tx, key, job)
            tx.commit(force_reliable=True)
            return done, job["deleted"]
        finally:
            tx.close()

    def recover(self, guard=None):
        count = 0
        while True:
            with self.store.snapshot() as view:
                jobs = take(view, ROOT + b"J/", 1)
            if not jobs:
                return count
            job_id = jobs[0][0].rsplit(b"/", 1)[1].decode()
            while True:
                if guard:
                    guard.check()
                done, _ = self.purge_step(job_id)
                if done:
                    count += 1
                    break
