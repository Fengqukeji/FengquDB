import io.fengqudb.Store;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Paths;
import java.util.Arrays;
import java.util.concurrent.*;

public final class StoreBufferSmoke {
    static byte[] bytes(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }
    static void check(boolean condition) {
        if (!condition)
            throw new AssertionError("Direct buffer check failed");
    }
    static void closed(Runnable call) {
        try {
            call.run();
        } catch (IllegalStateException expected) {
            return;
        }
        throw new AssertionError("Closed key accepted");
    }
    static byte[] contents(ByteBuffer buffer) {
        byte[] data = new byte[buffer.capacity()];
        buffer.duplicate().clear().get(data);
        return data;
    }
    public static void main(String[] args) throws Exception {
        Store.Options options = new Store.Options();
        options.buffered = true;
        options.segmented = true;
        Store db = Store.openAt(Paths.get(args[0], "db").toString(), "main", options);
        try {
            db.putIfChanged(bytes("key"), bytes("abc"));
            Store.PreparedKey key = db.prepareKey(bytes("key"));
            try {
                ByteBuffer buffer = ByteBuffer.allocateDirect(12);
                for (int i = 0; i < buffer.capacity(); ++i)
                    buffer.put(i, (byte)'?');
                buffer.position(2).limit(7);
                check(key.getIntoDirect(buffer) == 3);
                check(buffer.position() == 2 && buffer.limit() == 7);
                check(Arrays.equals(contents(buffer), bytes("??abc???????")));
                ByteBuffer slice = buffer.slice();
                check(key.getIntoDirect(slice) == 3 && slice.position() == 0 && slice.limit() == 5);
                for (ByteBuffer invalid :
                     new ByteBuffer[] {null, ByteBuffer.allocate(32), buffer.asReadOnlyBuffer()}) {
                    try {
                        key.getIntoDirect(invalid);
                        throw new AssertionError("Invalid accepted");
                    } catch (IllegalArgumentException expected) {
                    }
                }
                buffer.position(4).limit(6);
                byte[] before = contents(buffer);
                try {
                    key.getIntoDirect(buffer);
                    throw new AssertionError("Small accepted");
                } catch (Store.DbException expected) {
                    check(expected.code == 16);
                }
                check(Arrays.equals(before, contents(buffer)) && buffer.position() == 4 &&
                      buffer.limit() == 6);
                db.putIfChanged(bytes("key"), new byte[0]);
                check(key.getIntoDirect(ByteBuffer.allocateDirect(0)) == 0);
                check(key.getIntoDirect(buffer) == 0 && Arrays.equals(before, contents(buffer)));
                db.deleteMany(new byte[][] {bytes("key")});
                check(key.getIntoDirect(buffer) == -1 && Arrays.equals(before, contents(buffer)));
                db.putIfChanged(bytes("key"), bytes("new"));
                buffer.clear();
                db.checkpoint();
                db.compact();
                check(key.getIntoDirect(buffer) == 3 && buffer.get(0) == 'n');
                db.close();
                check(key.getIntoDirect(buffer) == 3); // Native child retains parent.
                key.close();
                closed(() -> key.getIntoDirect(buffer));
            } finally {
                key.close();
            }
        } finally {
            db.close();
        }

        // Each reader owns its buffer. close competes with native reads while
        // Java's resource lease must retain the handle for in-flight calls.
        try (Store parent = Store.openAt(Paths.get(args[0], "db").toString(), "race", options)) {
            byte[] payload = new byte[65536];
            Arrays.fill(payload, (byte)73);
            parent.putIfChanged(bytes("key"), payload);
            Store.PreparedKey key = parent.prepareKey(bytes("key"));
            ExecutorService pool = Executors.newFixedThreadPool(2);
            CountDownLatch started = new CountDownLatch(2);
            try {
                Callable<Integer> reads = () -> {
                    ByteBuffer target = ByteBuffer.allocateDirect(payload.length);
                    int count = 0;
                    try {
                        for (;;) {
                            check(key.getIntoDirect(target) == payload.length);
                            check(target.get(0) == 73 && target.get(payload.length - 1) == 73);
                            if (++count == 10)
                                started.countDown();
                        }
                    } catch (IllegalStateException expected) {
                        return count;
                    }
                };
                Future<Integer> first = pool.submit(reads), second = pool.submit(reads);
                check(started.await(10, TimeUnit.SECONDS));
                parent.close();
                key.close();
                check(first.get(10, TimeUnit.SECONDS) >= 10 &&
                      second.get(10, TimeUnit.SECONDS) >= 10);
                System.gc(); // Prior JNI local refs no longer need to keep buffers alive.
            } finally {
                key.close();
                pool.shutdownNow();
                check(pool.awaitTermination(10, TimeUnit.SECONDS));
            }
        }
        System.out.println(
            "PASS Java direct buffer bounds, slices, missing/empty and concurrent close");
    }
}
