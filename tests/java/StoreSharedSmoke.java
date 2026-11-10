import io.fengqudb.Store;
import java.nio.*;
import java.nio.file.Paths;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
public final class StoreSharedSmoke {
    static byte[] b(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }
    static void check(boolean ok) {
        if (!ok)
            throw new AssertionError("shared JNI");
    }
    static String text(ByteBuffer value) {
        byte[] bytes = new byte[value.remaining()];
        value.duplicate().get(bytes);
        return new String(bytes, StandardCharsets.UTF_8);
    }
    static void dropShared(Store db) {
        try (Store.SharedValue value = db.getShared(b("gc"))) {
            ByteBuffer alias = value.buffer().slice();
            check(alias.get(0) == 7 && (!value.isZeroCopy() || db.info()[8] > 32768));
        }
    }
    public static void main(String[] args) throws Exception {
        for (boolean segmented : new boolean[] {false, true}) {
            Store.Options options = new Store.Options();
            options.buffered = true;
            options.segmented = segmented;
            Store db = Store.openAt(args[0], segmented ? "segments" : "shared", options);
            db.putMany(new Store.Put[] {new Store.Put(b("a"), b("original")),
                                        new Store.Put(b("e"), b(""))});
            Store.SharedValue shared = db.getShared(b("a"));
            ByteBuffer view = shared.buffer();
            check(view.isDirect() && view.isReadOnly() && text(view).equals("original"));
            check(shared.isZeroCopy() != Boolean.getBoolean("io.fengqudb.shared.copy"));
            try {
                view.put(0, (byte)0);
                throw new AssertionError("writable");
            } catch (ReadOnlyBufferException expected) {
            }
            ByteBuffer derived = view.duplicate();
            derived.position(1).limit(5);
            derived = derived.slice();
            Store.PreparedReader reader =
                db.prepareReader(new byte[][] {b("a"), b("missing"), b("e"), b("a")});
            Store.SharedBatch held = reader.readShared();
            ByteBuffer old = held.get(0).buffer().duplicate();
            check(held.size() == 4 && held.get(1) == null && held.get(2).buffer().remaining() == 0);
            try (Store.Transaction tx = db.begin()) {
                tx.patch(b("a"), 0, 8, b("new"));
                tx.delete(b("e"));
                tx.commit();
            }
            db.checkpoint();
            db.compact();
            shared.close();
            held.close();
            reader.release();
            view = null;
            shared = null;
            held = null;
            System.gc();
            Thread.sleep(30);
            check(text(derived).equals("rigi") && text(old).equals("original"));
            try (Store.SharedBatch fresh = reader.readShared()) {
                check(text(fresh.get(0).buffer()).equals("new") && fresh.get(2) == null);
            }
            check(db.getShared(b("missing")) == null);
            try (Store.PreparedKey key = db.prepareKey(b("a"));
                 Store.SharedValue value = key.getShared()) {
                check(text(value.buffer()).equals("new"));
            }
            db.close();
            try (Store.SharedBatch fresh = reader.readShared()) {
                check(text(fresh.get(0).buffer()).equals("new"));
            }
            reader.close();
            check(text(old).equals("original"));
            derived = null;
            old = null;
        }
        Store.Options options = new Store.Options();
        options.buffered = true;
        options.pinnedBytes = 65536;
        try (Store db = Store.openAt(args[0], "limited", options)) {
            byte[] large = new byte[131072];
            Arrays.fill(large, (byte)7);
            db.putIfChanged(b("big"), large);
            check(Arrays.equals(db.get(b("big")), large));
            try {
                db.getShared(b("big"));
                throw new AssertionError("limit ignored");
            } catch (Store.DbException e) {
                check(e.code == 9);
            }
        }
        try (Store db = Store.openAt(args[0], "gc", new Store.Options())) {
            byte[] bytes = new byte[32768];
            Arrays.fill(bytes, (byte)7);
            db.putIfChanged(b("gc"), bytes);
            long baseline = db.info()[8];
            dropShared(db);
            long deadline = System.nanoTime() + 5000000000L;
            while (db.info()[8] != baseline && System.nanoTime() < deadline) {
                System.gc();
                Thread.sleep(10);
            }
            check(db.info()[8] == baseline);
        }
        System.gc();
        Thread.sleep(100);
        System.out.println("PASS JNI shared read-only direct buffers, derived lifetime, COW, " +
                           "missing/empty and pin fallback");
    }
}
