"""Exercise fixed output exports, native leases and concurrent close."""
import argparse
import concurrent.futures
import gc
from pathlib import Path
import sys
import threading
import weakref

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bindings/python"))
import fengqudb as fq


def closed(operation):
    try:
        operation()
    except RuntimeError:
        return
    raise AssertionError("Closed reader/key was accepted")


def checks(root, library):
    with fq.Store.open_at(root / "db", "main", library=library, buffered=True, segmented=True) as db:
        db.put_if_changed(b"key", b"abc")
        key = db.prepare_key(b"key")
        target = bytearray(b"????????")
        reader = key.bind_buffer(target)
        assert reader.read() == 3 and target == b"abc?????"
        try:
            target.extend(b"!")
        except BufferError:
            pass
        else:
            raise AssertionError("Bound buffer was allowed to resize")
        for invalid in (b"readonly", memoryview(target), None):
            try:
                key.bind_buffer(invalid)
            except TypeError:
                pass
            else:
                raise AssertionError("Non-bytearray output accepted")
        small = bytearray(b"??")
        with key.bind_buffer(small) as short:
            try:
                short.read()
            except fq.BufferTooSmall as error:
                assert error.required_size == 3
            else:
                raise AssertionError("Small output accepted")
            assert small == b"??"
            db.put_if_changed(b"key", b"x")
            assert short.read() == 1 and small == b"x?"
        db.put_if_changed(b"key", b"")
        with key.bind_buffer(bytearray()) as zero:
            assert zero.read() == 0
            db.delete_many([b"key"])
            assert zero.read() is None
        db.put_if_changed(b"key", b"latest")
        db.checkpoint()
        db.compact()
        assert reader.read() == 6 and target == b"latest??"
        reference = weakref.ref(key)
        del key
        gc.collect()
        assert reference() is not None and reader.read() == 6
        reader.close()
        reader.close()
        closed(reader.read)
        gc.collect()
        assert reference() is None, "Reader leaked its key"
        target.extend(b"!")
        key = db.prepare_key(b"key")
        reader = key.bind_buffer(target)
        # The reader must not retain a _NativeOwner between calls: close must
        # complete with the reader still alive, rather than waiting for it.
        with concurrent.futures.ThreadPoolExecutor(1) as pool:
            pool.submit(key.close).result(timeout=5)
        closed(reader.read)
        reader.close()
        key.close()
        closed(lambda: key.bind_buffer(bytearray(16)))
        child = db.prepare_key(b"key")
        with child.bind_buffer(target) as reader:
            db.close()
            assert reader.read() == 6  # Child owns the native Store lifetime.
        child.close()

    # Hold an actual native-call argument owner while close starts. Neither
    # close nor reader destruction may free the key until that call returns.
    with fq.Store.open_at(root / "db", "race", library=library, buffered=True, segmented=True) as db:
        payload = bytes(range(256)) * 256
        db.put_if_changed(b"key", payload)
        key = db.prepare_key(b"key")
        target = bytearray(len(payload))
        reader = key.bind_buffer(target)
        entered, resume, closing = threading.Event(), threading.Event(), threading.Event()
        native = key._native.store_key_get_into

        def delayed(*args):
            entered.set()
            assert resume.wait(5)
            return native(*args)

        def close_key():
            closing.set()
            key.close()

        key._native.store_key_get_into = delayed
        with concurrent.futures.ThreadPoolExecutor(3) as pool:
            result = pool.submit(reader.read)
            assert entered.wait(5)
            close_result = pool.submit(close_key)
            assert closing.wait(5)
            assert not close_result.done()
            reader_close = pool.submit(reader.close)
            assert not reader_close.done()
            resume.set()
            assert result.result(timeout=5) == len(payload)
            close_result.result(timeout=5)
            reader_close.result(timeout=5)
        key._native.store_key_get_into = native
        assert target == payload
        target.append(1)
        closed(reader.read)
        key.close()


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("library", type=Path)
    parser.add_argument("--fallback", action="store_true")
    args = parser.parse_args()
    args.root.mkdir(parents=True, exist_ok=False)
    if args.fallback:
        fq._REFERENCE_LEASES = False
    checks(args.root, args.library)
    print("PASS Python fixed buffer, resize protection, leases, close and parent lifetime",
          "fallback" if args.fallback else "CPython")


if __name__ == "__main__":
    main()
