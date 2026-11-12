package io.fengqudb;
import java.nio.charset.StandardCharsets;
import java.nio.ByteBuffer;
import java.lang.ref.PhantomReference;
import java.lang.ref.ReferenceQueue;
import java.util.concurrent.ConcurrentHashMap;
import java.util.Iterator;
import java.util.ArrayList;
import java.util.List;
import java.io.ByteArrayOutputStream;
import java.util.NoSuchElementException;
import java.util.concurrent.locks.ReentrantReadWriteLock;
import java.util.function.LongFunction;
import java.util.function.LongUnaryOperator;

/**
 * Paged database directory. Byte arrays returned by reads belong to Java.
 * Version longs carry unsigned 64-bit patterns; compare/display them with
 * Long.compareUnsigned/Long.toUnsignedString. Sizes and timeouts are nonnegative.
 */
public final class Store implements AutoCloseable {
    public static final class DbException extends RuntimeException {
        public final int code;
        public DbException(int code) {
            super("FengquDB " + code + ": " + nStatusMessage(code));
            this.code = code;
        }
    }
    private static native int nAbiVersion();
    private static native String nStatusMessage(int code);
    private static native boolean nSharedBufferAliasesRetainRoot();
    private static native ByteBuffer nGetShared(long handle, byte[] key, int kind, long[] metadata);
    private static native long nReaderCreate(long handle, byte[][] keys);
    private static native long nReaderAcquire(long handle, ByteBuffer[] buffers, long[] metadata);
    private static native void nReaderRelease(long handle);
    static {
        System.loadLibrary("fengqudb");
        System.loadLibrary("fengqudb_jni");
        if (nAbiVersion() != 2)
            throw new UnsupportedOperationException("Use matching FengquDB ABI 2 libraries");
    }
    public static final class Options {
        public boolean buffered = false, create = true, segmented = false;
        public long cacheBytes = 64L * 1024 * 1024, deltaBytes = 4L * 1024 * 1024,
                    writerBytes = 32L * 1024 * 1024;
        public long transactionBytes = 64L * 1024 * 1024, allTransactionBytes = 256L * 1024 * 1024;
        public long pinnedBytes = 128L * 1024 * 1024, maxSnapshots = 1024;
    }
    public enum WriteMode { RELIABLE, PERFORMANCE }
    public static final class WriteOptions {
        public WriteMode mode = WriteMode.PERFORMANCE;
        public long periodMs = 50, thresholdBytes = 1048576;
    }
    /** Admission is deferred. Poll from an event loop; await() and close() can block. */
    public static final class CommitReceipt implements AutoCloseable {
        private final Resource resource;
        private CommitReceipt(long handle) {
            resource = new Resource(handle, 10);
        }
        private static Long result(long[] values) {
            if (values == null)
                return null;
            if (values[0] != 0)
                throw new DbException((int)values[0]);
            return values[1];
        }
        public Long poll() {
            return resource.call(h -> result(nCommitResult(h, 0, true)));
        }
        public long await(long timeoutMs) {
            positive(timeoutMs);
            return resource.call(h -> result(nCommitResult(h, timeoutMs, false)));
        }
        public long await() {
            return await(0);
        }
        public void cancel() {
            resource.call(h -> {
                nCommitCancel(h);
                return null;
            });
        }
        public void close() {
            resource.close();
        }
    }
    private static CommitReceipt receipt(long handle) {
        try {
            return new CommitReceipt(handle);
        } catch (Throwable error) {
            nClose(handle, 10);
            throw error;
        }
    }
    public static final class Row {
        public final byte[] key, value;
        public final long version, expiresAtMs;
        public Row(byte[] key, byte[] value, long version, long expiresAtMs) {
            this.key = key;
            this.value = value;
            this.version = version;
            this.expiresAtMs = expiresAtMs;
        }
    }
    public static final class Entry {
        public final byte[] value;
        public final long version, expiresAtMs;
        private Entry(byte[] value, long version, long expiresAtMs) {
            this.value = value;
            this.version = version;
            this.expiresAtMs = expiresAtMs;
        }
    }
    public static final class Put {
        public final byte[] key, value;
        public final long expiresAtMs;
        public Put(byte[] key, byte[] value) {
            this(key, value, 0);
        }
        public Put(byte[] key, byte[] value, long expiresAtMs) {
            if (key == null || value == null)
                throw new IllegalArgumentException("Null key/value");
            this.key = key;
            this.value = value;
            this.expiresAtMs = positive(expiresAtMs);
        }
    }
    public static final class Int64FieldUpdate {
        public final byte[] key;
        public final long offset, delta, minimum, maximum;
        public Int64FieldUpdate(byte[] key, long offset, long delta) {
            this(key, offset, delta, Long.MIN_VALUE, Long.MAX_VALUE);
        }
        public Int64FieldUpdate(byte[] key, long offset, long delta, long minimum, long maximum) {
            if (key == null || minimum > maximum)
                throw new IllegalArgumentException("Invalid field update");
            this.key = key;
            this.offset = positive(offset);
            this.delta = delta;
            this.minimum = minimum;
            this.maximum = maximum;
        }
    }
    public static final class AtomicInfo {
        public long version, observedVersion, value;
        public boolean matched, changed;
        private AtomicInfo() {}
        private void assign(long[] out) {
            version = out[0];
            observedVersion = out[1];
            value = out[2];
            matched = out[3] != 0;
            changed = out[4] != 0;
        }
    }
    public static final class PreparedKey implements AutoCloseable {
        private final Resource resource = new Resource(0, 4);
        private PreparedKey() {}
        public SharedValue getShared() {
            return resource.call(h -> sharedValue(h, null, 4));
        }
        public Entry getEntry() {
            return resource.call(h -> nGetEntry(h, null, 4));
        }
        public byte[] get() {
            return resource.call(h -> nKeyGet(h));
        }
        public long getInto(byte[] destination) {
            return resource.callLong(h -> nKeyGetInto(h, destination));
        }
        /**
         * Copy into a writable direct buffer at its position, up to remaining().
         * Returns byte count, or -1 for a missing key. Position/limit and the
         * unused tail remain unchanged. Insufficient space throws code 16
         * without modifying bytes. Synchronize concurrent access to the buffer.
         */
        public long getIntoDirect(ByteBuffer destination) {
            if (destination == null || !destination.isDirect() || destination.isReadOnly())
                throw new IllegalArgumentException("Expected writable direct ByteBuffer");
            int offset = destination.position(), size = destination.remaining();
            return resource.callLong(h -> nKeyGetIntoDirect(h, destination, offset, size));
        }
        public void close() {
            resource.close();
        }
    }
    // Share only when the runtime probe confirms that derived direct buffers
    // retain their original buffer. Other VMs use an owned direct-buffer copy.
    private static final class SharedOwner {
        final long handle;
        final int kind;
        int references = 1;
        SharedOwner(long handle, int kind) {
            this.handle = handle;
            this.kind = kind;
        }
        synchronized void retain() {
            ++references;
        }
        synchronized void release() {
            if (--references == 0)
                nClose(handle, kind);
        }
    }
    private static final class BufferLease extends PhantomReference<ByteBuffer> {
        static final ReferenceQueue<ByteBuffer> queue = new ReferenceQueue<>();
        static final ConcurrentHashMap<BufferLease, Boolean> active = new ConcurrentHashMap<>();
        final SharedOwner owner;
        BufferLease(ByteBuffer buffer, SharedOwner owner) {
            super(buffer, queue);
            this.owner = owner;
        }
        static {
            Thread reaper = new Thread(() -> {
                while (true) {
                    try {
                        BufferLease lease = (BufferLease)queue.remove();
                        active.remove(lease);
                        lease.clear();
                        lease.owner.release();
                    } catch (InterruptedException ignored) {
                    }
                }
            }, "FengquDB-shared-buffers");
            reaper.setDaemon(true);
            reaper.start();
        }
        static void register(ByteBuffer buffer, SharedOwner owner) {
            synchronized (buffer) {
                BufferLease lease = new BufferLease(buffer, owner);
                owner.retain();
                try {
                    active.put(lease, Boolean.TRUE);
                } catch (Throwable error) {
                    owner.release();
                    throw error;
                }
            }
        }
    }
    /**
     * Shared immutable bytes. buffer() returns a read-only direct buffer.
     * Exported duplicates/slices survive writes, Store close and wrapper close.
     * isZeroCopy() reports whether this VM supports shared-buffer ownership.
     * Native pins are released after ALL buffer aliases become unreachable and
     * the GC queue is processed. For bounded deterministic reuse use getIntoDirect.
     */
    public static final class SharedValue implements AutoCloseable {
        private static final boolean SHARE =
            !Boolean.getBoolean("io.fengqudb.shared.copy") && nSharedBufferAliasesRetainRoot();
        private ByteBuffer bytes;
        public final long version, expiresAtMs;
        public boolean isZeroCopy() {
            return SHARE;
        }
        private SharedValue(ByteBuffer buffer, long version, long expires, SharedOwner owner) {
            this.version = version;
            expiresAtMs = expires;
            if (SHARE) {
                bytes = buffer.asReadOnlyBuffer();
                BufferLease.register(buffer, owner);
            } else {
                ByteBuffer owned = ByteBuffer.allocateDirect(buffer.remaining());
                owned.put(buffer).flip();
                bytes = owned.asReadOnlyBuffer();
            }
        }
        public synchronized ByteBuffer buffer() {
            if (bytes == null)
                throw new IllegalStateException("Shared value closed");
            return bytes.asReadOnlyBuffer();
        }
        public synchronized void close() {
            bytes = null;
        }
    }
    /** Any exported row keeps the immutable batch pinned until its aliases are collected. */
    public static final class SharedBatch implements AutoCloseable {
        private SharedValue[] values;
        private SharedBatch(SharedValue[] values) {
            this.values = values;
        }
        public synchronized int size() {
            return values.length;
        }
        public synchronized SharedValue get(int index) {
            return values[index];
        }
        public synchronized void close() {
            values = new SharedValue[0];
        }
    }
    public static final class PreparedReader implements AutoCloseable {
        private final Resource resource = new Resource(0, 9);
        private final int count;
        private PreparedReader(int count) {
            this.count = count;
        }
        public synchronized SharedBatch readShared() {
            ByteBuffer[] buffers = new ByteBuffer[count];
            long[] metadata = new long[count * 2];
            long handle = resource.callLong(h -> nReaderAcquire(h, buffers, metadata));
            SharedOwner owner = adoptOwner(handle, 8);
            try {
                SharedValue[] values = new SharedValue[count];
                for (int i = 0; i < count; ++i)
                    if (buffers[i] != null)
                        values[i] = new SharedValue(buffers[i], metadata[i * 2],
                                                    metadata[i * 2 + 1], owner);
                return new SharedBatch(values);
            } finally {
                owner.release();
            }
        }
        public synchronized void release() {
            resource.callLong(h -> {
                nReaderRelease(h);
                return 0;
            });
        }
        public synchronized void close() {
            resource.close();
        }
    }
    private static SharedOwner adoptOwner(long handle, int kind) {
        try {
            return new SharedOwner(handle, kind);
        } catch (Throwable error) {
            nClose(handle, kind);
            throw error;
        }
    }
    private static SharedValue sharedValue(long handle, byte[] key, int kind) {
        long[] metadata = new long[3];
        ByteBuffer buffer = nGetShared(handle, key, kind, metadata);
        if (buffer == null)
            return null;
        SharedOwner owner = adoptOwner(metadata[0], 7);
        try {
            return new SharedValue(buffer, metadata[1], metadata[2], owner);
        } finally {
            owner.release();
        }
    }
    public static final class PreparedBatch implements AutoCloseable {
        private final Resource resource = new Resource(0, 5);
        private PreparedBatch() {}
        public void close() {
            resource.close();
        }
    }
    public static final class ExpiryOptions {
        public int maxKeys = 1024;
        public long timeoutMs = 0, maxBatchBytes = 1048576;
        private long[] nativeValues() {
            if (maxKeys < 1 || maxKeys > 65536)
                throw new IllegalArgumentException("Invalid maximum keys");
            positive(timeoutMs);
            positive(maxBatchBytes);
            return new long[] {maxKeys, timeoutMs, maxBatchBytes};
        }
    }
    public static final class ExpiryInfo {
        public long version, cutoffMs, removedKeys, removedValueBytes, visitedPages, more;
        private ExpiryInfo() {}
        private void assign(long[] values) {
            version = values[0];
            cutoffMs = values[1];
            removedKeys = values[2];
            removedValueBytes = values[3];
            visitedPages = values[4];
            more = values[5];
        }
    }
    public static final class MaintenanceOptions {
        public int maxCatchupRounds = 8;
        public long timeoutMs = 0, maxBarrierBytes = 1048576;
        public long maxReadBytes, maxWriteBytes;
        public MaintenanceIO ioStats;
        private long[] nativeValues() {
            positive(timeoutMs);
            positive(maxBarrierBytes);
            if (maxCatchupRounds < 0 || maxCatchupRounds > 64)
                throw new IllegalArgumentException("Invalid catch-up rounds");
            return new long[] {maxCatchupRounds, timeoutMs, maxBarrierBytes, positive(maxReadBytes),
                               positive(maxWriteBytes)};
        }
        private long[] output() {
            return new long[ioStats == null ? 8 : 11];
        }
        private void collect(long[] output) {
            if (ioStats != null) {
                ioStats.readBytes = output[8];
                ioStats.writeBytes = output[9];
                ioStats.syncCalls = output[10];
            }
        }
    }
    public static final class MaintenanceIO {
        public long readBytes, writeBytes, syncCalls;
    }
    public static final class MaintenanceInfo {
        public long snapshotVersion, finalVersion, sourceFileBytes, outputFileBytes;
        public long storedKeys, storedValueBytes, copiedKeys, catchupRounds;
        private MaintenanceInfo() {}
        private void assign(long[] values) {
            snapshotVersion = values[0];
            finalVersion = values[1];
            sourceFileBytes = values[2];
            outputFileBytes = values[3];
            storedKeys = values[4];
            storedValueBytes = values[5];
            copiedKeys = values[6];
            catchupRounds = values[7];
        }
    }
    public static final class Operation {
        public static final int HASH_GET = 1, HASH_SET = 2, HASH_DELETE = 3, HASH_SCAN = 4;
        public static final int SET_CONTAINS = 10, SET_ADD = 11, SET_REMOVE = 12, SET_SCAN = 13;
        public static final int LIST_PUSH_LEFT = 20, LIST_PUSH_RIGHT = 21, LIST_POP_LEFT = 22,
                                LIST_POP_RIGHT = 23, LIST_GET = 24, LIST_SET = 25, LIST_RANGE = 26;
        public static final int SORTED_ADD = 30, SORTED_REMOVE = 31, SORTED_SCORE = 32,
                                SORTED_RANGE = 33;
        public static final int COLLECTION_SIZE = 40, COLLECTION_CLEAR = 41, INDEX_LOOKUP = 50,
                                INDEX_QUERY = 51, RECORD_PROJECT = 52;
    }
    public static final class FeatureRow {
        public byte[] key = new byte[0], value = new byte[0];
        public double score;
    }
    public static final class CommandResult {
        public long version, count;
        public boolean changed;
        public byte[] value;
        public byte[] cursor = new byte[0];
        public final List<FeatureRow> rows = new ArrayList<>();
    }
    public static final class IndexField {
        public final int field, type;
        public IndexField(int field, int type) {
            if (field <= 0 || field > 0x1fffffff || type < 1 || type > 11)
                throw new IllegalArgumentException("Invalid index field");
            this.field = field;
            this.type = type;
        }
    }
    public static final class IndexQuery {
        public byte[] equal = new byte[0], lower, upper;
        public boolean lowerExclusive, upperInclusive, reverse, includeValues;
        public int[] fields = new int[0];
        private byte[] encode() {
            if (fields == null || fields.length > 256)
                throw new IllegalArgumentException("Invalid projection fields");
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            FeatureWire.bytes(out, 1, equal);
            if (lower != null)
                FeatureWire.bytes(out, 2, lower);
            if (upper != null)
                FeatureWire.bytes(out, 3, upper);
            FeatureWire.var(out, 32);
            FeatureWire.var(out, (lowerExclusive ? 1 : 0) | (upperInclusive ? 2 : 0) |
                                     (reverse ? 4 : 0) | (includeValues ? 8 : 0));
            for (int field : fields) {
                if (field <= 0 || field > 0x1fffffff)
                    throw new IllegalArgumentException("Invalid projection field");
                FeatureWire.var(out, 40);
                FeatureWire.var(out, field);
            }
            return out.toByteArray();
        }
    }
    public static final class HistoryInfo {
        public final long version, floor, oldestPin, activePins, totalPins, retainedGenerations,
            retainedFileBytes;
        private HistoryInfo(long[] values) {
            version = values[0];
            floor = values[1];
            oldestPin = values[2];
            activePins = values[3];
            totalPins = values[4];
            retainedGenerations = values[5];
            retainedFileBytes = values[6];
        }
    }
    public static final class WatchOptions {
        public boolean metadataOnly, includeSystem;
        public int maxScanCommits = 256;
        public long maxBatchBytes = 67108864;
        private long[] nativeValues() {
            return new long[] {(metadataOnly ? 1 : 0) | (includeSystem ? 2 : 0), maxScanCommits,
                               positive(maxBatchBytes)};
        }
    }
    public static final class Change {
        public final byte[] key, value;
        public final long expiresAtMs;
        public final int kind;
        private Change(byte[] key, byte[] value, long expires, int kind) {
            this.key = key;
            this.value = value;
            this.expiresAtMs = expires;
            this.kind = kind;
        }
    }
    public static final class Changes {
        public final long version, unixMs, durableVersion;
        public final int flags;
        public final Change[] rows;
        private Changes(Object[] data) {
            byte[][] bytes = (byte[][])data[0];
            long[] numbers = (long[])data[1];
            version = numbers[0];
            unixMs = numbers[1];
            durableVersion = numbers[2];
            flags = (int)numbers[3];
            rows = new Change[bytes.length / 2];
            for (int i = 0; i < rows.length; ++i)
                rows[i] = new Change(bytes[2 * i], bytes[2 * i + 1], numbers[4 + 2 * i],
                                     (int)numbers[5 + 2 * i]);
        }
    }
    public static final class Watch implements AutoCloseable {
        private final Resource resource = new Resource(0, 6);
        private Watch() {}
        public Changes next() {
            return next(0);
        }
        public Changes next(long timeoutMs) {
            positive(timeoutMs);
            Object[] data = resource.call(h -> nWatchNext(h, timeoutMs));
            return data == null ? null : new Changes(data);
        }
        public void ack(long version) {
            resource.call(h -> {
                nWatchAck(h, version);
                return null;
            });
        }
        public void cancel() {
            resource.call(h -> {
                nWatchCancel(h);
                return null;
            });
        }
        public void close() {
            resource.close();
        }
    }
    public static final class RuntimeOptions {
        public boolean background = false;
        public int intervalMs = 200, expiryKeys = 256, groupMax = 128, groupDelayUs = 0,
                   garbagePercent = 30;
        public long compactMinBytes = 33554432, compactGrowthBytes = 33554432, jobTimeoutMs = 1000;
        public long jobMaxReadBytes, jobMaxWriteBytes;
        private long[] values() {
            long[] result = {background ? 1 : 0, intervalMs,         expiryKeys,
                             groupMax,           groupDelayUs,       garbagePercent,
                             compactMinBytes,    compactGrowthBytes, jobTimeoutMs,
                             jobMaxReadBytes,    jobMaxWriteBytes};
            for (long value : result)
                positive(value);
            return result;
        }
    }
    public static final class RuntimeInfo {
        public long running, lastStatus, expiryBatches, compactions, groupFlushes,
            groupTransactions;
        private void assign(long[] v) {
            running = v[0];
            lastStatus = v[1];
            expiryBatches = v[2];
            compactions = v[3];
            groupFlushes = v[4];
            groupTransactions = v[5];
        }
    }
    private static final class FeatureWire {
        static void var(ByteArrayOutputStream out, long v) {
            while ((v & ~127L) != 0) {
                out.write((int)((v & 127) | 128));
                v >>>= 7;
            }
            out.write((int)v);
        }
        static void fixed(ByteArrayOutputStream out, long v) {
            for (int i = 0; i < 8; i++) {
                out.write((int)(v & 255));
                v >>>= 8;
            }
        }
        static void bytes(ByteArrayOutputStream out, int field, byte[] data) {
            if (data == null)
                data = new byte[0];
            var(out, field * 8 + 2);
            var(out, data.length);
            out.write(data, 0, data.length);
        }
        static byte[] request(int operation, byte[] scope, byte[] key, byte[] value, long argument,
                              double score, long limit, int flags) {
            if (scope == null || operation < 0 || flags < 0)
                throw new IllegalArgumentException();
            positive(limit);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            out.write(8);
            var(out, operation);
            bytes(out, 2, scope);
            bytes(out, 3, key);
            bytes(out, 4, value);
            out.write(41);
            fixed(out, argument);
            out.write(49);
            fixed(out, Double.doubleToRawLongBits(score));
            out.write(57);
            fixed(out, limit);
            out.write(64);
            var(out, flags);
            return out.toByteArray();
        }
        static final class Reader {
            final byte[] data;
            int at = 0;
            Reader(byte[] bytes) {
                data = bytes;
            }
            long var() {
                long result = 0;
                for (int shift = 0; shift < 64; shift += 7) {
                    if (at >= data.length)
                        throw new IllegalStateException("Truncated response");
                    int b = data[at++] & 255;
                    result |= (long)(b & 127) << shift;
                    if ((b & 128) == 0)
                        return result;
                }
                throw new IllegalStateException("Invalid varint");
            }
            long fixed(int n) {
                if (at > data.length - n)
                    throw new IllegalStateException("Truncated response");
                long result = 0;
                for (int i = 0; i < n; i++)
                    result |= (long)(data[at++] & 255) << (8 * i);
                return result;
            }
            byte[] bytes() {
                long length = var();
                if (length < 0 || length > data.length - at)
                    throw new IllegalStateException("Invalid response");
                byte[] result = new byte[(int)length];
                System.arraycopy(data, at, result, 0, result.length);
                at += result.length;
                return result;
            }
            void skip(int kind) {
                if (kind == 0)
                    var();
                else if (kind == 1)
                    fixed(8);
                else if (kind == 5)
                    fixed(4);
                else if (kind == 2)
                    bytes();
                else
                    throw new IllegalStateException("Invalid response");
            }
        }
        static CommandResult result(byte[] bytes) {
            CommandResult result = new CommandResult();
            Reader r = new Reader(bytes);
            while (r.at < bytes.length) {
                int tag = (int)r.var(), field = tag >>> 3;
                if (field == 1)
                    result.value = r.bytes();
                else if (field == 2)
                    result.count = r.fixed(8);
                else if (field == 3)
                    result.changed = r.var() != 0;
                else if (field == 5)
                    result.cursor = r.bytes();
                else if (field == 4) {
                    byte[] item = r.bytes();
                    Reader p = new Reader(item);
                    FeatureRow row = new FeatureRow();
                    while (p.at < item.length) {
                        int t = (int)p.var();
                        if ((t >>> 3) == 1)
                            row.key = p.bytes();
                        else if ((t >>> 3) == 2)
                            row.value = p.bytes();
                        else if ((t >>> 3) == 3)
                            row.score = Double.longBitsToDouble(p.fixed(8));
                        else
                            p.skip(t & 7);
                    }
                    result.rows.add(row);
                } else
                    r.skip(tag & 7);
            }
            return result;
        }
    }
    private static final class Resource implements AutoCloseable {
        private final ReentrantReadWriteLock life = new ReentrantReadWriteLock(true);
        private long handle;
        private final int kind;
        Resource(long value, int kind) {
            handle = value;
            this.kind = kind;
        }
        <T> T call(LongFunction<T> action) {
            life.readLock().lock();
            try {
                if (handle == 0)
                    throw new IllegalStateException("Handle closed");
                return action.apply(handle);
            } finally {
                life.readLock().unlock();
            }
        }
        long callLong(LongUnaryOperator action) {
            life.readLock().lock();
            try {
                if (handle == 0)
                    throw new IllegalStateException("Handle closed");
                return action.applyAsLong(handle);
            } finally {
                life.readLock().unlock();
            }
        }
        public void close() {
            if (kind == 6) {
                life.readLock().lock();
                try {
                    if (handle != 0)
                        nWatchCancel(handle);
                } finally {
                    life.readLock().unlock();
                }
            }
            life.writeLock().lock();
            try {
                if (handle != 0) {
                    long value = handle;
                    handle = 0;
                    nClose(value, kind);
                }
            } finally {
                life.writeLock().unlock();
            }
        }
    }
    private final Resource resource;
    private static long positive(long value) {
        if (value < 0)
            throw new IllegalArgumentException("Negative unsigned argument");
        return value;
    }
    private static long[] nativeLimits(Options options) {
        long[] limits = new long[options.segmented ? 8 : 7];
        limits[0] = options.cacheBytes;
        limits[1] = options.deltaBytes;
        limits[2] = options.writerBytes;
        limits[3] = options.transactionBytes;
        limits[4] = options.allTransactionBytes;
        limits[5] = options.pinnedBytes;
        limits[6] = options.maxSnapshots;
        if (options.segmented)
            limits[7] = 1;
        return limits;
    }
    public Store(String directory) {
        this(directory, new Options());
    }
    public Store(String directory, Options options) {
        if (directory == null || directory.isEmpty() || directory.indexOf('\0') >= 0 ||
            options == null)
            throw new IllegalArgumentException("Invalid Store arguments");
        long[] limits = nativeLimits(options);
        for (long value : limits)
            positive(value);
        resource = new Resource(0, 0);
        resource.handle = nOpen(directory.getBytes(StandardCharsets.UTF_8), options.buffered,
                                options.create, limits);
    }
    private Store(Resource resource) {
        this.resource = resource;
    }
    public static String pathAt(String root, String name) {
        return new String(nPathAt(path(root), path(name)), StandardCharsets.UTF_8);
    }
    public static Store openAt(String root, String name) {
        return openAt(root, name, new Options());
    }
    public static Store openAt(String root, String name, Options options) {
        if (options == null)
            throw new IllegalArgumentException("Null options");
        byte[] a = path(root), b = path(name);
        long[] limits = nativeLimits(options);
        for (long value : limits)
            positive(value);
        Resource resource = new Resource(0, 0);
        Store result = new Store(resource);
        resource.handle = nOpenAt(a, b, options.buffered, options.create, limits);
        return result;
    }
    public SharedValue getShared(byte[] key) {
        return resource.call(h -> sharedValue(h, key, 0));
    }
    public PreparedReader prepareReader(byte[] key) {
        return prepareReader(new byte[][] {key});
    }
    public PreparedReader prepareReader(byte[][] keys) {
        if (keys == null || keys.length > 4096)
            throw new IllegalArgumentException("Invalid keys");
        PreparedReader result = new PreparedReader(keys.length);
        resource.call(h -> {
            result.resource.handle = nReaderCreate(h, keys);
            return null;
        });
        return result;
    }
    public PreparedReader prepareProjectReader(byte[][] keys, int[] fields) {
        if (keys == null || keys.length > 4096 || fields == null || fields.length > 256)
            throw new IllegalArgumentException("Invalid projection");
        PreparedReader result = new PreparedReader(keys.length);
        resource.call(h -> {
            result.resource.handle = nProjectReaderCreate(h, keys, fields);
            return null;
        });
        return result;
    }
    public Entry getEntry(byte[] key) {
        return resource.call(h -> nGetEntry(h, key, 0));
    }
    public byte[][] getMany(byte[][] keys) {
        return resource.call(h -> (byte[][])nGetMany(h, keys, 0, false));
    }
    public Entry[] getEntries(byte[][] keys) {
        return resource.call(h -> (Entry[])nGetMany(h, keys, 0, true));
    }
    public PreparedKey prepareKey(byte[] key) {
        PreparedKey result = new PreparedKey();
        resource.call(h -> {
            result.resource.handle = nPrepareKey(h, key);
            return null;
        });
        return result;
    }
    private static byte[][] putKeys(Put[] items, boolean values) {
        if (items == null || items.length > 4096)
            throw new IllegalArgumentException("Invalid items");
        byte[][] result = new byte[items.length][];
        for (int i = 0; i < items.length; i++) {
            if (items[i] == null)
                throw new IllegalArgumentException("Null item");
            result[i] = values ? items[i].value : items[i].key;
        }
        return result;
    }
    private static long[] putExpiries(Put[] items) {
        long[] result = new long[items.length];
        for (int i = 0; i < items.length; i++)
            result[i] = items[i].expiresAtMs;
        return result;
    }
    public PreparedBatch prepareBatch(Put[] items) {
        byte[][] keys = putKeys(items, false), values = putKeys(items, true);
        long[] expires = putExpiries(items);
        PreparedBatch result = new PreparedBatch();
        resource.call(h -> {
            result.resource.handle = nBulkWrite(h, 2, keys, values, expires);
            return null;
        });
        return result;
    }
    private AtomicInfo atomic(int operation, byte[] key, byte[] value, long expected, long expires,
                              long delta, long minimum, long maximum, long timeout,
                              boolean... forceReliable) {
        boolean durable = force(forceReliable);
        positive(expires);
        positive(timeout);
        long[] input = {expected, expires, delta, minimum, maximum, timeout}, output = new long[5];
        AtomicInfo result = new AtomicInfo();
        resource.call(h -> {
            nAtomic(h, operation, key, value, input, output, durable);
            return null;
        });
        result.assign(output);
        return result;
    }
    public AtomicInfo holdHistory(byte[] name) {
        return atomic(4, name, null, -1, 0, 0, 0, 0, 0);
    }
    public AtomicInfo holdHistory(byte[] name, long fromVersion) {
        return atomic(4, name, null, fromVersion, 0, 0, 0, 0, 0);
    }
    public AtomicInfo releaseHistory(byte[] name) {
        return atomic(5, name, null, 0, 0, 0, 0, 0, 0);
    }
    public HistoryInfo historyInfo() {
        long[] output = new long[8];
        resource.call(h -> {
            nFeatureAdminEx(h, 8, new byte[0][], new long[0], null, output);
            return null;
        });
        return new HistoryInfo(output);
    }
    public Watch watch() {
        return watch(-1, new byte[0], new WatchOptions());
    }
    public Watch watch(long fromVersion, byte[] prefix, WatchOptions options) {
        if (prefix == null || options == null)
            throw new IllegalArgumentException("Invalid watch arguments");
        Watch watch = new Watch();
        long[] settings = options.nativeValues();
        watch.resource.handle = resource.call(h -> nWatchOpen(h, fromVersion, prefix, settings));
        return watch;
    }
    public AtomicInfo compareExchange(byte[] key, long expected, byte[] value) {
        return compareExchange(key, expected, value, 0, 0);
    }
    public AtomicInfo compareExchange(byte[] key, long expected, byte[] value, long expires,
                                      long timeout, boolean... forceReliable) {
        return atomic(0, key, value, expected, expires, 0, 0, 0, timeout, forceReliable);
    }
    public AtomicInfo putIfAbsent(byte[] key, byte[] value) {
        return compareExchange(key, 0, value);
    }
    public AtomicInfo deleteIfVersion(byte[] key, long expected, boolean... forceReliable) {
        return atomic(1, key, null, expected, 0, 0, 0, 0, 0, forceReliable);
    }
    public AtomicInfo putIfChanged(byte[] key, byte[] value, boolean... forceReliable) {
        return atomic(2, key, value, 0, 0, 0, 0, 0, 0, forceReliable);
    }
    public AtomicInfo addInt64(byte[] key, long delta) {
        return addInt64(key, delta, Long.MIN_VALUE, Long.MAX_VALUE, 0);
    }
    public AtomicInfo addInt64(byte[] key, long delta, long minimum, long maximum, long timeout,
                               boolean... forceReliable) {
        return atomic(3, key, null, 0, 0, delta, minimum, maximum, timeout, forceReliable);
    }
    public AtomicInfo addInt64Fields(Int64FieldUpdate[] items) {
        return addInt64Fields(items, 0);
    }
    public AtomicInfo addInt64Fields(Int64FieldUpdate[] items, long timeout,
                                      boolean... forceReliable) {
        if (items == null || items.length == 0 || items.length > 256)
            throw new IllegalArgumentException("Expected 1..256 field updates");
        positive(timeout);
        boolean reliable = force(forceReliable);
        byte[][] keys = new byte[items.length][];
        long[] values = new long[items.length * 4], output = new long[5];
        AtomicInfo result = new AtomicInfo();
        for (int i = 0; i < items.length; i++) {
            Int64FieldUpdate item = items[i];
            if (item == null)
                throw new IllegalArgumentException("Null item");
            keys[i] = item.key;
            values[i * 4] = item.offset;
            values[i * 4 + 1] = item.delta;
            values[i * 4 + 2] = item.minimum;
            values[i * 4 + 3] = item.maximum;
        }
        resource.call(h -> {
            nAddInt64Fields(h, keys, values, timeout, output, reliable);
            return null;
        });
        result.assign(output);
        return result;
    }
    public long putMany(Put[] items, boolean... forceReliable) {
        try (Transaction tx = begin()) {
            tx.putMany(items);
            return tx.commit(forceReliable);
        }
    }
    public long deleteMany(byte[][] keys, boolean... forceReliable) {
        try (Transaction tx = begin()) {
            tx.deleteMany(keys);
            return tx.commit(forceReliable);
        }
    }
    public long putBatch(PreparedBatch batch, boolean... forceReliable) {
        try (Transaction tx = begin()) {
            tx.putBatch(batch);
            return tx.commit(forceReliable);
        }
    }
    public byte[] hashGet(byte[] scope, byte[] key) {
        return resource.call(h -> nHashGet(h, scope, key));
    }
    /** Read-only SELECT (53) / EXPLAIN (54) on a fresh snapshot; no transaction or dependencies. */
    public CommandResult selectRaw(int operation, byte[] value) {
        byte[] request = FeatureWire.request(operation, new byte[0], null, value, 0, 0, 256, 0);
        return resource.call(h -> FeatureWire.result(nSelect(h, request)));
    }
    public CommandResult execute(int operation, byte[] scope, byte[] key, byte[] value,
                                 boolean... forceReliable) {
        return execute(operation, scope, key, value, 0, 0, 256, 0, forceReliable);
    }
    public CommandResult execute(int operation, byte[] scope, byte[] key, byte[] value,
                                 long argument, double score, long limit, int flags,
                                 boolean... forceReliable) {
        try (Transaction tx = begin()) {
            CommandResult result =
                tx.execute(operation, scope, key, value, argument, score, limit, flags);
            result.version = tx.commit(forceReliable);
            return result;
        }
    }
    private static MaintenanceInfo featureAdmin(Resource resource, int operation, byte[][] texts,
                                                long[] values) {
        return featureAdmin(resource, operation, texts, values, null);
    }
    private static MaintenanceInfo featureAdmin(Resource resource, int operation, byte[][] texts,
                                                long[] values, MaintenanceOptions options) {
        long[] output = options == null ? new long[8] : options.output();
        long[] budget = options == null ? null : options.nativeValues();
        MaintenanceInfo info = new MaintenanceInfo();
        try {
            if (resource == null)
                nFeatureAdminEx(0, operation, texts, values, budget, output);
            else
                resource.call(h -> {
                    nFeatureAdminEx(h, operation, texts, values, budget, output);
                    return null;
                });
        } finally {
            if (options != null)
                options.collect(output);
        }
        info.assign(output);
        return info;
    }
    public MaintenanceInfo defineCompoundIndex(byte[] name, byte[] prefix, IndexField[] fields,
                                               int flags) {
        return defineCompoundIndex(name, prefix, fields, flags, null);
    }
    public MaintenanceInfo defineCompoundIndex(byte[] name, byte[] prefix, IndexField[] fields,
                                               int flags, MaintenanceOptions options) {
        if (fields == null || fields.length == 0 || fields.length > 8)
            throw new IllegalArgumentException("Invalid index definition");
        long[] numbers = new long[fields.length + 2];
        numbers[0] = positive(flags);
        numbers[1] = 0;
        for (int i = 0; i < fields.length; i++) {
            if (fields[i] == null)
                throw new IllegalArgumentException("Null index field");
            numbers[i + 2] = ((long)fields[i].field << 32) | fields[i].type;
        }
        return featureAdmin(resource, 7, new byte[][] {name, prefix}, numbers, options);
    }
    public CommandResult queryIndex(byte[] name, IndexQuery query, byte[] cursor, long limit) {
        return execute(Operation.INDEX_QUERY, name, cursor,
                       (query == null ? new IndexQuery() : query).encode(), 0, 0, limit, 0);
    }
    public byte[] project(byte[] key, int[] fields) {
        if (key == null || fields == null || fields.length > 256)
            throw new IllegalArgumentException("Invalid projection");
        return resource.call(h -> nProject(h, key, fields));
    }
    public MaintenanceInfo defineIndex(byte[] name, byte[] prefix, int field, int type, int flags) {
        return defineIndex(name, prefix, field, type, flags, null);
    }
    public MaintenanceInfo defineIndex(byte[] name, byte[] prefix, int field, int type, int flags,
                                       MaintenanceOptions options) {
        return featureAdmin(resource, 0, new byte[][] {name, prefix},
                            new long[] {positive(field), positive(type), positive(flags), 0},
                            options);
    }
    public MaintenanceInfo dropIndex(byte[] name) {
        return dropIndex(name, null);
    }
    public MaintenanceInfo dropIndex(byte[] name, MaintenanceOptions options) {
        return featureAdmin(resource, 1, new byte[][] {name}, new long[] {0}, options);
    }
    public MaintenanceInfo backupIncremental(String destination, long fromVersion) {
        return backupIncremental(destination, fromVersion, null);
    }
    public MaintenanceInfo backupIncremental(String destination, long fromVersion,
                                             MaintenanceOptions options) {
        return featureAdmin(resource, 2, new byte[][] {path(destination)},
                            new long[] {fromVersion, 0}, options);
    }
    public static MaintenanceInfo recoverChain(String base, String[] archives, String destination,
                                               long targetVersion, long targetUnixMs) {
        return recoverChain(base, archives, destination, targetVersion, targetUnixMs, null);
    }
    public static MaintenanceInfo recoverChain(String base, String[] archives, String destination,
                                               long targetVersion, long targetUnixMs,
                                               MaintenanceOptions options) {
        if (archives == null || archives.length > 1024)
            throw new IllegalArgumentException();
        byte[][] paths = new byte[archives.length + 2][];
        paths[0] = path(base);
        paths[1] = path(destination);
        for (int i = 0; i < archives.length; i++)
            paths[i + 2] = path(archives[i]);
        return featureAdmin(null, 3, paths, new long[] {targetVersion, positive(targetUnixMs), 0},
                            options);
    }
    public void configure() {
        configure(new RuntimeOptions());
    }
    private static boolean force(boolean... value) {
        if (value == null || value.length > 1)
            throw new IllegalArgumentException("Expected zero or one forceReliable flag");
        return value.length == 1 && value[0];
    }
    public static final class WriteState {
        public final long version, durableVersion, pendingBytes, lastStatus;
        private WriteState(long[] v) {
            version = v[0];
            durableVersion = v[1];
            pendingBytes = v[2];
            lastStatus = v[3];
        }
    }
    public WriteState writeState() {
        return resource.call(h -> new WriteState(nWriteState(h)));
    }
    public AtomicInfo put(byte[] key, byte[] value, long expiresAtMs, boolean... forceReliable) {
        positive(expiresAtMs);
        return mutation(0, key, value, 0, 0, expiresAtMs, force(forceReliable));
    }
    public AtomicInfo put(byte[] key, byte[] value, boolean... forceReliable) {
        return put(key, value, 0, force(forceReliable));
    }
    public AtomicInfo delete(byte[] key, boolean... forceReliable) {
        return mutation(1, key, null, 0, 0, 0, force(forceReliable));
    }
    public AtomicInfo patch(byte[] key, long offset, long remove, byte[] insert,
                            boolean... forceReliable) {
        positive(offset);
        positive(remove);
        return mutation(2, key, insert, offset, remove, 0, force(forceReliable));
    }
    private AtomicInfo mutation(int kind, byte[] key, byte[] value, long offset, long remove,
                                long expires, boolean durable) {
        AtomicInfo result = new AtomicInfo();
        resource.call(h -> {
            result.assign(nMutate(h, kind, key, value, offset, remove, expires, durable));
            return null;
        });
        return result;
    }
    private static native long[] nWriteState(long handle);
    private static native long[] nMutate(long handle, int kind, byte[] key, byte[] value,
                                         long offset, long remove, long expires,
                                         boolean forceReliable);
    public WriteOptions writeOptions() {
        return resource.call(h -> {
            long[] n = nWriteOptions(h, 0, 0, 0, 0);
            WriteOptions p = new WriteOptions();
            p.mode = WriteMode.values()[(int)n[0]];
            p.periodMs = n[1];
            p.thresholdBytes = n[2];
            return p;
        });
    }
    public void configureWrite(WriteOptions p) {
        if (p == null || p.mode == null)
            throw new IllegalArgumentException("Null write options");
        positive(p.periodMs);
        positive(p.thresholdBytes);
        resource.call(h -> nWriteOptions(h, 1, p.mode.ordinal(), p.periodMs, p.thresholdBytes));
    }
    public void resetWriteOptions() {
        resource.call(h -> nWriteOptions(h, 2, 0, 0, 0));
    }
    public java.util.concurrent.CompletableFuture<Void>
    configureWriteAsync(WriteOptions p, java.util.concurrent.Executor executor) {
        if (p == null || executor == null)
            throw new IllegalArgumentException();
        WriteOptions copy = new WriteOptions();
        copy.mode = p.mode;
        copy.periodMs = p.periodMs;
        copy.thresholdBytes = p.thresholdBytes;
        return java.util.concurrent.CompletableFuture.runAsync(
            () -> configureWrite(copy), executor);
    }
    public java.util.concurrent.CompletableFuture<Void>
    resetWriteOptionsAsync(java.util.concurrent.Executor executor) {
        return java.util.concurrent.CompletableFuture.runAsync(() -> resetWriteOptions(), executor);
    }
    public CommitReceipt submitPut(byte[] key, byte[] value, long expiresAtMs,
                                   boolean... forceReliable) {
        positive(expiresAtMs);
        return resource.call(
            h -> receipt(nSubmitPut(h, key, value, expiresAtMs, force(forceReliable))));
    }
    public CommitReceipt submitPut(byte[] key, byte[] value) {
        return submitPut(key, value, 0);
    }
    public void configure(RuntimeOptions options) {
        featureAdmin(resource, 5, new byte[0][], options.values());
    }
    public RuntimeInfo runtimeInfo() {
        RuntimeInfo info = new RuntimeInfo();
        long[] output = new long[8];
        resource.call(h -> {
            nFeatureAdminEx(h, 6, new byte[0][], new long[0], null, output);
            return null;
        });
        info.assign(output);
        return info;
    }
    public byte[] get(byte[] key) {
        return resource.call(h -> nGet(h, key, 0));
    }
    /**
       version, durableVersion, fileBytes, indexedKeys, storedKeys, storedValueBytes,
        deltaBytes, cacheBytes, pinnedBytes, transactionBytes, recoveredTransactions, repairedBytes.
     */
    public long[] info() {
        return resource.call(Store::nInfo);
    }
    public void sync() {
        resource.call(h -> {
            nAdmin(h, 0);
            return null;
        });
    }
    public void checkpoint() {
        resource.call(h -> {
            nAdmin(h, 1);
            return null;
        });
    }
    private static byte[] path(String path) {
        if (path == null || path.isEmpty() || path.indexOf('\0') >= 0)
            throw new IllegalArgumentException("Invalid directory");
        return path.getBytes(StandardCharsets.UTF_8);
    }
    public MaintenanceInfo compact() {
        return compact(new MaintenanceOptions());
    }
    public MaintenanceInfo compact(MaintenanceOptions options) {
        return maintain(null, 0, options);
    }
    public MaintenanceInfo backup(String destination) {
        return backup(destination, new MaintenanceOptions());
    }
    public MaintenanceInfo backup(String destination, MaintenanceOptions options) {
        return maintain(path(destination), 1, options);
    }
    public MaintenanceInfo verify() {
        return verify(new MaintenanceOptions());
    }
    public MaintenanceInfo verify(MaintenanceOptions options) {
        return maintain(null, 2, options);
    }
    public static MaintenanceInfo restore(String source, String destination) {
        return restore(source, destination, new MaintenanceOptions());
    }
    public static MaintenanceInfo restore(String source, String destination,
                                          MaintenanceOptions options) {
        byte[] from = path(source), to = path(destination);
        long[] limits = options.nativeValues(), output = options.output();
        MaintenanceInfo info = new MaintenanceInfo();
        try {
            nRestore(from, to, limits, output);
        } finally {
            options.collect(output);
        }
        info.assign(output);
        return info;
    }
    private MaintenanceInfo maintain(byte[] destination, int operation,
                                     MaintenanceOptions options) {
        // Allocate the result before any native publication can succeed.
        long[] limits = options.nativeValues(), output = options.output();
        MaintenanceInfo info = new MaintenanceInfo();
        try {
            resource.call(h -> {
                nMaintenance(h, destination, operation, limits, output);
                return null;
            });
        } finally {
            options.collect(output);
        }
        info.assign(output);
        return info;
    }
    public ExpiryInfo purgeExpired() {
        return purgeExpired(new ExpiryOptions());
    }
    public ExpiryInfo purgeExpired(ExpiryOptions options) {
        long[] limits = options.nativeValues(), output = new long[6];
        ExpiryInfo info = new ExpiryInfo(); // allocate before native commit
        resource.call(h -> {
            nPurgeExpired(h, limits, output);
            return null;
        });
        info.assign(output);
        return info;
    }
    public void cancelMaintenance() {
        resource.call(h -> {
            nCancelMaintenance(h);
            return null;
        });
    }
    public Transaction begin() {
        return begin(0);
    }
    public Transaction begin(long timeoutMs) {
        positive(timeoutMs);
        return resource.call(h -> new Transaction(nBegin(h, timeoutMs)));
    }
    public Snapshot snapshot() {
        return resource.call(h -> new Snapshot(nSnapshot(h)));
    }
    public void close() {
        resource.close();
    }
    public static final class Snapshot implements AutoCloseable {
        private final Resource resource;
        private Snapshot(long handle) {
            resource = new Resource(handle, 2);
        }
        public byte[] get(byte[] key) {
            return resource.call(h -> nGet(h, key, 2));
        }
        public byte[][] getMany(byte[][] keys) {
            return resource.call(h -> (byte[][])nGetMany(h, keys, 2, false));
        }
        public Entry[] getEntries(byte[][] keys) {
            return resource.call(h -> (Entry[])nGetMany(h, keys, 2, true));
        }
        public Cursor scan(byte[] begin, byte[] end, boolean reverse) {
            return resource.call(h -> new Cursor(nCursor(h, begin, end, reverse, false)));
        }
        public Cursor scan() {
            return scan(new byte[0], new byte[0], false);
        }
        public void close() {
            resource.close();
        }
    }
    public static final class Transaction implements AutoCloseable {
        public CommandResult queryIndex(byte[] name, IndexQuery query, byte[] cursor, long limit) {
            return execute(Operation.INDEX_QUERY, name, cursor,
                           (query == null ? new IndexQuery() : query).encode(), 0, 0, limit, 0);
        }
        public byte[] project(byte[] key, int[] fields) {
            IndexQuery query = new IndexQuery();
            query.fields = fields;
            return execute(Operation.RECORD_PROJECT, new byte[0], key, query.encode(), 0, 0, 256, 0)
                .value;
        }
        private final Resource resource;
        private final Object gate = new Object();
        private Transaction(long handle) {
            resource = new Resource(handle, 1);
        }
        public CommandResult execute(int operation, byte[] scope, byte[] key, byte[] value) {
            return execute(operation, scope, key, value, 0, 0, 256, 0);
        }
        public CommandResult execute(int operation, byte[] scope, byte[] key, byte[] value,
                                     long argument, double score, long limit, int flags) {
            byte[] request =
                FeatureWire.request(operation, scope, key, value, argument, score, limit, flags);
            synchronized (gate) {
                return resource.call(h -> FeatureWire.result(nExecute(h, request)));
            }
        }
        public byte[] get(byte[] key) {
            synchronized (gate) {
                return resource.call(h -> nGet(h, key, 1));
            }
        }
        public byte[][] getMany(byte[][] keys) {
            synchronized (gate) {
                return resource.call(h -> (byte[][])nGetMany(h, keys, 1, false));
            }
        }
        public Entry[] getEntries(byte[][] keys) {
            synchronized (gate) {
                return resource.call(h -> (Entry[])nGetMany(h, keys, 1, true));
            }
        }
        public void putMany(Put[] items) {
            byte[][] keys = putKeys(items, false), values = putKeys(items, true);
            long[] expires = putExpiries(items);
            synchronized (gate) {
                resource.call(h -> {
                    nBulkWrite(h, 0, keys, values, expires);
                    return null;
                });
            }
        }
        public void deleteMany(byte[][] keys) {
            synchronized (gate) {
                resource.call(h -> {
                    nBulkWrite(h, 1, keys, null, null);
                    return null;
                });
            }
        }
        public void putBatch(PreparedBatch batch) {
            if (batch == null)
                throw new IllegalArgumentException("Null batch");
            synchronized (gate) {
                resource.call(h -> batch.resource.call(b -> {
                    nPutBatch(h, b);
                    return null;
                }));
            }
        }
        public void put(byte[] key, byte[] value) {
            put(key, value, 0);
        }
        /** expiresAtMs is absolute Unix milliseconds, zero means no expiry. */
        public void put(byte[] key, byte[] value, long expiresAtMs) {
            positive(expiresAtMs);
            synchronized (gate) {
                resource.call(h -> {
                    nEdit(h, key, value, expiresAtMs, 0);
                    return null;
                });
            }
        }
        public void delete(byte[] key) {
            synchronized (gate) {
                resource.call(h -> {
                    nEdit(h, key, null, 0, 1);
                    return null;
                });
            }
        }
        public void expire(byte[] key, long expiresAtMs) {
            positive(expiresAtMs);
            synchronized (gate) {
                resource.call(h -> {
                    nEdit(h, key, null, expiresAtMs, 2);
                    return null;
                });
            }
        }
        public void patch(byte[] key, long offset, long remove, byte[] insert) {
            positive(offset);
            positive(remove);
            synchronized (gate) {
                resource.call(h -> {
                    nPatch(h, key, offset, remove, insert);
                    return null;
                });
            }
        }
        public long savepoint() {
            synchronized (gate) {
                return resource.call(h -> nSavepoint(h, 0, 0));
            }
        }
        public void rollbackTo(long id) {
            positive(id);
            synchronized (gate) {
                resource.call(h -> nSavepoint(h, id, 1));
            }
        }
        public void release(long id) {
            positive(id);
            synchronized (gate) {
                resource.call(h -> nSavepoint(h, id, 2));
            }
        }
        public void cancel() {
            resource.call(h -> {
                nCancel(h);
                return null;
            });
        }
        public long commit(boolean... forceReliable) {
            synchronized (gate) {
                boolean durable = force(forceReliable);
                return resource.call(h -> nCommit(h, durable));
            }
        }
        public CommitReceipt submit(boolean... forceReliable) {
            synchronized (gate) {
                return resource.call(h -> receipt(nSubmitTxn(h, force(forceReliable))));
            }
        }
        public Cursor scan(byte[] begin, byte[] end, boolean reverse) {
            synchronized (gate) {
                return resource.call(h -> new Cursor(nCursor(h, begin, end, reverse, true)));
            }
        }
        public Cursor scan() {
            return scan(new byte[0], new byte[0], false);
        }
        public void close() {
            resource.close();
        }
    }
    public static final class Cursor implements AutoCloseable, Iterable<Row>, Iterator<Row> {
        private final Resource resource;
        private Row pending;
        private boolean ended;
        private Cursor(long handle) {
            resource = new Resource(handle, 3);
        }
        public Iterator<Row> iterator() {
            return this;
        }
        public synchronized boolean hasNext() {
            if (pending == null && !ended) {
                pending = resource.call(Store::nNext);
                ended = pending == null;
            }
            return pending != null;
        }
        public synchronized Row next() {
            if (!hasNext())
                throw new NoSuchElementException();
            Row row = pending;
            pending = null;
            return row;
        }
        public void close() {
            resource.close();
        }
    }
    private static native void nAddInt64Fields(long handle, byte[][] keys, long[] items,
                                               long timeout, long[] output, boolean forceReliable);
    private static native byte[] nProject(long handle, byte[] key, int[] fields);
    private static native long nProjectReaderCreate(long handle, byte[][] keys, int[] fields);
    private static native long nOpen(byte[] path, boolean buffered, boolean create, long[] limits);
    private static native long nWatchOpen(long handle, long from, byte[] prefix, long[] options);
    private static native Object[] nWatchNext(long handle, long timeoutMs);
    private static native void nWatchAck(long handle, long version);
    private static native void nWatchCancel(long handle);
    private static native void nClose(long handle, int kind);
    private static native byte[] nGet(long handle, byte[] key, int kind);
    private static native long[] nInfo(long handle);
    private static native void nAdmin(long handle, int operation);
    private static native long nBegin(long handle, long timeoutMs);
    private static native long nSnapshot(long handle);
    private static native void nEdit(long handle, byte[] key, byte[] value, long expiresAtMs,
                                     int operation);
    private static native void nPatch(long handle, byte[] key, long offset, long remove,
                                      byte[] insert);
    private static native long nSavepoint(long handle, long id, int operation);
    private static native void nCancel(long handle);
    private static native long nCommit(long handle, boolean forceReliable);
    private static native long[] nWriteOptions(long handle, int action, int mode, long period,
                                               long bytes);
    private static native long nSubmitPut(long handle, byte[] key, byte[] value, long expires,
                                          boolean forceReliable);
    private static native long nSubmitTxn(long handle, boolean forceReliable);
    private static native long[] nCommitResult(long handle, long timeout, boolean poll);
    private static native void nCommitCancel(long handle);
    private static native long nCursor(long handle, byte[] begin, byte[] end, boolean reverse,
                                       boolean transaction);
    private static native Row nNext(long handle);
    private static native void nMaintenance(long handle, byte[] destination, int operation,
                                            long[] limits, long[] output);
    private static native void nRestore(byte[] source, byte[] destination, long[] limits,
                                        long[] output);
    private static native void nCancelMaintenance(long handle);
    private static native byte[] nHashGet(long handle, byte[] scope, byte[] key);
    private static native byte[] nExecute(long handle, byte[] request);
    private static native byte[] nSelect(long handle, byte[] request);
    private static native long nOpenAt(byte[] root, byte[] name, boolean buffered, boolean create,
                                       long[] limits);
    private static native byte[] nPathAt(byte[] root, byte[] name);
    private static native Entry nGetEntry(long handle, byte[] key, int kind);
    private static native Object[] nGetMany(long handle, byte[][] keys, int kind, boolean metadata);
    private static native byte[] nKeyGet(long handle);
    private static native long nPrepareKey(long handle, byte[] key);
    private static native long nKeyGetInto(long handle, byte[] destination);
    private static native long nKeyGetIntoDirect(long handle, ByteBuffer destination, int offset,
                                                 int size);
    private static native long nBulkWrite(long handle, int operation, byte[][] keys,
                                          byte[][] values, long[] expires);
    private static native void nPutBatch(long txn, long batch);
    private static native void nAtomic(long handle, int operation, byte[] key, byte[] value,
                                       long[] input, long[] output, boolean forceReliable);
    private static native void nFeatureAdminEx(long handle, int operation, byte[][] text,
                                               long[] values, long[] budget, long[] output);
    private static native void nPurgeExpired(long handle, long[] limits, long[] output);
}
