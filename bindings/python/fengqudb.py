"""Dependency-free ctypes adapter. Use bytes for keys and serialized values."""
import ctypes as C
import os
import threading
import sys
import sysconfig
import struct
from pathlib import Path

class Error(RuntimeError):
    def __init__(self, code, message):
        self.code = code
        super().__init__(f"FengquDB {code}: {message}")

class BufferTooSmall(Error):
    def __init__(self, required_size):
        self.required_size = required_size
        super().__init__(16, f"buffer needs {required_size} bytes")

class Slice(C.Structure):
    _fields_ = [("data", C.c_void_p), ("size", C.c_uint64)]

def _slice(value):
    if not isinstance(value, bytes):
        raise TypeError("keys and values must be bytes")
    owner = C.c_char_p(value)
    return Slice(C.cast(owner, C.c_void_p), len(value)), owner

def _submit_slice(value):
    # Submission copies before returning. Keep an exported mutable buffer alive
    # for that call; callers must not mutate it concurrently with submission.
    if isinstance(value, bytearray):
        owner = (C.c_ubyte * len(value)).from_buffer(value)
        return Slice(C.cast(owner, C.c_void_p), len(value)), owner
    return _slice(value)

class _Native:
    def __init__(self, library):
        self.lib = C.CDLL(str(library or os.environ.get("FENGQUDB_LIBRARY") or
                              ("fengqudb.dll" if os.name == "nt" else "libfengqudb.so")))
        signatures = {"abi_version": (C.c_uint32, []),
                      "status_message": (C.c_char_p, [C.c_int32]),
                      "buffer_free": (None, [C.POINTER(Slice)])}
        for name, (result, arguments) in signatures.items():
            function = getattr(self.lib, "fqdb_" + name)
            function.restype, function.argtypes = result, arguments
            setattr(self, name, function)
        if self.abi_version() != 2:
            raise RuntimeError("Unsupported FengquDB ABI")
    def check(self, code):
        if code:
            raise Error(code, self.status_message(code).decode("utf-8"))

# Immediate finalization is required by the argument-ownership fast path.
# Other interpreters and free-threaded builds retain an explicit call counter.
_REFERENCE_LEASES = (sys.implementation.name == "cpython" and
                     not sysconfig.get_config_var("Py_GIL_DISABLED"))

class _ConcurrentLease:
    def __init__(self):
        self.cv = threading.Condition()
        self.active = 0
        self.closing = self.closed = False
    def __enter__(self):
        with self.cv:
            if self.closing: raise RuntimeError("Handle is closed")
            self.active += 1
        return self
    def __exit__(self, *_):
        with self.cv:
            self.active -= 1
            if not self.active: self.cv.notify_all()
    def close(self, release):
        with self.cv:
            if self.closing:
                self.cv.wait_for(lambda: self.closed)
                return
            self.closing = True
            self.cv.notify_all()
            self.cv.wait_for(lambda: not self.active)
        try:
            release()
        finally:
            with self.cv:
                self.closed = True
                self.cv.notify_all()

class _PassiveLock:
    def __enter__(self):
        return self
    def __exit__(self, *_):
        pass

_PASSIVE = _PassiveLock()

class _NativeOwner:
    """ctypes retains each argument object until its native call returns.

    Detaching the public handle prevents new calls. The last argument owner
    closes it and signals close(), without a shared counter lock per read.
    """
    __slots__ = ("native", "pointer", "closer", "closed")
    def __init__(self, native, pointer, closer):
        self.closed = threading.Event()
        self.native, self.closer, self.pointer = native, closer, pointer
    @property
    def _as_parameter_(self):
        return self.pointer
    def __del__(self):
        pointer = getattr(self, "pointer", None)
        if pointer:
            try:
                getattr(self.native, self.closer)(pointer)
            finally:
                self.pointer = None
                self.closed.set()

class _Handle:
    _closer = None
    def __init__(self, native, handle):
        self._native, self._handle, self._lock = native, handle, threading.RLock()
    def _concurrent(self):
        if not _REFERENCE_LEASES:
            self._lock = _ConcurrentLease()
            return
        owner = _NativeOwner(self._native, self._handle, self._closer)
        self._handle = owner
        self._lock = _PASSIVE
        self._closed_event = owner.closed
    def _live(self):
        handle = self._handle  # Strong ownership acquired before checking close.
        if handle is None:
            raise RuntimeError("Handle is closed")
        return handle
    def close(self):
        if self._lock is _PASSIVE:
            owner = self._handle
            if owner is not None:
                event = owner.closed
                self._closed_event = event
                self._handle = None
                del owner
            else:
                event = self._closed_event
            event.wait()
        else:
            def release():
                if self._handle:
                    handle, self._handle = self._handle, None
                    getattr(self._native, self._closer)(handle)
            if isinstance(self._lock, _ConcurrentLease):
                self._lock.close(release)
            else:
                with self._lock:
                    release()
    def __enter__(self):
        self._live()
        return self
    def __exit__(self, *_):
        self.close()
    def __del__(self):
        try:
            self.close()
        except Exception:
            pass
    def _get(self, method, key):
        key_slice, owner = _slice(key)
        with self._lock:
            shared_method = ('store_get_shared' if method == 'store_get' else
                             'store_snapshot_get_shared' if method == 'store_snapshot_get' else None)
            if shared_method:
                shared = _StoreSharedValue()
                code = getattr(self._native, shared_method)(self._live(), key_slice, C.byref(shared))
                if code == 1: return None
                if code != 9:
                    self._native.check(code)
                    try:
                        row = shared.row
                        return C.string_at(row.value.data, row.value.size) if row.value.size else b''
                    finally: self._native.store_value_close(shared.owner)
            output = Slice()
            code = getattr(self._native, method)(self._live(), key_slice, C.byref(output))
            if code == 1:
                return None
            self._native.check(code)
            try:
                return C.string_at(output.data, output.size) if output.size else b""
            finally:
                self._native.buffer_free(C.byref(output))
    def _get_into(self, method, key, destination):
        """Fill a reusable bytearray; return length or None if absent. No partial copy."""
        if not isinstance(destination, bytearray):
            raise TypeError("destination must be a bytearray")
        key_slice, owner = _slice(key)
        buffer = (C.c_ubyte * len(destination)).from_buffer(destination)
        required = C.c_uint64()
        with self._lock:
            code = getattr(self._native, method)(self._live(), key_slice, buffer, len(buffer), C.byref(required))
            if code == 1:
                return None
            if code == 16:
                raise BufferTooSmall(required.value)
            self._native.check(code)
            return required.value


def _uint64(value):
    if not isinstance(value, int) or not 0 <= value <= 0xffffffffffffffff:
        raise ValueError("Expected an unsigned 64-bit integer")



class _Iterator(_Handle):
    _closer = "iterator_close"
    def __iter__(self):
        return self
    def __next__(self):
        with self._lock:
            if not self._handle:
                raise StopIteration
            row = Row()
            code = self._native.iterator_next(self._handle, C.byref(row))
            if code == 2:
                self.close()
                raise StopIteration
            self._native.check(code)
            key = C.string_at(row.key.data, row.key.size) if row.key.size else b""
            value = (C.string_at(row.value.data, row.value.size) if row.value.size else b"") if row.kind == 1 else None
            return row.version, key, value


def _take(native, code, out):
    if code == 1: return None
    native.check(code)
    try:
        return C.string_at(out.data, out.size) if out.size else b""
    finally:
        native.buffer_free(C.byref(out))





class StoreOperation:
    HASH_GET, HASH_SET, HASH_DELETE, HASH_SCAN = 1, 2, 3, 4
    SET_CONTAINS, SET_ADD, SET_REMOVE, SET_SCAN = 10, 11, 12, 13
    LIST_PUSH_LEFT, LIST_PUSH_RIGHT, LIST_POP_LEFT, LIST_POP_RIGHT, LIST_GET, LIST_SET, LIST_RANGE = range(20, 27)
    SORTED_ADD, SORTED_REMOVE, SORTED_SCORE, SORTED_RANGE = range(30, 34)
    COLLECTION_SIZE, COLLECTION_CLEAR, INDEX_LOOKUP = 40, 41, 50
    INDEX_QUERY, RECORD_PROJECT = 51, 52

