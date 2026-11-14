"""Independent stdlib wire/lease contracts and actual FengquIndex ABI integration."""

import ctypes as C
import json
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from collections import namedtuple
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from bindings.python.fengqudb import Error, Slice, Store
from bindings.python import fengqudb_index as ffi
from bindings.python import fengqudb_index_wire as wire

CORE = os.environ.get("FENGQUDB_LIBRARY") or os.environ.get("FQDB_LIBRARY")
INDEX = os.environ.get("FENGQUDB_INDEX_LIBRARY")


def result_bytes(documents=(), *, termination=0, evidence=None, total=None, count=0):
    evidence = (1 if documents else 2) if evidence is None else evidence
    result = wire._u(2, 7) + wire._u(3, 3) + wire._u(5, termination) + wire._u(7, evidence)
    result += wire._u(13, count) + wire._b(15, b"fixture") + wire._b(16, b"fqidx-utf8-v1")
    for document in documents:
        result += wire._b(1, wire._b(1, wire.encode_document(document)) + wire._d(2, 1.5))
    if total is not None:
        result += wire._u(12, total)
    return result


class Function:
    def __init__(self, function):
        self.function = function
    def __call__(self, *args):
        return self.function(*args)


class CoreFixture:
    def __init__(self):
        self.freed = 0
        self.buffers = []
        self.fqdb_store_bridge_abi_version = C.CFUNCTYPE(C.c_uint32)(lambda: 1)
        self.fqdb_abi_version = C.CFUNCTYPE(C.c_uint32)(lambda: 2)
        self.status_buffer = C.create_string_buffer(b"fixture status")
        self.fqdb_status_message = C.CFUNCTYPE(C.c_void_p, C.c_int32)(lambda _status: C.addressof(self.status_buffer))
        self.fqdb_buffer_free = C.CFUNCTYPE(None, C.POINTER(Slice))(self.free)
    def free(self, pointer):
        self.freed += 1
        output = C.cast(pointer, C.POINTER(Slice)).contents
        output.data, output.size = None, 0


class LibraryFixture:
    def __init__(self, core):
        self.core, self.calls = core, []
        self.status, self.response = 0, b"fixture-response"
        self.entered, self.release = None, None
        self.fqidx_abi_version = Function(lambda: 1)
        self.fqidx_core_bridge_address = Function(lambda: C.cast(core.fqdb_store_bridge_abi_version, C.c_void_p).value)
        self.fqidx_txn_apply = Function(self.apply)
        self.fqidx_txn_query = Function(lambda *args: self.query("transaction", *args))
        self.fqidx_snapshot_query = Function(lambda *args: self.query("snapshot", *args))
    def apply(self, pointer, token, request):
        self.calls.append(("apply", pointer, token, C.string_at(request.data, request.size)))
        return self.status
    def query(self, kind, pointer, token, request, control, output):
        self.calls.append((kind, pointer, token, C.string_at(request.data, request.size)))
        if self.entered:
            self.entered.set()
            if not self.release.wait(5):
                raise RuntimeError("fixture wait timeout")
        if control:
            callback = C.cast(control, C.POINTER(ffi._Control)).contents.cancelled
            if callback(None):
                return 19
        buffer = C.create_string_buffer(self.response)
        self.core.buffers.append(buffer)
        result = C.cast(output, C.POINTER(Slice)).contents
        result.data, result.size = C.cast(buffer, C.c_void_p), len(self.response)
        return self.status


Identity = namedtuple("Identity", "struct_size abi_version kind reserved store_token snapshot_version uuid")


class HandleFixture:
    def __init__(self, core, kind=2, token=17):
        self.library, self.kind, self.token = core, kind, token
        self.active, self.closed = 0, False
        self.condition = threading.Condition()
    @contextmanager
    def borrow_native(self, expected_store_token=None):
        with self.condition:
            if self.closed:
                raise RuntimeError("closed fixture")
            if expected_store_token is not None and expected_store_token != self.token:
                raise ValueError("wrong Store token")
            self.active += 1
        try:
            yield SimpleNamespace(pointer=4096, library=self.library,
                identity=Identity(48, 1, self.kind, 0, self.token, 7, bytes(16)))
        finally:
            with self.condition:
                self.active -= 1
                self.condition.notify_all()
    def close(self):
        with self.condition:
            self.condition.wait_for(lambda: self.active == 0)
            self.closed = True


