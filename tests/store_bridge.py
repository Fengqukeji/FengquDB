"""Standalone public Store bridge lifecycle tests; no model or Index dependency.

python tests/store_bridge.py --library <fresh Core> [--legacy-library <ABI2 Core>]
"""
import argparse
import ctypes as C
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bindings/python"))
import fengqudb as fq

parser = argparse.ArgumentParser()
parser.add_argument("--library", required=True)
parser.add_argument("--legacy-library")
args, remaining = parser.parse_known_args()
LIBRARY = str(Path(args.library).resolve())
LEGACY_LIBRARY = str(Path(args.legacy_library).resolve()) if args.legacy_library else None


class Info(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("abi_version", C.c_uint32),
                ("kind", C.c_uint32), ("reserved", C.c_uint32),
                ("store_token", C.c_uint64), ("snapshot_version", C.c_uint64),
                ("uuid", C.c_ubyte * 16)]


def identity_function(lease, kind=None):
    kind = kind or lease.identity.kind
    suffix = {1: "bridge_identity", 2: "txn_bridge_identity", 3: "snapshot_bridge_identity"}[kind]
    return C.CFUNCTYPE(C.c_int32, C.c_void_p, C.c_void_p)(
        ("fqdb_store_" + suffix, lease.library))


def native_identity(lease):
    info = Info(C.sizeof(Info), 1)
    code = identity_function(lease)(lease.pointer, C.byref(info))
    return code, info


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.path = Path(self.folder.name) / "db"
        self.db = fq.Store(self.path, library=LIBRARY, buffered=True)
        self.addCleanup(self.db.close)

    def test_public_identity_layout_and_validation(self):
        self.assertEqual(C.sizeof(Info), 48)
        info = self.db.native_identity()
        self.assertEqual((info.struct_size, info.abi_version, info.kind, info.reserved), (48, 1, 1, 0))
        self.assertNotEqual(info.store_token, 0)
        self.assertEqual(len(info.uuid), 16)
        self.assertNotEqual(info.uuid, bytes(16))
        with self.assertRaises(AttributeError):
            info.kind = 9
        with self.db.borrow_native() as lease:
            self.assertIsInstance(lease.library, C.CDLL)
            self.assertIsInstance(lease.pointer, int)
            self.assertEqual(lease.library.fqdb_store_bridge_abi_version(), 1)
            for kind in (1, 2, 3):
                function = identity_function(lease, kind)
                output = Info(48, 1, 9, 9, 9, 9)
                self.assertEqual(function(None, C.byref(output)), 3)
                self.assertEqual((output.kind, output.store_token, output.snapshot_version), (0, 0, 0))
                self.assertEqual(function(None, None), 3)
            function = identity_function(lease)
            for size, abi in ((0, 1), (47, 1), (48, 0), (48, 2)):
                output = Info(size, abi, 9, 9, 9, 9)
                before = bytes(output)
                self.assertEqual(function(lease.pointer, C.byref(output)), 3)
                self.assertEqual(bytes(output), before)
            # An oversized forward-compatible buffer must retain its tail.
            buffer = (C.c_ubyte * 64)()
            C.memset(buffer, 0xA5, 64)
            output = C.cast(buffer, C.POINTER(Info)).contents
            output.struct_size, output.abi_version = 64, 1
            self.assertEqual(function(lease.pointer, buffer), 0)
            self.assertEqual(bytes(buffer)[48:], b"\xa5" * 16)

    def test_snapshot_and_transaction_base_version(self):
        self.db.put(b"key", b"old")
        tx, snap = self.db.transaction(), self.db.snapshot()
        self.addCleanup(tx.close)
        self.addCleanup(snap.close)
        before = self.db.native_identity()
        self.db.put(b"key", b"new")
        self.assertGreater(self.db.native_identity().snapshot_version, before.snapshot_version)
        for value, kind in ((tx, 2), (snap, 3)):
            identity = value.native_identity()
            self.assertEqual((identity.kind, identity.store_token, identity.uuid),
                             (kind, before.store_token, before.uuid))
            self.assertEqual(identity.snapshot_version, before.snapshot_version)
            self.assertEqual(value.get(b"key"), b"old")

    def test_other_store_rejected_without_leaking_borrow(self):
        with fq.Store(Path(self.folder.name) / "other", library=LIBRARY, buffered=True) as other:
            token = other.native_identity().store_token
            self.assertNotEqual(token, self.db.native_identity().store_token)
            tx, snap = self.db.transaction(), self.db.snapshot()
            try:
                for owner in (self.db, tx, snap):
                    with self.assertRaises(ValueError):
                        with owner.borrow_native(expected_store_token=token):
                            self.fail("wrong Store accepted")
                    self.assertNotEqual(owner.native_identity().store_token, token)
            finally:
                tx.close()
                snap.close()

    def test_same_thread_close_and_pointer_scope(self):
        lease = self.db.borrow_native()
        with self.assertRaises(RuntimeError):
            _ = lease.pointer
        with lease:
            with self.assertRaises(RuntimeError):
                self.db.close()
            self.assertEqual(native_identity(lease)[0], 0)
            with self.db.borrow_native(lease.identity.store_token) as nested:
                self.assertEqual(nested.pointer, lease.pointer)
        with self.assertRaises(RuntimeError):
            _ = lease.pointer
        with self.assertRaises(RuntimeError):
            lease.__enter__()
        self.db.put(b"live", b"yes")
        self.assertEqual(self.db.get(b"live"), b"yes")

    def exercise_close_race(self, owner):
        started, finished = threading.Event(), threading.Event()
        errors = []

        def close():
            try:
                started.set()
                owner.close()
            except BaseException as exc:
                errors.append(exc)
            finally:
                finished.set()

        with owner.borrow_native() as lease:
            thread = threading.Thread(target=close, daemon=True)
            thread.start()
            self.assertTrue(started.wait(2))
            self.assertFalse(finished.wait(0.05))
            self.assertEqual(native_identity(lease)[0], 0)
            # The raw lease remains usable even after public close has begun.
            failure = []

            def wrong_thread():
                try:
                    _ = lease.pointer
                except RuntimeError:
                    failure.append(True)

            probe = threading.Thread(target=wrong_thread)
            probe.start()
            probe.join(2)
            self.assertEqual(failure, [True])
        thread.join(3)
        self.assertFalse(thread.is_alive(), "close did not finish after lease exit")
        self.assertEqual(errors, [])
        with self.assertRaises(RuntimeError):
            owner.native_identity()

    def test_close_waits_for_store_snapshot_and_transaction_leases(self):
        tx, snap = self.db.transaction(), self.db.snapshot()
        self.addCleanup(tx.close)
        self.addCleanup(snap.close)
        for owner in (tx, snap, self.db):
            self.exercise_close_race(owner)

    def test_explicit_counter_lifetime_path(self):
        with patch.object(fq, "_REFERENCE_LEASES", False):
            store = fq.Store(Path(self.folder.name) / "counter", library=LIBRARY, buffered=True)
            tx, snap = store.transaction(), store.snapshot()
        self.addCleanup(store.close)
        self.addCleanup(tx.close)
        self.addCleanup(snap.close)
        for owner in (tx, snap, store):
            self.exercise_close_race(owner)

    def test_cancel_is_not_blocked_by_transaction_lease(self):
        tx = self.db.transaction()
        self.addCleanup(tx.close)
        with tx.borrow_native() as lease:
            thread = threading.Thread(target=tx.cancel)
            thread.start()
            thread.join(2)
            self.assertFalse(thread.is_alive(), "cancel waited on the transaction operation lock")
            self.assertEqual(native_identity(lease)[0], 19)
            with self.assertRaises(fq.Error) as caught:
                tx.put(b"cancelled", b"not stored")
            self.assertEqual(caught.exception.code, 19)
        with self.assertRaises(fq.Error) as caught:
            tx.native_identity()
        self.assertEqual(caught.exception.code, 19)
        self.assertIsNone(self.db.get(b"cancelled"))

    def test_commit_serialized_and_sealed_transaction_rejected(self):
        tx = self.db.transaction()
        self.addCleanup(tx.close)
        started, finished = threading.Event(), threading.Event()
        errors = []

        def commit():
            try:
                started.set()
                tx.commit()
            except BaseException as exc:
                errors.append(exc)
            finally:
                finished.set()

        with tx.borrow_native() as lease:
            tx.put(b"committed", b"value")
            thread = threading.Thread(target=commit, daemon=True)
            thread.start()
            self.assertTrue(started.wait(2))
            self.assertFalse(finished.wait(0.05))
            self.assertEqual(native_identity(lease)[0], 0)
        thread.join(3)
        self.assertFalse(thread.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual(self.db.get(b"committed"), b"value")
        with self.assertRaises(fq.Error) as caught:
            tx.native_identity()
        self.assertEqual(caught.exception.code, 3)

    def test_capture_keeps_core_alive_after_store_close(self):
        self.db.put(b"captured", b"value")
        original = self.db.native_identity()
        tx, snap = self.db.transaction(), self.db.snapshot()
        try:
            self.db.close()
            for owner in (tx, snap):
                self.assertEqual(owner.native_identity().store_token, original.store_token)
                self.assertEqual(owner.get(b"captured"), b"value")
            with self.assertRaises(fq.Error) as caught:
                fq.Store(self.path, library=LIBRARY, create=False, buffered=True)
            self.assertEqual(caught.exception.code, 6)
            tx.put(b"after-close", b"safe")
            # This fixture explicitly opened BUFFERED; persistence needs a barrier.
            tx.commit(force_reliable=True)
            self.assertEqual(snap.get(b"captured"), b"value")
        finally:
            tx.close()
            snap.close()
        with fq.Store(self.path, library=LIBRARY, create=False, buffered=True) as reopened:
            identity = reopened.native_identity()
            self.assertEqual(identity.uuid, original.uuid)
            self.assertNotEqual(identity.store_token, original.store_token)
            self.assertEqual(reopened.get(b"after-close"), b"safe")

    def test_reopen_tokens_and_compaction_uuid(self):
        original = self.db.native_identity()
        self.db.put(b"kept", b"text")
        self.db.checkpoint()
        self.db.compact()
        self.assertEqual(self.db.native_identity().uuid, original.uuid)
        self.assertEqual(self.db.native_identity().store_token, original.store_token)
        self.db.close()
        tokens = {original.store_token}
        for _ in range(16):
            with fq.Store(self.path, library=LIBRARY, create=False, buffered=True) as reopened:
                identity = reopened.native_identity()
                self.assertEqual(identity.uuid, original.uuid)
                self.assertNotIn(identity.store_token, tokens)
                tokens.add(identity.store_token)
                with self.assertRaises(ValueError):
                    with reopened.borrow_native(original.store_token):
                        self.fail("old lifetime token accepted")

    def test_timeout_and_failed_context_release(self):
        tx = self.db.transaction(timeout_ms=1)
        self.addCleanup(tx.close)
        time.sleep(0.01)
        with self.assertRaises(fq.Error) as caught:
            tx.native_identity()
        self.assertEqual(caught.exception.code, 17)
        tx.close()
        with self.assertRaisesRegex(LookupError, "inside"):
            with self.db.borrow_native():
                raise LookupError("inside")
        self.db.close()

    def test_invalid_expected_tokens(self):
        for value in (0, -1, True, False, 1 << 64, "1", 1.0):
            with self.assertRaises(ValueError):
                self.db.borrow_native(value)

    def test_optional_symbols_are_lazy(self):
        real_cdll = C.CDLL
        bridge_lookups = []

        class LegacyLibrary:
            def __init__(self, *values, **kwargs):
                self.library = real_cdll(*values, **kwargs)
            def __getattr__(self, name):
                if "bridge" in name:
                    bridge_lookups.append(name)
                    raise AttributeError(name)
                return getattr(self.library, name)

        with patch.object(C, "CDLL", LegacyLibrary):
            with fq.Store(Path(self.folder.name) / "old-symbols", library=LIBRARY, buffered=True) as old:
                old.put(b"old", b"works")
                self.assertEqual(bridge_lookups, [])
                for value in (old, old.transaction(), old.snapshot()):
                    try:
                        with self.assertRaises(fq.Error) as caught:
                            value.native_identity()
                        self.assertEqual(caught.exception.code, 12)
                        self.assertEqual(value.get(b"old"), b"works")
                    finally:
                        if value is not old:
                            value.close()
                old.put(b"still", b"works")
                self.assertEqual(old.get(b"still"), b"works")

    @unittest.skipUnless(LEGACY_LIBRARY, "provide --legacy-library for actual old ABI2 binary")
    def test_actual_legacy_library(self):
        with fq.Store(Path(self.folder.name) / "legacy", library=LEGACY_LIBRARY, buffered=True) as old:
            old.put(b"legacy", b"compatible")
            for value in (old, old.transaction(), old.snapshot()):
                try:
                    with self.assertRaises(fq.Error) as caught:
                        value.native_identity()
                    self.assertEqual(caught.exception.code, 12)
                    self.assertEqual(value.get(b"legacy"), b"compatible")
                finally:
                    if value is not old:
                        value.close()
            old.put(b"after", b"still-compatible")
            self.assertEqual(old.get(b"after"), b"still-compatible")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + remaining)