def _index_query(equal=b"", lower=None, upper=None, *, lower_exclusive=False, upper_inclusive=False,
                 reverse=False, include_values=False, fields=()):
    selected=[]
    for field in fields:
        if len(selected)>=256 or not isinstance(field,int) or not 0<field<=0x1fffffff: raise ValueError("invalid projection field")
        selected.append(field)
    out=bytearray()
    for n,value in ((1,equal),(2,lower),(3,upper)):
        if value is not None:
            value=bytes(value);out.extend(bytes([n*8+2])+_feature_varint(len(value)));out.extend(value)
    flags=int(bool(lower_exclusive))|int(bool(upper_inclusive))*2|int(bool(reverse))*4|int(bool(include_values))*8
    out.extend(b"\x20"+_feature_varint(flags))
    for field in selected:out.extend(b"\x28"+_feature_varint(field))
    return bytes(out)

class StoreIndexField(C.Structure):
    _fields_=[("field",C.c_uint32),("type",C.c_uint32)]

class StoreHistoryInfo(C.Structure):
    _fields_=[(name,C.c_uint64) for name in ('version','floor','oldest_pin','active_pins','total_pins','retained_generations','retained_file_bytes')]

class _StoreWatchOptions(C.Structure):
    _fields_=[('size',C.c_uint32),('abi',C.c_uint32),('flags',C.c_uint32),('scans',C.c_uint32),('bytes',C.c_uint64)]

class _StoreChangeRow(C.Structure):
    _fields_=[('key',Slice),('value',Slice),('expires_at_ms',C.c_uint64),('kind',C.c_uint32),('reserved',C.c_uint32)]

class _StoreChanges(C.Structure):
    _fields_=[('version',C.c_uint64),('unix_ms',C.c_uint64),('durable_version',C.c_uint64),('count',C.c_uint32),('flags',C.c_uint32),('rows',C.POINTER(_StoreChangeRow))]

class StoreChange:
    __slots__=('key','value','kind','expires_at_ms')
    def __init__(self,key,value,kind,expires_at_ms):
        self.key,self.value,self.kind,self.expires_at_ms=key,value,kind,expires_at_ms

class StoreChanges:
    __slots__=('version','unix_ms','durable_version','flags','rows')
    def __init__(self,data):
        self.version,self.unix_ms,self.durable_version,self.flags=data.version,data.unix_ms,data.durable_version,data.flags
        self.rows=[]
        for i in range(data.count):
            row=data.rows[i]
            key=C.string_at(row.key.data,row.key.size) if row.key.size else b''
            value=None if row.kind==2 or data.flags&1 else (C.string_at(row.value.data,row.value.size) if row.value.size else b'')
            self.rows.append(StoreChange(key,value,row.kind,row.expires_at_ms))

class StoreRuntimeOptions(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in ("size", "abi", "background", "interval_ms", "expiry_keys",
        "group_max", "group_delay_us", "garbage_percent")] + [(name, C.c_uint64) for name in
        ("compact_min_bytes", "compact_growth_bytes", "job_timeout_ms", "job_max_read_bytes", "job_max_write_bytes")]
class StoreRuntimeInfo(C.Structure):
    _fields_ = [(name, C.c_uint64) for name in ("running", "last_status", "expiry_batches", "compactions",
        "group_flushes", "group_transactions")]
class StoreCommandResult:
    def __init__(self):
        self.version, self.count, self.changed, self.value, self.rows = 0, 0, False, None, []
        self.cursor = b""

def _feature_varint(value):
    value &= (1 << 64) - 1
    output = bytearray()
    while value >= 128: output.append((value & 127) | 128); value >>= 7
    output.append(value)
    return bytes(output)

def _feature_request(operation, scope, key=b"", value=b"", *, argument=0, score=0, limit=256, flags=0):
    if not isinstance(argument, int) or not -(1 << 63) <= argument < (1 << 63): raise ValueError("argument out of range")
    _uint64(operation); _uint64(limit); _uint64(flags)
    output = b"\x08" + _feature_varint(operation)
    for number, text in ((2, scope), (3, key), (4, value)):
        text = bytes(text); output += bytes([number * 8 + 2]) + _feature_varint(len(text)) + text
    return output + b"\x29" + struct.pack("<q", argument) + b"\x31" + struct.pack("<d", score) + b"\x39" + struct.pack("<Q", limit) + b"\x40" + _feature_varint(flags)

def _feature_fields(data):
    position = 0
    def varint():
        nonlocal position
        result = 0
        for shift in range(0, 70, 7):
            if position >= len(data): raise ValueError("truncated feature response")
            value = data[position]; position += 1; result |= (value & 127) << shift
            if not value & 128: return result
        raise ValueError("invalid feature response")
    while position < len(data):
        tag = varint(); kind = tag & 7
        if kind == 0: value = varint()
        elif kind in (1, 5):
            length = 8 if kind == 1 else 4
            if position + length > len(data): raise ValueError("truncated feature response")
            value = int.from_bytes(data[position:position+length], "little"); position += length
        elif kind == 2:
            length = varint()
            if position + length > len(data): raise ValueError("truncated feature response")
            value = data[position:position+length]; position += length
        else: raise ValueError("invalid feature response")
        yield tag >> 3, value

def _feature_result(data):
    result = StoreCommandResult()
    for number, value in _feature_fields(data):
        if number == 1: result.value = value
        elif number == 2: result.count = value
        elif number == 3: result.changed = bool(value)
        elif number == 5: result.cursor = value
        elif number == 4:
            row = dict(_feature_fields(value))
            result.rows.append((row.get(1,b""), row.get(2,b""), struct.unpack("<d", struct.pack("<Q",row.get(3,0)))[0]))
    return result

class StoreOptions(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in ("struct_size", "abi_version", "durability", "flags")] + [
        (name, C.c_uint64) for name in ("cache_bytes", "delta_bytes", "writer_bytes", "transaction_bytes",
                                      "all_transaction_bytes", "pinned_bytes", "max_snapshots")]

class StoreInfo(C.Structure):
    _fields_ = [(name, C.c_uint64) for name in ("version", "durable_version", "file_bytes", "indexed_keys",
        "stored_keys", "stored_value_bytes", "delta_bytes", "cache_bytes", "pinned_bytes",
        "transaction_bytes", "recovered_transactions", "repaired_bytes")]

class StoreExpiryOptions(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in ("struct_size", "abi_version", "max_keys", "reserved")] + [
        ("timeout_ms", C.c_uint64), ("max_batch_bytes", C.c_uint64)]

class StoreExpiryInfo(C.Structure):
    _fields_ = [(name, C.c_uint64) for name in ("version", "cutoff_ms", "removed_keys",
        "removed_value_bytes", "visited_pages", "more")]

class StoreMaintenanceIO(C.Structure):
    _fields_=[('read_bytes',C.c_uint64),('write_bytes',C.c_uint64),('sync_calls',C.c_uint64)]

class StoreMaintenanceOptions(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in ("struct_size", "abi_version", "max_catchup_rounds", "reserved")] + [
        ("timeout_ms", C.c_uint64), ("max_barrier_bytes", C.c_uint64),
        ('max_read_bytes',C.c_uint64),('max_write_bytes',C.c_uint64),('io_stats',C.POINTER(StoreMaintenanceIO))]

class StoreMaintenanceInfo(C.Structure):
    _fields_ = [(name, C.c_uint64) for name in ("snapshot_version", "final_version", "source_file_bytes",
        "output_file_bytes", "stored_keys", "stored_value_bytes", "copied_keys", "catchup_rounds")]