class NativeTransportTests(unittest.TestCase):
    def setUp(self):
        self.core = CoreFixture()
        self.library = LibraryFixture(self.core)
        with patch.object(ffi.C, "CDLL", return_value=self.library):
            self.client = ffi.NativeIndex("fixture-library")
        self.handle = HandleFixture(self.core)

    def test_public_borrow_selects_handle_kind_and_frees_before_return(self):
        self.client.apply(self.handle, b"request", expected_store_token=17)
        self.assertEqual(b"fixture-response", self.client.query(self.handle, b"request"))
        snapshot = HandleFixture(self.core, kind=3)
        self.client.query(snapshot, b"snapshot")
        self.assertEqual(["apply", "transaction", "snapshot"], [call[0] for call in self.library.calls])
        self.assertEqual(2, self.core.freed)
        self.assertEqual(0, self.handle.active)

    def test_wrong_core_store_and_handle_kind_fail_before_call(self):
        wrong = HandleFixture(CoreFixture())
        with self.assertRaises(ffi.IndexBridgeError):
            self.client.apply(wrong, b"request")
        with self.assertRaises(ValueError):
            self.client.apply(self.handle, b"request", expected_store_token=99)
        with self.assertRaises(ffi.IndexBridgeError):
            self.client.apply(HandleFixture(self.core, kind=3), b"request")
        with self.assertRaises(ffi.IndexBridgeError):
            self.client.query(HandleFixture(self.core, kind=1), b"request")
        self.assertFalse(self.library.calls)

    def test_error_and_oversized_output_always_release_owned_buffer(self):
        self.library.status = 9
        with self.assertRaises(Error) as error:
            self.client.query(self.handle, b"request")
        self.assertEqual(9, error.exception.code)
        self.assertEqual(1, self.core.freed)
        self.library.status = 0
        self.client.max_response_bytes = 4
        with self.assertRaises(ffi.IndexBridgeError):
            self.client.query(self.handle, b"request")
        self.assertEqual(2, self.core.freed)

    def test_close_waits_for_inflight_query_lease(self):
        self.library.entered, self.library.release = threading.Event(), threading.Event()
        with ThreadPoolExecutor(2) as pool:
            query = pool.submit(self.client.query, self.handle, b"request")
            self.assertTrue(self.library.entered.wait(5))
            closing = pool.submit(self.handle.close)
            self.assertFalse(closing.done())
            self.library.release.set()
            self.assertEqual(b"fixture-response", query.result(5))
            closing.result(5)
        self.assertTrue(self.handle.closed)
        self.assertEqual(1, self.core.freed)

    def test_cancel_callback_and_missing_public_borrow(self):
        control = ffi.QueryControl(lambda: True)
        with self.assertRaises(Error) as error:
            self.client.query(self.handle, b"request", control=control)
        self.assertEqual(19, error.exception.code)
        with self.assertRaises(ffi.IndexUnavailable):
            self.client.query(object(), b"request")
        def broken():
            raise RuntimeError("callback fixture failure")
        control = ffi.QueryControl(broken)
        self.assertEqual(1, control.native.cancelled(None))
        self.assertEqual("RuntimeError", control.error)

    def test_unsupported_native_abi_is_rejected(self):
        self.library.fqidx_abi_version = Function(lambda: 2)
        with patch.object(ffi.C, "CDLL", return_value=self.library):
            with self.assertRaises(ffi.IndexUnavailable):
                ffi.NativeIndex("fixture-library")


