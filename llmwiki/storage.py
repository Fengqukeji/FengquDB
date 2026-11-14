"""Use the repository's public binding; all wiki state lives in FengquDB."""

import json
import threading
from contextlib import contextmanager

try:
    from ..bindings.python.fengqudb import Error, Store
except ImportError:
    try:
        from bindings.python.fengqudb import Error, Store
    except ImportError:
        from fengqudb import Error, Store


def encode(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")


def canonical(value):
    """Object key order is irrelevant to request/content identity; array order remains explicit."""
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False, sort_keys=True).encode("utf-8")


def decode(value, default=None):
    return default if value is None else json.loads(value)


class View:
    def __init__(self, handle, prefix):
        self.handle, self.prefix = handle, prefix

    def get(self, key, default=None):
        return decode(self.handle.get(self.prefix + key.encode()), default)

    def get_many(self, keys):
        return [decode(v) for v in self.handle.get_many([self.prefix + k.encode() for k in keys])]

    def put(self, key, value):
        self.handle.put(self.prefix + key.encode(), encode(value))

    def delete(self, key):
        self.handle.delete(self.prefix + key.encode())

    def scan(self, prefix, *, after="", limit=None):
        # Keys use a restricted ASCII vocabulary; prefix+0xff is a strict bound.
        begin = self.prefix + (after or prefix).encode()
        end = self.prefix + prefix.encode() + b"\xff"
        with self.handle.scan(begin, end) as cursor:
            count = 0
            for key, value, _, _ in cursor:
                key = key[len(self.prefix):].decode()
                if key <= after:
                    continue
                yield key, decode(value)
                count += 1
                if limit is not None and count >= limit:
                    return


class Repository:
    def __init__(self, path, prefix, library=None):
        self.store = Store(path, library=library)
        self.prefix = prefix.encode()
        self.lock = threading.RLock()

    @contextmanager
    def snapshot(self):
        with self.store.snapshot() as handle:
            yield View(handle, self.prefix)

    def write(self, operation):
        # Only deterministic database work belongs here. Provider calls must be outside.
        with self.lock:
            for attempt in range(5):
                try:
                    with self.store.transaction(timeout_ms=5000) as tx:
                        result = operation(View(tx, self.prefix))
                        tx.commit(force_reliable=True)
                        return result
                except Error as exc:
                    if exc.code != 7 or attempt == 4:
                        raise

    def close(self):
        with self.lock:
            self.store.close()