def _store_maintenance_options(timeout_ms=0, max_barrier_bytes=1048576, max_catchup_rounds=8,
                              max_read_bytes=0,max_write_bytes=0,io_stats=None):
    _uint64(timeout_ms)
    _uint64(max_barrier_bytes)
    _uint64(max_read_bytes);_uint64(max_write_bytes)
    if io_stats is not None and not isinstance(io_stats,StoreMaintenanceIO):raise TypeError('Expected StoreMaintenanceIO')
    if not isinstance(max_catchup_rounds, int) or not 0 <= max_catchup_rounds <= 64:
        raise ValueError("max_catchup_rounds must be between 0 and 64")
    return StoreMaintenanceOptions(C.sizeof(StoreMaintenanceOptions), 2 if max_read_bytes or max_write_bytes or io_stats is not None else 1,
        max_catchup_rounds,0,timeout_ms,max_barrier_bytes,max_read_bytes,max_write_bytes,C.pointer(io_stats) if io_stats is not None else None)

def _store_path(path):
    encoded = os.fspath(Path(path)).encode("utf-8")
    if b"\0" in encoded: raise ValueError("Path contains NUL")
    return encoded

class _StoreRow(C.Structure):
    _fields_ = [("key", Slice), ("value", Slice), ("version", C.c_uint64), ("expires_at_ms", C.c_uint64)]

class StoreAtomicInfo(C.Structure):
    _fields_ = [("version", C.c_uint64), ("observed_version", C.c_uint64), ("value", C.c_int64),
                ("matched", C.c_uint32), ("changed", C.c_uint32)]

class _StorePut(C.Structure):
    _fields_ = [("key", Slice), ("value", Slice), ("expires_at_ms", C.c_uint64)]

class _StoreI64FieldUpdate(C.Structure):
    _fields_ = [("key", Slice), ("offset", C.c_uint64)] + [
        (name, C.c_int64) for name in ("delta", "minimum", "maximum")]

class _StoreResultRow(C.Structure):
    _fields_ = [("value", Slice), ("version", C.c_uint64), ("expires_at_ms", C.c_uint64),
                ("found", C.c_uint32), ("reserved", C.c_uint32)]

class StoreEntry:
    __slots__ = ("value", "version", "expires_at_ms")
    def __init__(self, value, version, expires_at_ms):
        self.value, self.version, self.expires_at_ms = value, version, expires_at_ms

def _store_keys(keys):
    values, owners = [], []
    for item in keys:
        if len(values) == 4096: raise ValueError("too many keys")
        value, owner = _slice(item)
        values.append(value); owners.append(owner)
    return (Slice * len(values))(*values), owners

def _store_items(items):
    values, owners = [], []
    for item in items:
        if len(values) == 4096: raise ValueError("too many items")
        if len(item) not in (2,3): raise ValueError("expected (key,value[,expires_at_ms])")
        key, a = _slice(item[0]); value, b = _slice(item[1])
        expiry = item[2] if len(item) == 3 else 0
        _uint64(expiry)
        owners.extend((a,b)); values.append(_StorePut(key,value,expiry))
    return (_StorePut * len(values))(*values), owners

def _store_get_many(owner, method, keys, metadata=False):
    keys, keep = _store_keys(keys)
    with owner._lock:
        result = C.c_void_p()
        owner._native.check(getattr(owner._native,method)(owner._live(),keys,len(keys),C.byref(result)))
        try:
            rows = owner._native.store_results_data(result)
            values=[]
            for i in range(len(keys)):
                row=rows[i]
                if not row.found: values.append(None); continue
                value=C.string_at(row.value.data,row.value.size) if row.value.size else b""
                values.append(StoreEntry(value,row.version,row.expires_at_ms) if metadata else value)
            return values
        finally: owner._native.store_results_close(result)

def _store_entry(owner, key=None):
    with owner._lock:
        result = _StoreSharedValue()
        if key is None:
            status = owner._native.store_key_get_shared(owner._live(), C.byref(result))
        else:
            key, keep = _slice(key)
            status = owner._native.store_get_shared(owner._live(), key, C.byref(result))
        if status == 1: return None
        owner._native.check(status)
        try:
            row = result.row
            return StoreEntry(C.string_at(row.value.data, row.value.size) if row.value.size else b"",
                              row.version, row.expires_at_ms)
        finally: owner._native.store_value_close(result.owner)

class _StoreSharedValue(C.Structure):
    _fields_ = [("owner", C.c_void_p), ("row", _StoreResultRow)]

class _SharedLease:
    # Exporters retain this owner even after the public wrapper is closed.
    __slots__ = ('native', 'handle', 'closer')
    def __init__(self, native, handle, closer):
        self.native, self.closer = native, closer
        self.handle = handle
    def __del__(self):
        handle = getattr(self, 'handle', None)
        if handle:
            self.handle = None
            getattr(self.native, self.closer)(handle)

def _adopt_shared(native, handle, closer):
    try: return _SharedLease(native, handle, closer)
    except BaseException:
        getattr(native, closer)(handle)
        raise

class StoreSharedValue:
    """Immutable zero-copy bytes. Exported/derived memoryviews retain the lease.

    close() drops this wrapper's reference; existing views remain valid and
    keep their pinned-byte budget until all references are released.
    """
    def __init__(self, row, lease):
        self.version, self.expires_at_ms = row.version, row.expires_at_ms
        exporter = ((C.c_ubyte * row.value.size).from_address(row.value.data)
                    if row.value.size else (C.c_ubyte * 0)())
        exporter._lease = lease
        self._value = memoryview(exporter).cast('B').toreadonly()
    @property
    def value(self):
        value = self._value
        if value is None: raise RuntimeError('Shared value is closed')
        return value
    def close(self): self._value = None
    def __enter__(self): return self
    def __exit__(self, *_): self.close()

class StoreSharedBatch:
    """One consistent view/TTL time. Any exported row retains the whole batch."""
    def __init__(self, lease, count):
        rows = lease.native.store_results_data(lease.handle)
        self._values = tuple(StoreSharedValue(rows[i], lease) if rows[i].found else None
                             for i in range(count))
    def __len__(self): return len(self._values)
    def __getitem__(self, index): return self._values[index]
    def close(self): self._values = ()
    def __enter__(self): return self
    def __exit__(self, *_): self.close()

class StoreReader(_Handle):
    _closer = 'store_reader_close'
    def __init__(self, native, handle, count):
        super().__init__(native, handle)
        self._count = count
    def read_shared(self):
        with self._lock:
            out = C.c_void_p()
            self._native.check(self._native.store_reader_acquire(self._live(), C.byref(out)))
            return StoreSharedBatch(_adopt_shared(self._native, out, 'store_results_close'), self._count)
    def release(self):
        with self._lock: self._native.store_reader_release(self._live())

def _store_shared(owner, key=None):
    out = _StoreSharedValue()
    with owner._lock:
        if key is None:
            status = owner._native.store_key_get_shared(owner._live(), C.byref(out))
        else:
            key, keep = _slice(key)
            status = owner._native.store_get_shared(owner._live(), key, C.byref(out))
        if status == 1: return None
        owner._native.check(status)
    lease = _adopt_shared(owner._native, out.owner, 'store_value_close')
    return StoreSharedValue(out.row, lease)

from enum import IntEnum
class StoreWriteMode(IntEnum):
    RELIABLE = 0
    PERFORMANCE = 1
def _write_flag(value):
    if not isinstance(value,bool): raise TypeError("force_reliable must be bool")
    return int(value)
class StoreWriteState(C.Structure):
    _fields_=[("version",C.c_uint64),("durable_version",C.c_uint64),("pending_bytes",C.c_uint64),("last_status",C.c_uint64)]
class StoreWriteOptions(C.Structure):
    _fields_ = [("struct_size",C.c_uint32),("abi_version",C.c_uint32),
                ("mode",C.c_uint32),("period_ms",C.c_uint32),("threshold_bytes",C.c_uint64)]
    def __init__(self,mode=StoreWriteMode.PERFORMANCE,period_ms=50,threshold_bytes=1048576):
        _uint64(period_ms); _uint64(threshold_bytes)
        if period_ms>0xffffffff: raise ValueError("period_ms exceeds uint32")
        if isinstance(mode,str): mode=StoreWriteMode[mode.upper()]
        mode=StoreWriteMode(mode)
        super().__init__(C.sizeof(type(self)),1,int(mode),period_ms,threshold_bytes)