class NativeWireTests(unittest.TestCase):
    def test_scalar_types_preserve_large_integer_and_bytes_identity(self):
        for value in (None, True, False, -(2 ** 63), 2 ** 63 - 1, ffi.UInt(2 ** 64 - 1), "中文", b"\0bytes", 1.25):
            self.assertEqual(value, wire.decode_scalar(wire.scalar(value)))
        with self.assertRaises(wire.WireError):
            wire.scalar(float("nan"))
        with self.assertRaises(wire.WireError):
            wire.decode_scalar(wire._u(1, 1) + wire._u(4, 1))

    def test_known_field_types_strict_and_unknown_fields_forward_skipped(self):
        result = wire.decode_result(wire._u(100, 123) + wire._b(101, b"future") + wire._u(7, 2))
        self.assertEqual(2, result["match_evidence"])
        self.assertIsNone(result["total_hits"])
        with self.assertRaises(wire.WireError):
            wire.decode_result(wire._b(2, b"wrong snapshot version wire"))
        for malformed in (b"\x80", b"\x0a\x05xx", b"\x00", b"\x80" * 10 + b"\x00"):
            with self.assertRaises(wire.WireError):
                wire.decode_result(malformed)

    def test_document_result_retains_locator_payload_and_presence(self):
        document = ffi.Document(b"opaque/id", 3, 5, 1, texts={"body": "中文原件"}, metadata={"visible": True},
                                source_locator=b"opaque-locator", source_revision=4, payload=b"\0not-json")
        result = wire.decode_result(result_bytes([document], total=1))
        decoded = result["hits"][0]["document"]
        self.assertEqual(b"\0not-json", decoded["payload"])
        self.assertEqual(b"opaque-locator", decoded["source_locator"])
        self.assertEqual(("中文原件", 1.0), decoded["texts"]["body"])
        self.assertEqual(1, result["total_hits"])
        self.assertEqual(0, wire.decode_result(result_bytes(total=0))["total_hits"])

    def test_predicate_vector_and_unsupported_strict_statistics(self):
        predicate = ffi.Predicate(9, children=(ffi.Predicate(1, "kind", ("chunk",)), ffi.Predicate(8, "visible")))
        request = wire.encode_query(index_space=b"space", partition=b"private", where=predicate)
        self.assertTrue(any(number == 6 for number, _, _ in wire.fields(request)))
        self.assertEqual(struct.pack("<ff", 1.0, 2.0), ffi.vector([1.0, 2.0]))
        for bad in ([0.0, 0.0], [float("inf")], b"bad"):
            with self.assertRaises(wire.WireError):
                ffi.vector(bad)
        with self.assertRaises(wire.WireError):
            wire.encode_query(index_space=b"space", partition=b"p", statistics_policy=1)
        with self.assertRaises(wire.WireError):
            wire.encode_predicate(ffi.Predicate(11, children=()))
        for arguments in ({"mode": 1, "text": "ignored"}, {"vector_space": b"ignored"},
                          {"min_similarity": 0.0}, {"mode": 1, "operation": ffi.Query.STATS},
                          {"mode": 1, "vector_f32_le": ffi.vector([1.0]), "vector_space": b"v", "min_similarity": 1.1},
                          {"mode": 1, "vector_f32_le": ffi.vector([1.0]), "vector_space": b"v", "match_all_terms": True}):
            with self.assertRaises(wire.WireError):
                wire.encode_query(index_space=b"space", partition=b"p", **arguments)



