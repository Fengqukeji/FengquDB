"""Stdlib transport for FengquIndex ABI 1 and the public Core bridge ABI 1."""

import ctypes as C
import os
import sys
from dataclasses import dataclass

try:
    from . import fengqudb as _core_binding
    from .fengqudb import Error, Slice
    from .fengqudb_index_wire import (Document, Maintenance, Mutation, Phrase, Predicate, Projection, Query, UInt, WireError,
                                     decode_maintenance, decode_result, encode_maintenance, encode_mutation, encode_query, fields, vector)
except ImportError:
    import fengqudb as _core_binding
    from fengqudb import Error, Slice
    from fengqudb_index_wire import (Document, Maintenance, Mutation, Phrase, Predicate, Projection, Query, UInt, WireError,
                                    decode_maintenance, decode_result, encode_maintenance, encode_mutation, encode_query, fields, vector)


class IndexUnavailable(RuntimeError):
    pass


class IndexBridgeError(RuntimeError):
    pass


class _CoreFunctions:
    """Private prototypes over public symbols; do not retype the owner's table."""

    def __init__(self, library):
        self.owner = library
        address = lambda name: C.cast(getattr(library, name), C.c_void_p).value
        self.fqdb_abi_version = C.CFUNCTYPE(C.c_uint32)(address("fqdb_abi_version"))
        self.fqdb_status_message = C.CFUNCTYPE(C.c_char_p, C.c_int32)(address("fqdb_status_message"))
        self.fqdb_buffer_free = C.CFUNCTYPE(None, C.POINTER(Slice))(address("fqdb_buffer_free"))


@dataclass(frozen=True)
class CoreLeaseIdentity:
    """Normalized bridge facts, not a guessed layout for Core's public identity."""

    kind: str
    store_token: int
    core_abi: int
    core_bridge_address: int


_CancelFunction = C.CFUNCTYPE(C.c_uint32, C.c_void_p)


class _Control(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("abi_version", C.c_uint32),
                ("context", C.c_void_p), ("cancelled", _CancelFunction)]


class QueryControl:
    """Keep a cancellation callback alive until the synchronous query returns."""

    def __init__(self, cancelled):
        if not callable(cancelled):
            raise TypeError("cancelled must be callable, e.g. threading.Event.is_set")
        self.error = None
        def callback(_context):
            try:
                return int(bool(cancelled()))
            except BaseException as exc:
                self.error = type(exc).__name__
                return 1
        self.callback = _CancelFunction(callback)
        self.native = _Control(C.sizeof(_Control), 1, None, self.callback)


def public_bridge_identity(lease):
    identity, core = lease.identity, lease.library
    if identity.struct_size < 48 or identity.abi_version != 1 or identity.reserved or len(identity.uuid) != 16:
        raise IndexBridgeError("invalid public Core bridge identity")
    kinds = {2: "transaction", 3: "snapshot"}
    bridge_address = C.cast(core.fqdb_store_bridge_abi_version, C.c_void_p).value
    if C.CFUNCTYPE(C.c_uint32)(bridge_address)() != 1:
        raise IndexBridgeError("unsupported Core bridge ABI")
    return CoreLeaseIdentity(kinds.get(identity.kind, "store"), identity.store_token,
                             _CoreFunctions(core).fqdb_abi_version(), bridge_address)