class StoreCommit(_Handle):
    """Deferred result for the configured guarantee. Close waits; use poll/wait_async before closing on a UI thread."""
    _closer = "store_commit_close"
    def __init__(self,native,handle):
        super().__init__(native,handle)
        self._concurrent()
    def poll(self):
        result,version=C.c_int32(),C.c_uint64()
        with self._lock:
            status=self._native.store_commit_poll(self._live(),C.byref(result),C.byref(version))
        if status==18: return None
        self._native.check(status); self._native.check(result.value)
        return version.value
    def wait(self,timeout_ms=0):
        _uint64(timeout_ms); result,version=C.c_int32(),C.c_uint64()
        with self._lock:
            self._native.check(self._native.store_commit_wait(self._live(),timeout_ms,C.byref(result),C.byref(version)))
        self._native.check(result.value)
        return version.value
    async def wait_async(self,poll_interval=.005):
        import asyncio
        if poll_interval<=0: raise ValueError("poll_interval must be positive")
        while True:
            result=self.poll()
            if result is not None: return result
            await asyncio.sleep(poll_interval)
    def cancel(self):
        with self._lock: self._native.store_commit_cancel(self._live())

class _StoreNative(_Native):
    def __init__(self, library):
        super().__init__(library)
        pointer, u64, status = C.c_void_p, C.c_uint64, C.c_int32
        signatures = {
            "write_configure": [pointer,C.POINTER(StoreWriteOptions)],
            "write_options_get": [pointer,C.POINTER(StoreWriteOptions)],
            "write_state_get": [pointer,C.POINTER(StoreWriteState)],
            "put": [pointer,Slice,Slice,u64,C.POINTER(StoreAtomicInfo),C.c_uint32],
            "delete": [pointer,Slice,C.POINTER(StoreAtomicInfo),C.c_uint32],
            "patch": [pointer,Slice,u64,u64,Slice,C.POINTER(StoreAtomicInfo),C.c_uint32],
            "put_submit": [pointer,Slice,Slice,u64,C.POINTER(pointer),C.c_uint32],
            "txn_submit": [pointer,C.POINTER(pointer),C.c_uint32],
            "commit_wait": [pointer,u64,C.POINTER(status),C.POINTER(u64)],
            "commit_poll": [pointer,C.POINTER(status),C.POINTER(u64)],
            "commit_close": [pointer], "commit_cancel": [pointer],
            "open": [C.c_char_p, C.POINTER(StoreOptions), C.POINTER(pointer)],
            "get_info": [pointer, C.POINTER(StoreInfo)], "sync": [pointer], "checkpoint": [pointer],
            "get": [pointer, Slice, C.POINTER(Slice)],
            "snapshot_create": [pointer, C.POINTER(pointer)], "snapshot_get": [pointer, Slice, C.POINTER(Slice)],
            "txn_begin": [pointer, u64, C.POINTER(pointer)], "txn_get": [pointer, Slice, C.POINTER(Slice)],
            "txn_put": [pointer, Slice, Slice, u64], "txn_delete": [pointer, Slice],
            "txn_expire": [pointer, Slice, u64], "txn_patch": [pointer, Slice, u64, u64, Slice],
            "txn_savepoint": [pointer, C.POINTER(u64)], "txn_rollback_to": [pointer, u64],
            "txn_release": [pointer, u64], "txn_commit": [pointer, C.POINTER(u64),C.c_uint32],
            "cursor_open": [pointer, Slice, Slice, C.c_uint32, C.POINTER(pointer)],
            "txn_cursor_open": [pointer, Slice, Slice, C.c_uint32, C.POINTER(pointer)],
            "cursor_next": [pointer, C.POINTER(_StoreRow)],
        }
        for name in ("close", "snapshot_close", "txn_close", "txn_cancel", "cursor_close"):
            signatures[name] = [pointer]
        signatures["purge_expired"] = [pointer, C.POINTER(StoreExpiryOptions), C.POINTER(StoreExpiryInfo)]
        signatures.update({"hash_get_view": [pointer,Slice,Slice,C.POINTER(pointer)],
            "value_data": [pointer], "value_close": [pointer], "txn_execute": [pointer, Slice, C.POINTER(Slice)],
            "select": [pointer, Slice, C.POINTER(Slice)],
            "project": [pointer,Slice,C.POINTER(C.c_uint32),C.c_uint32,C.POINTER(Slice)],
            "project_reader_create": [pointer,C.POINTER(Slice),C.c_uint32,C.POINTER(C.c_uint32),C.c_uint32,C.POINTER(pointer)],
            "configure": [pointer, C.POINTER(StoreRuntimeOptions)],
            "runtime_info_get": [pointer, C.POINTER(StoreRuntimeInfo)],
            "history_hold": [pointer,Slice,C.c_uint64,C.c_uint64,C.POINTER(StoreAtomicInfo)],
            "history_release": [pointer,Slice,C.c_uint64,C.POINTER(StoreAtomicInfo)],
            "history_info_get": [pointer,C.POINTER(StoreHistoryInfo)],
            "watch_open": [pointer,u64,Slice,C.POINTER(_StoreWatchOptions),C.POINTER(pointer)],
            "watch_next": [pointer,u64,C.POINTER(pointer)],
            "watch_ack": [pointer,u64], "watch_cancel": [pointer], "watch_close": [pointer],
            "change_batch_data": [pointer], "change_batch_close": [pointer],
            "index_define": [pointer, Slice, Slice, C.c_uint32, C.c_uint32, C.c_uint32, C.POINTER(StoreMaintenanceOptions), C.POINTER(StoreMaintenanceInfo)],
            "index_define_compound": [pointer,Slice,Slice,C.POINTER(StoreIndexField),C.c_uint32,C.c_uint32,C.POINTER(StoreMaintenanceOptions),C.POINTER(StoreMaintenanceInfo)],
            "index_drop": [pointer, Slice, C.POINTER(StoreMaintenanceOptions), C.POINTER(StoreMaintenanceInfo)],
            "backup_incremental": [pointer, C.c_char_p, u64, C.POINTER(StoreMaintenanceOptions), C.POINTER(StoreMaintenanceInfo)],
            "recover_chain": [C.c_char_p, C.POINTER(C.c_char_p), C.c_uint32, C.c_char_p, u64, u64, C.POINTER(StoreMaintenanceOptions), C.POINTER(StoreMaintenanceInfo)]})
        maintenance = C.POINTER(StoreMaintenanceOptions)
        result_info = C.POINTER(StoreMaintenanceInfo)
        signatures.update({"compact": [pointer, maintenance, result_info],
            "backup": [pointer, C.c_char_p, maintenance, result_info],
            "verify": [pointer, maintenance, result_info],
            "restore": [C.c_char_p, C.c_char_p, maintenance, result_info], "cancel_maintenance": [pointer]})
        atomic = C.POINTER(StoreAtomicInfo)
        signatures.update({
            "path_at": [C.c_char_p,C.c_char_p,C.POINTER(Slice)],
            "open_at": [C.c_char_p,C.c_char_p,C.POINTER(StoreOptions),C.POINTER(pointer)],
            "get_view": [pointer,Slice,C.POINTER(pointer)],
            "value_version": [pointer], "value_expires_at_ms": [pointer],
            "compare_exchange": [pointer,Slice,u64,Slice,u64,u64,atomic,C.c_uint32],
            "delete_if_version": [pointer,Slice,u64,u64,atomic,C.c_uint32],
            "put_if_changed": [pointer,Slice,Slice,u64,u64,atomic,C.c_uint32],
            "add_i64": [pointer,Slice,C.c_int64,C.c_int64,C.c_int64,u64,atomic,C.c_uint32],
            "add_i64_fields": [pointer,C.POINTER(_StoreI64FieldUpdate),C.c_uint32,u64,atomic,C.c_uint32],
            "get_many": [pointer,C.POINTER(Slice),C.c_uint32,C.POINTER(pointer)],
            "snapshot_get_many": [pointer,C.POINTER(Slice),C.c_uint32,C.POINTER(pointer)],
            "txn_get_many": [pointer,C.POINTER(Slice),C.c_uint32,C.POINTER(pointer)],
            "results_data": [pointer], "results_close": [pointer],
            "txn_put_many": [pointer,C.POINTER(_StorePut),C.c_uint32],
            "txn_delete_many": [pointer,C.POINTER(Slice),C.c_uint32],
            "batch_create": [pointer,C.POINTER(_StorePut),C.c_uint32,C.POINTER(pointer)],
            "batch_close": [pointer], "txn_put_batch": [pointer,pointer],
            "key_prepare": [pointer,Slice,C.POINTER(pointer)], "key_close": [pointer],
            "key_get_view": [pointer,C.POINTER(pointer)],
            "key_get_into": [pointer,pointer,u64,C.POINTER(u64)]})
        signatures.update({
            "get_shared": [pointer, Slice, C.POINTER(_StoreSharedValue)],
            "snapshot_get_shared": [pointer, Slice, C.POINTER(_StoreSharedValue)],
            "key_get_shared": [pointer, C.POINTER(_StoreSharedValue)],
            "reader_create": [pointer, C.POINTER(Slice), C.c_uint32, C.POINTER(pointer)],
            "reader_acquire": [pointer, C.POINTER(pointer)],
            "reader_release": [pointer], "reader_close": [pointer]})
        for name, arguments in signatures.items():
            function = getattr(self.lib, "fqdb_store_" + name)
            function.restype = (None if name.endswith("close") or name in ("txn_cancel", "commit_cancel", "cancel_maintenance", "watch_cancel", "reader_release") else
                Slice if name == "value_data" else u64 if name in ("value_version","value_expires_at_ms") else
                C.POINTER(_StoreChanges) if name == "change_batch_data" else
                C.POINTER(_StoreResultRow) if name == "results_data" else status)
            function.argtypes = arguments
            setattr(self, "store_" + name, function)