@unittest.skipUnless(CORE and INDEX, "actual Core bridge + native index library have not been supplied")
class NativeLibraryIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.client = ffi.NativeIndex(INDEX)
        self.path = Path(self.directory.name) / "db"
    def tearDown(self):
        self.directory.cleanup()

    def test_external_transaction_query_and_failed_apply_cancels_everything(self):
        instance = {"index_space": b"integration", "partition": b"p"}
        document = ffi.Document(b"id", 1, 7, 1, texts={"body": "nativeatomicterm"}, metadata={"visible": True})
        with Store(self.path, library=CORE) as store:
            token = store.native_identity().store_token
            with store.transaction() as transaction:
                transaction.put(b"domain", b"original")
                self.client.mutate(transaction, **instance, operation=ffi.Mutation.UPSERT,
                                   document=document, expected_store_token=token)
                result = self.client.execute(transaction, **instance, operation=ffi.Query.GET, id=b"id")
                self.assertEqual(1, result["hits"][0]["document"]["projection_version"])
                transaction.commit(force_reliable=True)
            transaction = store.transaction()
            try:
                transaction.put(b"domain", b"must-not-commit")
                with self.assertRaises(Error):
                    self.client.mutate(transaction, **instance, operation=ffi.Mutation.ERASE, id=b"id",
                        expected_projection_version=99, next_projection_version=100, lifecycle_generation=1)
                with self.assertRaises(Error):
                    transaction.commit(force_reliable=True)
            finally:
                # A deliberately poisoned txn cannot use the context manager's
                # normal-exit auto-commit path.
                transaction.close()
            self.assertEqual(b"original", store.get(b"domain"))
            transaction = store.transaction()
            try:
                transaction.put(b"domain", b"wire-validation-must-not-commit")
                with self.assertRaises(wire.WireError):
                    self.client.mutate(transaction, index_space=b"", partition=b"p",
                                       operation=ffi.Mutation.UPSERT, document=document)
                with self.assertRaises(Error):
                    transaction.commit(force_reliable=True)
            finally:
                transaction.close()
            self.assertEqual(b"original", store.get(b"domain"))
            with store.snapshot() as snapshot:
                result = self.client.execute(snapshot, **instance, text="nativeatomicterm")
                self.assertTrue(result["hits"])
                with self.assertRaises(Error) as error:
                    self.client.execute(snapshot, **instance, text="nativeatomicterm", control=ffi.QueryControl(lambda: True))
                self.assertEqual(19, error.exception.code)


    def test_dense_filter_metadata_patch_and_guarded_list(self):
        instance = {"index_space": b"dense-integration", "partition": b"private"}
        with Store(self.path, library=CORE) as store:
            with store.transaction() as transaction:
                for record_id, values, group in ((b"a", [1.0, 0.0], "keep"), (b"b", [0.0, 1.0], "drop")):
                    self.client.mutate(transaction, **instance, operation=ffi.Mutation.UPSERT,
                        document=ffi.Document(record_id, 1, 7, 1, metadata={"group": group},
                                              vector_f32_le=ffi.vector(values), vector_space=b"vectors-v1"))
                transaction.commit(force_reliable=True)
            with store.snapshot() as snapshot:
                found = self.client.execute(snapshot, **instance, mode=1, vector_f32_le=ffi.vector([1.0, 0.0]),
                    vector_space=b"vectors-v1", where=ffi.Predicate(1, "group", ("keep",)))
                self.assertEqual([b"a"], [hit["document"]["id"] for hit in found["hits"]])
                self.assertAlmostEqual(1.0, found["hits"][0]["dense_similarity"])
            with store.transaction() as transaction:
                self.client.mutate(transaction, **instance, operation=ffi.Mutation.PATCH, id=b"a",
                    expected_projection_version=1, next_projection_version=2, lifecycle_generation=1,
                    document=ffi.Document(b"a", metadata={"group": "drop"}))
                transaction.commit(force_reliable=True)
            with store.snapshot() as snapshot:
                found = self.client.execute(snapshot, **instance, mode=1, vector_f32_le=ffi.vector([1.0, 0.0]),
                    vector_space=b"vectors-v1", where=ffi.Predicate(1, "group", ("keep",)))
                self.assertFalse(found["hits"])
                self.assertEqual(2, found["match_evidence"])
                first = self.client.execute(snapshot, **instance, operation=ffi.Query.LIST, limit=1)
                second = self.client.execute(snapshot, **instance, operation=ffi.Query.LIST, limit=1,
                    after_id=first["next_cursor"], expected_mutation_sequence=first["mutation_sequence"])
                self.assertEqual(b"a", first["hits"][0]["document"]["id"])
                self.assertEqual(b"b", second["hits"][0]["document"]["id"])
                with self.assertRaises(Error):
                    self.client.execute(snapshot, **instance, operation=ffi.Query.GET, id=b"a", expected_projection_version=1)
                current = self.client.execute(snapshot, **instance, operation=ffi.Query.GET, id=b"a", expected_projection_version=2)
                self.assertEqual(7, current["hits"][0]["document"]["domain_revision"])
                self.assertEqual(ffi.vector([1.0, 0.0]), current["hits"][0]["document"]["vector_f32_le"])

    def test_separately_loaded_core_image_is_rejected(self):
        copied = Path(self.directory.name) / ("foreign-core.dll" if os.name == "nt" else "foreign-core.so")
        shutil.copyfile(CORE, copied)
        # The child releases Windows' DLL file mapping on exit; no private
        # FreeLibrary or ctypes handle access is used for fixture cleanup.
        code = """import os,sys
from bindings.python.fengqudb import Store
from bindings.python.fengqudb_index import NativeIndex,IndexBridgeError
client=NativeIndex(os.environ['FENGQUDB_INDEX_LIBRARY'])
with Store(sys.argv[2],library=sys.argv[1]) as store:
    with store.snapshot() as snapshot:
        try: client.execute(snapshot,index_space=b'wrong-core',partition=b'p',operation=3)
        except IndexBridgeError: print('rejected')
        else: raise RuntimeError('foreign Core image was accepted')
"""
        result = subprocess.run([sys.executable, "-c", code, str(copied), str(Path(self.directory.name) / "foreigndb")],
                                capture_output=True, text=True, check=True, timeout=20)
        self.assertIn("rejected", result.stdout)

    def test_public_binding_module_alias_keeps_its_buffer_types(self):
        binding = Path(__file__).resolve().parents[1] / "bindings/python/fengqudb.py"
        specification = importlib.util.spec_from_file_location("fqdb_public_alias_fixture", binding)
        alias = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(alias)
        with alias.Store(self.path, library=CORE) as store:
            with store.transaction() as transaction:
                transaction.put(b"alias", b"buffer-owner")
                transaction.commit(force_reliable=True)
            with store.snapshot() as snapshot:
                self.client.execute(snapshot, index_space=b"alias", partition=b"p", operation=ffi.Query.STATS)
                self.assertEqual(b"buffer-owner", snapshot.get(b"alias"))
            self.assertEqual(b"buffer-owner", store.get(b"alias"))


if __name__ == "__main__":
    unittest.main()