class NativeIndex:
    """Attach calls to a publicly borrowed Core handle; never open another Store.

    The default identity adapter uses only StoreNativeLease's public properties.
    """

    def __init__(self, library=None, *, identity_adapter=None, max_response_bytes=64 * 1024 * 1024):
        if type(max_response_bytes) is not int or not 1 <= max_response_bytes <= sys.maxsize:
            raise ValueError("invalid index response byte budget")
        filename = "fengqudb_index.dll" if os.name == "nt" else (
            "libfengqudb_index.dylib" if sys.platform == "darwin" else "libfengqudb_index.so")
        configured = os.environ.get("FENGQUDB_INDEX_LIBRARY")
        resolver = getattr(_core_binding, "native_library_path", None)
        selected = library if library is not None else configured or (
            resolver("fengqudb_index") if resolver is not None else filename)
        selected = os.fspath(selected)
        self._bundled_core = None
        if library is None and not configured and resolver is not None and os.path.isabs(selected):
            core_path = os.fspath(resolver("fengqudb"))
            directory = lambda path: os.path.normcase(os.path.dirname(os.path.abspath(path)))
            if os.path.isabs(core_path) and directory(core_path) == directory(selected):
                # Resolve the public paired Core before loading Index's SONAME dependency.
                self._bundled_core = C.CDLL(core_path)
        self.library = C.CDLL(selected)
        self.identity_adapter = identity_adapter or public_bridge_identity
        self.max_response_bytes = max_response_bytes
        pointer, u64 = C.c_void_p, C.c_uint64
        signatures = {
            "abi_version": (C.c_uint32, []),
            "core_bridge_address": (C.c_size_t, []),
            "txn_apply": (C.c_int32, [pointer, u64, Slice]),
            "snapshot_query": (C.c_int32, [pointer, u64, Slice, C.POINTER(_Control), C.POINTER(Slice)]),
            "txn_query": (C.c_int32, [pointer, u64, Slice, C.POINTER(_Control), C.POINTER(Slice)]),
        }
        for name, (result, arguments) in signatures.items():
            function = getattr(self.library, "fqidx_" + name)
            function.restype, function.argtypes = result, arguments
        self._maintain_function = getattr(self.library, "fqidx_txn_maintain", None)
        if self._maintain_function is not None:
            self._maintain_function.restype = C.c_int32
            self._maintain_function.argtypes = [pointer, u64, Slice, C.POINTER(_Control), C.POINTER(Slice)]
        if self.library.fqidx_abi_version() != 1:
            raise IndexUnavailable("unsupported FengquIndex ABI")
        self.core_bridge_address = self.library.fqidx_core_bridge_address()
        if not self.core_bridge_address:
            raise IndexBridgeError("index library has no Core bridge address")

    @staticmethod
    def _request(data):
        if not isinstance(data, bytes) or not data or len(data) > 16 * 1024 * 1024:
            raise ValueError("index protobuf request must contain 1..16777216 bytes")
        owner = C.c_char_p(data)
        return Slice(C.cast(owner, C.c_void_p), len(data)), owner

    def _identity(self, lease, expected_store_token, *, transaction_only=False):
        identity = self.identity_adapter(lease)
        if not isinstance(identity, CoreLeaseIdentity):
            raise IndexBridgeError("invalid normalized Core lease identity")
        if identity.kind not in ("transaction", "snapshot") or (transaction_only and identity.kind != "transaction"):
            raise IndexBridgeError("index operation requires the correct Core handle kind")
        if identity.core_abi != 2 or identity.core_bridge_address != self.core_bridge_address:
            raise IndexBridgeError("index and handle must use the same Core instance and ABI")
        if type(identity.store_token) is not int or not 0 < identity.store_token < 2 ** 64:
            raise IndexBridgeError("invalid Store identity token")
        if expected_store_token is not None and identity.store_token != expected_store_token:
            raise IndexBridgeError("Store identity token mismatch")
        if not C.cast(lease.pointer, C.c_void_p).value:
            raise IndexBridgeError("Core lease returned an empty pointer")
        core = _CoreFunctions(lease.library)
        # The allocator owner comes from the lease, never from a separately loaded Core.
        if core.fqdb_abi_version() != 2:
            raise IndexBridgeError("unsupported leased Core ABI")
        return identity, core

    @staticmethod
    def _check(core, status):
        if status:
            message = core.fqdb_status_message(status)
            raise Error(status, message.decode("utf-8", errors="replace") if message else "index operation failed")

    @staticmethod
    def _borrow(handle, expected_store_token):
        borrow = getattr(handle, "borrow_native", None)
        if not callable(borrow):
            raise IndexUnavailable("Core binding does not expose public borrow_native()")
        return borrow(expected_store_token=expected_store_token)

    def apply(self, transaction, request, *, expected_store_token=None):
        try:
            data, owner = self._request(request)
            with self._borrow(transaction, expected_store_token) as lease:
                identity, core = self._identity(lease, expected_store_token, transaction_only=True)
                self._check(core, self.library.fqidx_txn_apply(lease.pointer, identity.store_token, data))
        except BaseException:
            self.cancel_failed_stage(transaction)
            raise

    @staticmethod
    def cancel_failed_stage(transaction):
        # Native apply also poisons the txn. Cover failures before C admission
        # (wire validation, wrong bridge, etc.) using only the public API.
        cancel = getattr(transaction, "cancel", None)
        if callable(cancel):
            try:
                cancel()
            except Exception:
                pass  # Keep the original failure; a closed handle cannot commit.

    def query(self, handle, request, *, expected_store_token=None, control=None):
        if self._maintain_function is None:
            for number, _wire, value in fields(request):
                if number in (23, 24, 26, 27, 28, 29, 30, 31, 32, 33, 34) or (number == 7 and value == 2) or (number == 21 and value == 1):
                    raise IndexUnavailable("this Index runtime does not expose the extended query/maintenance group")
        return self._output(handle, request, expected_store_token=expected_store_token, control=control)

    @property
    def features(self):
        return {"abi": 1, "extended_query": self._maintain_function is not None,
                "maintenance": self._maintain_function is not None}

    def _output(self, handle, request, *, expected_store_token=None, control=None, maintenance=False):
        if control is not None and not isinstance(control, QueryControl):
            raise TypeError("control must be QueryControl or None")
        data, owner = self._request(request)
        with self._borrow(handle, expected_store_token) as lease:
            identity, core = self._identity(lease, expected_store_token, transaction_only=maintenance)
            method = (self._maintain_function if maintenance else
                      self.library.fqidx_txn_query if identity.kind == "transaction" else self.library.fqidx_snapshot_query)
            output = Slice()
            try:
                self._check(core, method(lease.pointer, identity.store_token, data,
                                        C.byref(control.native) if control is not None else None, C.byref(output)))
                if output.size > self.max_response_bytes or (output.size and not output.data):
                    raise IndexBridgeError("invalid or oversized index response buffer")
                return C.string_at(output.data, output.size) if output.size else b""
            finally:
                if output.data:
                    core.fqdb_buffer_free(C.byref(output))

    def mutate(self, transaction, *, expected_store_token=None, **mutation):
        try:
            request = encode_mutation(**mutation)
        except BaseException:
            self.cancel_failed_stage(transaction)
            raise
        return self.apply(transaction, request, expected_store_token=expected_store_token)

    def execute(self, handle, *, expected_store_token=None, control=None, **query):
        query.setdefault("output_budget", self.max_response_bytes)
        return decode_result(self.query(handle, encode_query(**query),
            expected_store_token=expected_store_token, control=control), max_bytes=self.max_response_bytes)

    def maintain_raw(self, transaction, request, *, expected_store_token=None, control=None):
        try:
            if self._maintain_function is None:
                raise IndexUnavailable("this Index runtime does not expose transactional maintenance")
            return self._output(transaction, request, expected_store_token=expected_store_token,
                                control=control, maintenance=True)
        except BaseException:
            self.cancel_failed_stage(transaction)
            raise

    def maintain(self, transaction, *, expected_store_token=None, control=None, **request):
        try:
            encoded = encode_maintenance(**request)
            return decode_maintenance(self.maintain_raw(transaction, encoded,
                expected_store_token=expected_store_token, control=control))
        except BaseException:
            self.cancel_failed_stage(transaction)
            raise