def _store_scan(owner, method, begin, end, reverse):
    low, low_owner = _slice(begin)
    high, high_owner = _slice(end)
    with owner._lock:
        output = C.c_void_p()
        owner._native.check(getattr(owner._native, method)(owner._live(), low, high, int(bool(reverse)), C.byref(output)))
        return StoreCursor(owner._native, output)

class Store(_Handle):
    def write_state(self):
        out=StoreWriteState()
        with self._lock: self._native.check(self._native.store_write_state_get(self._live(),C.byref(out)))
        return out
    def put(self,key,value,*,expires_at_ms=0,force_reliable=False):
        _uint64(expires_at_ms);k,a=_slice(key);v,b=_slice(value);out=StoreAtomicInfo()
        with self._lock: self._native.check(self._native.store_put(self._live(),k,v,expires_at_ms,C.byref(out),_write_flag(force_reliable)))
        return out
    def delete(self,key,*,force_reliable=False):
        k,a=_slice(key);out=StoreAtomicInfo()
        with self._lock: self._native.check(self._native.store_delete(self._live(),k,C.byref(out),_write_flag(force_reliable)))
        return out
    def patch(self,key,offset,remove,insert,*,force_reliable=False):
        _uint64(offset);_uint64(remove);k,a=_slice(key);v,b=_slice(insert);out=StoreAtomicInfo()
        with self._lock: self._native.check(self._native.store_patch(self._live(),k,offset,remove,v,C.byref(out),_write_flag(force_reliable)))
        return out
    def write_options(self):
        out=StoreWriteOptions()
        with self._lock: self._native.check(self._native.store_write_options_get(self._live(),C.byref(out)))
        return out
    def configure_write(self,*,mode=StoreWriteMode.PERFORMANCE,period_ms=50,threshold_bytes=1048576):
        settings=StoreWriteOptions(mode,period_ms,threshold_bytes)
        with self._lock: self._native.check(self._native.store_write_configure(self._live(),C.byref(settings)))
    def reset_write_options(self):
        with self._lock: self._native.check(self._native.store_write_configure(self._live(),None))
    async def configure_write_async(self,**options):
        import asyncio,functools
        await asyncio.get_running_loop().run_in_executor(None,functools.partial(self.configure_write,**options))
    async def reset_write_options_async(self):
        import asyncio
        await asyncio.get_running_loop().run_in_executor(None,self.reset_write_options)
    def submit_put(self,key,value,*,expires_at_ms=0,force_reliable=False):
        _uint64(expires_at_ms)
        key,keep_key=_submit_slice(key); value,keep_value=_submit_slice(value); out=C.c_void_p()
        with self._lock: self._native.check(self._native.store_put_submit(self._live(),key,value,expires_at_ms,C.byref(out),_write_flag(force_reliable)))
        try: return StoreCommit(self._native,out)
        except BaseException:
            self._native.store_commit_close(out); raise
    def get_shared(self, key): return _store_shared(self, key)
    def prepare_reader(self, keys):
        if isinstance(keys, bytes): keys = [keys]
        keys, keep = _store_keys(keys)
        with self._lock:
            out = C.c_void_p()
            self._native.check(self._native.store_reader_create(self._live(), keys, len(keys), C.byref(out)))
            return StoreReader(self._native, out, len(keys))
    def get_many_shared(self, keys):
        keys, keep = _store_keys(keys)
        with self._lock:
            out = C.c_void_p()
            self._native.check(self._native.store_get_many(self._live(), keys, len(keys), C.byref(out)))
            return StoreSharedBatch(_adopt_shared(self._native, out, 'store_results_close'), len(keys))
    def prepare_project_reader(self, keys, fields):
        if isinstance(keys,bytes): keys=[keys]
        keys,keep=_store_keys(keys); fields=list(fields)
        if len(fields)>256 or any(not isinstance(f,int) or not 1<=f<=0x1fffffff for f in fields):
            raise ValueError('invalid projection fields')
        selected=(C.c_uint32*len(fields))(*fields);out=C.c_void_p()
        with self._lock:
            self._native.check(self._native.store_project_reader_create(self._live(),keys,len(keys),selected,len(fields),C.byref(out)))
            try:return StoreReader(self._native,out,len(keys))
            except BaseException:self._native.store_reader_close(out);raise
    def watch(self,from_version=None,prefix=b'',*,metadata_only=False,include_system=False,max_scan_commits=256,max_batch_bytes=67108864):
        if from_version is None:from_version=(1<<64)-1
        _uint64(from_version);_uint64(max_batch_bytes)
        if not isinstance(max_scan_commits,int) or not 1<=max_scan_commits<=4096:raise ValueError('Invalid scan limit')
        settings=_StoreWatchOptions(C.sizeof(_StoreWatchOptions),1,int(bool(metadata_only))|int(bool(include_system))*2,max_scan_commits,max_batch_bytes)
        prefix,keep=_slice(prefix);out=C.c_void_p()
        with self._lock:self._native.check(self._native.store_watch_open(self._live(),from_version,prefix,C.byref(settings),C.byref(out)))
        try:return StoreWatch(self._native,out)
        except BaseException:
            self._native.store_watch_close(out)
            raise
    def query_index(self,name,*,cursor=b"",limit=256,**query):
        return self.execute(StoreOperation.INDEX_QUERY,name,cursor,_index_query(**query),limit=limit)
    def project(self,key,fields):
        fields=list(fields)
        if len(fields)>256 or any(not isinstance(f,int) or not 1<=f<=0x1fffffff for f in fields):
            raise ValueError('invalid projection fields')
        selected=(C.c_uint32*len(fields))(*fields);key,keep=_slice(key);out=Slice()
        with self._lock:
            self._native.check(self._native.store_project(self._live(),key,selected,len(fields),C.byref(out)))
            try:return C.string_at(out.data,out.size) if out.size else b''
            finally:self._native.buffer_free(C.byref(out))
    """Paged database directory. Values are bytes; TTL uses absolute Unix milliseconds.

    Returned bytes/rows are Python-owned copies. Native caches remain shared.
    Closing a Store leaves child handles usable until their own close().
    """
    _closer = "store_close"
    def __init__(self, path, *, name=None, library=None, buffered=False, create=True, segmented=False, cache_bytes=64*1024*1024,
                 delta_bytes=4*1024*1024, writer_bytes=32*1024*1024, transaction_bytes=64*1024*1024,
                 all_transaction_bytes=256*1024*1024, pinned_bytes=128*1024*1024, max_snapshots=1024):
        native = _StoreNative(library)
        encoded = os.fspath(Path(path)).encode("utf-8")
        if b"\0" in encoded: raise ValueError("Path contains NUL")
        limits = (cache_bytes, delta_bytes, writer_bytes, transaction_bytes, all_transaction_bytes, pinned_bytes, max_snapshots)
        for value in limits: _uint64(value)
        options = StoreOptions(C.sizeof(StoreOptions), 1, int(bool(buffered)), int(bool(create)) | (2 if segmented else 0), *limits)
        output = C.c_void_p()
        if name is None:
            native.check(native.store_open(encoded, C.byref(options), C.byref(output)))
        else:
            if not isinstance(name,str) or not name or '\0' in name: raise ValueError("invalid database name")
            native.check(native.store_open_at(encoded,name.encode('utf-8'),C.byref(options),C.byref(output)))
        super().__init__(native, output)
        self._concurrent()
    @classmethod
    def open_at(cls, root, name, **options): return cls(root,name=name,**options)
    @staticmethod
    def path_at(root, name, *, library=None):
        if not isinstance(name,str) or not name or '\0' in name: raise ValueError("invalid database name")
        native = _StoreNative(library); result = Slice()
        native.check(native.store_path_at(_store_path(root),name.encode('utf-8'),C.byref(result)))
        try: return C.string_at(result.data,result.size).decode('utf-8')
        finally: native.buffer_free(C.byref(result))
    def get_entry(self, key): return _store_entry(self,key)
    def get_many(self, keys): return _store_get_many(self,"store_get_many",keys)
    def get_entries(self, keys): return _store_get_many(self,"store_get_many",keys,True)
    def prepare_key(self, key):
        key, keep = _slice(key); result = C.c_void_p()
        with self._lock: self._native.check(self._native.store_key_prepare(self._live(),key,C.byref(result)))
        return StorePreparedKey(self._native,result)
    def prepare_batch(self, items):
        items, keep = _store_items(items); result = C.c_void_p()
        with self._lock: self._native.check(self._native.store_batch_create(self._live(),items,len(items),C.byref(result)))
        return StoreBatch(self._native,result)
    def compare_exchange(self, key, expected_version, value, expires_at_ms=0, timeout_ms=0,*,force_reliable=False):
        _uint64(expected_version); _uint64(expires_at_ms); _uint64(timeout_ms)
        key,a = _slice(key); value,b = _slice(value); result = StoreAtomicInfo()
        with self._lock: self._native.check(self._native.store_compare_exchange(self._live(),key,expected_version,value,expires_at_ms,timeout_ms,C.byref(result),_write_flag(force_reliable)))
        return result
    def put_if_absent(self, key, value, expires_at_ms=0, timeout_ms=0,*,force_reliable=False):
        return self.compare_exchange(key,0,value,expires_at_ms,timeout_ms,force_reliable=force_reliable)
    def delete_if_version(self, key, expected_version, timeout_ms=0,*,force_reliable=False):
        _uint64(expected_version); _uint64(timeout_ms); key,keep = _slice(key); result = StoreAtomicInfo()
        with self._lock: self._native.check(self._native.store_delete_if_version(self._live(),key,expected_version,timeout_ms,C.byref(result),_write_flag(force_reliable)))
        return result
    def put_if_changed(self, key, value, expires_at_ms=0, timeout_ms=0,*,force_reliable=False):
        _uint64(expires_at_ms); _uint64(timeout_ms); key,a = _slice(key); value,b = _slice(value); result = StoreAtomicInfo()
        with self._lock: self._native.check(self._native.store_put_if_changed(self._live(),key,value,expires_at_ms,timeout_ms,C.byref(result),_write_flag(force_reliable)))
        return result
    def add_i64(self, key, delta, minimum=-(1<<63), maximum=(1<<63)-1, timeout_ms=0,*,force_reliable=False):
        for v in (delta,minimum,maximum):
            if not isinstance(v,int) or not -(1<<63) <= v < (1<<63): raise ValueError("signed integer out of range")
        _uint64(timeout_ms); key,keep = _slice(key); result = StoreAtomicInfo()
        with self._lock: self._native.check(self._native.store_add_i64(self._live(),key,delta,minimum,maximum,timeout_ms,C.byref(result),_write_flag(force_reliable)))
        return result
    def add_i64_fields(self, items, timeout_ms=0, *, force_reliable=False):
        """Atomically add to existing LE int64 fields: (key,offset,delta[,min,max])."""
        items = list(items)
        if not 1 <= len(items) <= 256: raise ValueError("expected 1..256 field updates")
        _uint64(timeout_ms)
        native = (_StoreI64FieldUpdate * len(items))(); keep = []
        for i, item in enumerate(items):
            if len(item) == 3: key, offset, delta = item; minimum, maximum = -(1<<63), (1<<63)-1
            elif len(item) == 5: key, offset, delta, minimum, maximum = item
            else: raise ValueError("expected (key,offset,delta[,min,max])")
            _uint64(offset)
            for value in (delta, minimum, maximum):
                if not isinstance(value,int) or not -(1<<63) <= value < (1<<63):
                    raise ValueError("signed integer out of range")
            if minimum > maximum: raise ValueError("invalid field bounds")
            key, owner = _slice(key); keep.append(owner)
            native[i] = _StoreI64FieldUpdate(key, offset, delta, minimum, maximum)
        result = StoreAtomicInfo()
        with self._lock:
            self._native.check(self._native.store_add_i64_fields(
                self._live(),native,len(native),timeout_ms,C.byref(result),_write_flag(force_reliable)))
        return result
    def put_many(self, items,*,force_reliable=False):
        with self.transaction() as tx: tx.put_many(items); return tx.commit(force_reliable)
    def delete_many(self, keys,*,force_reliable=False):
        with self.transaction() as tx: tx.delete_many(keys); return tx.commit(force_reliable)
    def put_batch(self, batch,*,force_reliable=False):
        with self.transaction() as tx: tx.put_batch(batch); return tx.commit(force_reliable)
    def hash_get(self, scope, key):
        scope, a = _slice(scope); key, b = _slice(key)
        with self._lock:
            output = C.c_void_p()
            status = self._native.store_hash_get_view(self._live(),scope,key,C.byref(output))
            if status == 1: return None
            self._native.check(status)
            try:
                value = self._native.store_value_data(output)
                return C.string_at(value.data,value.size) if value.size else b""
            finally: self._native.store_value_close(output)
    def select_raw(self, operation, value):
        """Read-only SELECT/EXPLAIN on a fresh snapshot; no transaction or dependencies."""
        request, owner = _slice(_feature_request(operation, b"", b"", value))
        with self._lock:
            output = Slice()
            try:
                self._native.check(self._native.store_select(self._live(), request, C.byref(output)))
                data = C.string_at(output.data, output.size) if output.size else b""
            finally: self._native.buffer_free(C.byref(output))
        return _feature_result(data)
    def execute(self, operation, scope, key=b"", value=b"", *,force_reliable=False, **options):
        tx = self.transaction()
        try:
            result = tx.execute(operation, scope, key, value, **options)
            result.version = tx.commit(force_reliable)
            return result
        finally: tx.close()
    def configure(self, *, background=False, interval_ms=200, expiry_keys=256, group_max=128,
                  group_delay_us=0, garbage_percent=30, compact_min_bytes=33554432,
                  compact_growth_bytes=33554432, job_timeout_ms=1000,job_max_read_bytes=0,job_max_write_bytes=0):
        fields = (interval_ms, expiry_keys, group_max, group_delay_us, garbage_percent)
        if any(not isinstance(v,int) or not 0 <= v < (1<<32) for v in fields): raise ValueError("runtime option out of range")
        for v in (compact_min_bytes,compact_growth_bytes,job_timeout_ms,job_max_read_bytes,job_max_write_bytes): _uint64(v)
        options = StoreRuntimeOptions(C.sizeof(StoreRuntimeOptions),2 if job_max_read_bytes or job_max_write_bytes else 1,int(bool(background)),*fields,
            compact_min_bytes,compact_growth_bytes,job_timeout_ms,job_max_read_bytes,job_max_write_bytes)
        with self._lock: self._native.check(self._native.store_configure(self._live(),C.byref(options)))
    def runtime_info(self):
        result = StoreRuntimeInfo()
        with self._lock: self._native.check(self._native.store_runtime_info_get(self._live(),C.byref(result)))
        return result
    def hold_history(self,name,from_version=None,timeout_ms=0):
        if from_version is None:from_version=(1<<64)-1
        _uint64(from_version);_uint64(timeout_ms);name,owner=_slice(name);out=StoreAtomicInfo()
        with self._lock:self._native.check(self._native.store_history_hold(self._live(),name,from_version,timeout_ms,C.byref(out)))
        return out
    def release_history(self,name,timeout_ms=0):
        _uint64(timeout_ms);name,owner=_slice(name);out=StoreAtomicInfo()
        with self._lock:self._native.check(self._native.store_history_release(self._live(),name,timeout_ms,C.byref(out)))
        return out
    def history_info(self):
        out=StoreHistoryInfo()
        with self._lock:self._native.check(self._native.store_history_info_get(self._live(),C.byref(out)))
        return out
    def define_index(self, name, key_prefix, field, field_type, flags=0, **options):
        for v in (field,field_type,flags):
            if not isinstance(v,int) or not 0 <= v < (1<<32): raise ValueError("index option out of range")
        name, a = _slice(name); prefix, b = _slice(key_prefix)
        settings = _store_maintenance_options(**options); output = StoreMaintenanceInfo()
        with self._lock: self._native.check(self._native.store_index_define(self._live(),name,prefix,field,field_type,flags,C.byref(settings),C.byref(output)))
        return output
    def define_compound_index(self,name,key_prefix,fields,flags=0,**options):
        parts=[]
        for field,kind in fields:
            if len(parts)>=8 or not isinstance(field,int) or not isinstance(kind,int) or not 0<field<=0x1fffffff or not 1<=kind<=11: raise ValueError("invalid index field")
            parts.append(StoreIndexField(field,kind))
        if not parts or not isinstance(flags,int) or not 0<=flags<=3: raise ValueError("invalid index options")
        array=(StoreIndexField*len(parts))(*parts)
        name,a=_slice(name);prefix,b=_slice(key_prefix)
        settings=_store_maintenance_options(**options);output=StoreMaintenanceInfo()
        with self._lock:self._native.check(self._native.store_index_define_compound(self._live(),name,prefix,array,len(parts),flags,C.byref(settings),C.byref(output)))
        return output
    def drop_index(self, name, **options):
        name, owner = _slice(name); settings = _store_maintenance_options(**options); output = StoreMaintenanceInfo()
        with self._lock: self._native.check(self._native.store_index_drop(self._live(),name,C.byref(settings),C.byref(output)))
        return output
    def backup_incremental(self, destination, from_version, **options):
        _uint64(from_version); settings = _store_maintenance_options(**options); output = StoreMaintenanceInfo()
        with self._lock: self._native.check(self._native.store_backup_incremental(self._live(),_store_path(destination),from_version,C.byref(settings),C.byref(output)))
        return output
    @staticmethod
    def recover_chain(base, archives, destination, *, target_version=0, target_unix_ms=0, library=None, **options):
        _uint64(target_version); _uint64(target_unix_ms)
        paths = [_store_path(p) for p in archives]
        if len(paths) > 1024: raise ValueError("too many archives")
        values = (C.c_char_p * len(paths))(*paths)
        native = _StoreNative(library); settings = _store_maintenance_options(**options); output = StoreMaintenanceInfo()
        native.check(native.store_recover_chain(_store_path(base),values,len(paths),_store_path(destination),target_version,target_unix_ms,C.byref(settings),C.byref(output)))
        return output
    def get(self, key): return self._get("store_get", key)
    def info(self):
        with self._lock:
            result = StoreInfo()
            self._native.check(self._native.store_get_info(self._live(), C.byref(result)))
            return result
    def sync(self):
        with self._lock: self._native.check(self._native.store_sync(self._live()))
    def checkpoint(self):
        with self._lock: self._native.check(self._native.store_checkpoint(self._live()))
    def _maintain(self, method, destination=None, **options):
        settings = _store_maintenance_options(**options)
        output = StoreMaintenanceInfo()
        args = [] if destination is None else [_store_path(destination)]
        with self._lock:
            self._native.check(getattr(self._native, method)(self._live(), *args, C.byref(settings), C.byref(output)))
        return output
    def compact(self, **options): return self._maintain("store_compact", **options)
    def backup(self, destination, **options): return self._maintain("store_backup", destination, **options)
    def verify(self, **options): return self._maintain("store_verify", **options)
    def purge_expired(self, *, max_keys=1024, timeout_ms=0, max_batch_bytes=1048576):
        if not isinstance(max_keys, int) or not 1 <= max_keys <= 65536:
            raise ValueError("max_keys must be between 1 and 65536")
        _uint64(timeout_ms)
        _uint64(max_batch_bytes)
        settings = StoreExpiryOptions(C.sizeof(StoreExpiryOptions), 1, max_keys, 0, timeout_ms, max_batch_bytes)
        output = StoreExpiryInfo()
        with self._lock:
            self._native.check(self._native.store_purge_expired(self._live(), C.byref(settings), C.byref(output)))
        return output
    def cancel_maintenance(self):
        with self._lock: self._native.store_cancel_maintenance(self._live())
    @staticmethod
    def restore(source, destination, *, library=None, **options):
        native = _StoreNative(library)
        settings = _store_maintenance_options(**options)
        output = StoreMaintenanceInfo()
        native.check(native.store_restore(_store_path(source), _store_path(destination), C.byref(settings), C.byref(output)))
        return output
    def transaction(self, timeout_ms=0):
        _uint64(timeout_ms)
        with self._lock:
            output = C.c_void_p()
            self._native.check(self._native.store_txn_begin(self._live(), timeout_ms, C.byref(output)))
            return StoreTransaction(self._native, output)
    def snapshot(self):
        with self._lock:
            output = C.c_void_p()
            self._native.check(self._native.store_snapshot_create(self._live(), C.byref(output)))
            return StoreSnapshot(self._native, output)

class StoreSnapshot(_Handle):
    _closer = "store_snapshot_close"
    def __init__(self, native, handle):
        super().__init__(native, handle)
        self._concurrent()
    def get(self, key): return self._get("store_snapshot_get", key)
    def get_many(self, keys): return _store_get_many(self,"store_snapshot_get_many",keys)
    def get_entries(self, keys): return _store_get_many(self,"store_snapshot_get_many",keys,True)
    def scan(self, begin=b"", end=b"", *, reverse=False):
        return _store_scan(self, "store_cursor_open", begin, end, reverse)

class StoreTransaction(_Handle):
    def submit(self,force_reliable=False):
        out=C.c_void_p()
        with self._operation_lock,self._lock:
            self._native.check(self._native.store_txn_submit(self._live(),C.byref(out),_write_flag(force_reliable)))
            self._committed=True
        try: return StoreCommit(self._native,out)
        except BaseException:
            self._native.store_commit_close(out); raise
    def query_index(self,name,*,cursor=b"",limit=256,**query):
        return self.execute(StoreOperation.INDEX_QUERY,name,cursor,_index_query(**query),limit=limit)
    def project(self,key,fields):
        return self.execute(StoreOperation.RECORD_PROJECT,b"",key,_index_query(fields=fields)).value
    _closer = "store_txn_close"
    def execute(self, operation, scope, key=b"", value=b"", **options):
        request, owner = _slice(_feature_request(operation, scope, key, value, **options))
        with self._operation_lock, self._lock:
            output = Slice()
            try:
                self._native.check(self._native.store_txn_execute(self._live(),request,C.byref(output)))
                data = C.string_at(output.data,output.size) if output.size else b""
            finally: self._native.buffer_free(C.byref(output))
        return _feature_result(data)
    def __init__(self, native, handle):
        super().__init__(native, handle)
        self._operation_lock = threading.RLock()
        self._committed = False
        self._concurrent()
    def get(self, key):
        with self._operation_lock: return self._get("store_txn_get", key)
    def get_many(self, keys):
        with self._operation_lock: return _store_get_many(self,"store_txn_get_many",keys)
    def get_entries(self, keys):
        with self._operation_lock: return _store_get_many(self,"store_txn_get_many",keys,True)
    def put_many(self, items):
        items,keep = _store_items(items)
        with self._operation_lock,self._lock: self._native.check(self._native.store_txn_put_many(self._live(),items,len(items)))
    def delete_many(self, keys):
        keys,keep = _store_keys(keys)
        with self._operation_lock,self._lock: self._native.check(self._native.store_txn_delete_many(self._live(),keys,len(keys)))
    def put_batch(self, batch):
        if not isinstance(batch,StoreBatch): raise TypeError("expected StoreBatch")
        with self._operation_lock,self._lock,batch._lock:
            self._native.check(self._native.store_txn_put_batch(self._live(),batch._live()))
    def put(self, key, value, expires_at_ms=0):
        k, ko = _slice(key)
        v, vo = _slice(value)
        _uint64(expires_at_ms)
        with self._operation_lock, self._lock:
            self._native.check(self._native.store_txn_put(self._live(), k, v, expires_at_ms))
    def delete(self, key):
        k, owner = _slice(key)
        with self._operation_lock, self._lock: self._native.check(self._native.store_txn_delete(self._live(), k))
    def expire(self, key, expires_at_ms):
        k, owner = _slice(key)
        _uint64(expires_at_ms)
        with self._operation_lock, self._lock: self._native.check(self._native.store_txn_expire(self._live(), k, expires_at_ms))
    def patch(self, key, offset, remove, insert):
        k, ko = _slice(key)
        v, vo = _slice(insert)
        _uint64(offset)
        _uint64(remove)
        with self._operation_lock, self._lock:
            self._native.check(self._native.store_txn_patch(self._live(), k, offset, remove, v))
    def savepoint(self):
        with self._operation_lock, self._lock:
            result = C.c_uint64()
            self._native.check(self._native.store_txn_savepoint(self._live(), C.byref(result)))
            return result.value
    def rollback_to(self, savepoint):
        _uint64(savepoint)
        with self._operation_lock, self._lock: self._native.check(self._native.store_txn_rollback_to(self._live(), savepoint))
    def release(self, savepoint):
        _uint64(savepoint)
        with self._operation_lock, self._lock: self._native.check(self._native.store_txn_release(self._live(), savepoint))
    def cancel(self):
        with self._lock: self._native.store_txn_cancel(self._live())
    def scan(self, begin=b"", end=b"", *, reverse=False):
        with self._operation_lock: return _store_scan(self, "store_txn_cursor_open", begin, end, reverse)
    def commit(self,force_reliable=False):
        with self._operation_lock, self._lock:
            result = C.c_uint64()
            self._native.check(self._native.store_txn_commit(self._live(), C.byref(result),_write_flag(force_reliable)))
            self._committed = True
            return result.value
    def __exit__(self, kind, *_):
        try:
            if kind is None and not self._committed: self.commit()
        finally: self.close()

class StorePreparedKey(_Handle):
    def get_shared(self): return _store_shared(self)
    _closer = "store_key_close"
    def __init__(self, native, handle):
        super().__init__(native,handle); self._concurrent()
    def get_entry(self): return _store_entry(self)
    def get(self):
        with self._lock:
            result = _StoreSharedValue()
            status = self._native.store_key_get_shared(self._live(), C.byref(result))
            if status == 1: return None
            self._native.check(status)
            try:
                value = result.row.value
                return C.string_at(value.data, value.size) if value.size else b""
            finally: self._native.store_value_close(result.owner)
    def get_into(self, destination):
        if not isinstance(destination,bytearray): raise TypeError("expected bytearray")
        buffer = (C.c_ubyte*len(destination)).from_buffer(destination); required = C.c_uint64()
        with self._lock:
            code = self._native.store_key_get_into(self._live(),buffer,len(buffer),C.byref(required))
            if code == 1: return None
            if code == 16: raise BufferTooSmall(required.value)
            self._native.check(code); return required.value
    def bind_buffer(self, destination):
        """Reuse a fixed bytearray export and ctypes arguments across reads.

        Close the reader before resizing the bytearray. Closing the reader does
        not close this key; closing the key makes subsequent reads fail.
        """
        with self._lock:
            self._live()
            return StoreBufferReader(self, destination)

class StoreBufferReader:
    """Bound output buffer. read() returns its byte count, or None if missing.

    Calls on one reader are serialized. The caller must synchronize other access
    to the destination. Reads leave the unused tail alone and never resize it.
    A strong reference to the key wrapper keeps an otherwise unclosed key alive;
    the native argument owner is acquired only for the duration of each call.
    """
    def __init__(self, key, destination):
        self._lock = threading.Lock()
        self._key = self._buffer = None
        if not isinstance(destination, bytearray):
            raise TypeError("expected bytearray")
        self._buffer = (C.c_ubyte * len(destination)).from_buffer(destination)
        self._pointer = C.cast(C.addressof(self._buffer), C.c_void_p)
        self._capacity = len(destination)
        self._required = C.c_uint64()
        self._required_pointer = C.byref(self._required)
        self._key = key
    def read(self):
        with self._lock:
            key = self._key
            if key is None:
                raise RuntimeError("Buffer reader is closed")
            with key._lock:
                code = key._native.store_key_get_into(key._live(), self._pointer,
                                                      self._capacity, self._required_pointer)
                if code == 1: return None
                if code == 16: raise BufferTooSmall(self._required.value)
                key._native.check(code)
                return self._required.value
    def close(self):
        with self._lock:
            self._key = self._buffer = None
            self._pointer = None
    def __enter__(self):
        with self._lock:
            if self._key is None: raise RuntimeError("Buffer reader is closed")
        return self
    def __exit__(self, *_):
        self.close()
    def __del__(self):
        try: self.close()
        except Exception: pass

class StoreBatch(_Handle):
    _closer = "store_batch_close"
    def __init__(self, native, handle):
        super().__init__(native,handle); self._concurrent()

class StoreWatch(_Handle):
    _closer='store_watch_close'
    def __init__(self,native,handle):
        super().__init__(native,handle)
        self._concurrent()
    def next_batch(self,timeout_ms=0):
        _uint64(timeout_ms)
        with self._lock:
            out=C.c_void_p()
            status=self._native.store_watch_next(self._live(),timeout_ms,C.byref(out))
            if status in (17,18):return None
            self._native.check(status)
            try:return StoreChanges(self._native.store_change_batch_data(out).contents)
            finally:self._native.store_change_batch_close(out)
    def ack(self,version):
        _uint64(version)
        with self._lock:self._native.check(self._native.store_watch_ack(self._live(),version))
    def cancel(self):
        with self._lock:self._native.store_watch_cancel(self._live())
    def close(self):
        try:self.cancel()
        except RuntimeError:pass
        super().close()

class StoreCursor(_Handle):
    _closer = "store_cursor_close"
    def __iter__(self): return self
    def __next__(self):
        with self._lock:
            row = _StoreRow()
            status = self._native.store_cursor_next(self._live(), C.byref(row))
            if status == 2: raise StopIteration
            self._native.check(status)
            return (C.string_at(row.key.data, row.key.size) if row.key.size else b"",
                    C.string_at(row.value.data, row.value.size) if row.value.size else b"",
                    row.version, row.expires_at_ms)
