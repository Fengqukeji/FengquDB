import io.fengqudb.Store;
import java.nio.charset.StandardCharsets;
public final class WritePolicySmoke {
    static byte[] b(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }
    static void check(boolean value) {
        if (!value)
            throw new AssertionError("write policy Java");
    }
    static void projection(String path) {
        try (Store db = new Store(path)) {
            byte[] original = new byte[] {9, 7, 0, 0, 0, 0, 0, 0, 0, 18, 4, 98, 111, 100, 121};
            db.put(b("project"), original);
            check(db.project(b("project"), new int[] {1, 1}).length == 9 &&
                  db.project(b("project"), new int[0]).length == 0);
            try (Store.PreparedReader reader = db.prepareProjectReader(
                     new byte[][] {b("project"), b("missing")}, new int[] {1});
                 Store.SharedBatch held = reader.readShared()) {
                java.nio.ByteBuffer alias = held.get(0).buffer();
                check(alias.remaining() == 9 && alias.get(1) == 7 && held.get(1) == null);
                db.put(b("project"), new byte[] {9, 8, 0, 0, 0, 0, 0, 0, 0});
                try (Store.SharedBatch fresh = reader.readShared()) {
                    check(fresh.get(0).buffer().get(1) == 8);
                }
                reader.release();
                check(alias.get(1) == 7);
            }
        }
    }
    public static void main(String[] args) {
        check(new Store.WriteOptions().periodMs == 50 &&
              new Store.WriteOptions().thresholdBytes == 1048576);
        try (Store db = new Store(args[0])) {
            Store.WriteOptions p = db.writeOptions();
            check(p.mode == Store.WriteMode.PERFORMANCE && p.periodMs == 50 &&
                  p.thresholdBytes == 1048576);
            p.periodMs = 5000;
            db.configureWrite(p);
            projection(args[0] + "-projection");
            db.put(b("field-a"), java.nio.ByteBuffer.allocate(8).order(java.nio.ByteOrder.LITTLE_ENDIAN).putLong(100).array());
            db.put(b("field-b"), java.nio.ByteBuffer.allocate(8).order(java.nio.ByteOrder.LITTLE_ENDIAN).putLong(200).array());
            Store.AtomicInfo fields = db.addInt64Fields(new Store.Int64FieldUpdate[] {
                new Store.Int64FieldUpdate(b("field-a"), 0, -10, 0, 1000),
                new Store.Int64FieldUpdate(b("field-b"), 0, 10, 0, 1000)}, 0, true);
            check(fields.changed && fields.value == 210 && db.writeState().durableVersion >= fields.version);
            long fieldVersion = db.info()[0];
            try {
                db.addInt64Fields(new Store.Int64FieldUpdate[] {
                    new Store.Int64FieldUpdate(b("field-a"), 0, -1),
                    new Store.Int64FieldUpdate(b("field-b"), 0, -211, 0, 1000)});
                throw new AssertionError("invalid field update succeeded");
            } catch (Store.DbException expected) {}
            check(db.info()[0] == fieldVersion && java.nio.ByteBuffer.wrap(db.get(b("field-a"))).order(java.nio.ByteOrder.LITTLE_ENDIAN).getLong() == 90);
            try (Store.Transaction tx = db.begin()) {
                tx.put(b("tx"), b("value"));
                long version = tx.commit();
                check(db.get(b("tx"))[0] == 'v' && db.writeState().durableVersion < version);
                try (Store.Transaction forced = db.begin()) {
                    forced.put(b("forced-tx"), b("yes"));
                    long v = forced.commit(true);
                    check(db.writeState().durableVersion >= v);
                }
                db.put(b("paid"), b("yes"), true);
                check(db.writeState().durableVersion >= version);
                db.patch(b("paid"), 0, 3, b("new"), true);
                db.delete(b("paid"), true);
                check(db.get(b("paid")) == null);
                try (Store.Transaction next = db.begin()) {
                    next.put(b("async"), b("yes"));
                    try (Store.CommitReceipt r = next.submit(true)) {
                        check(r.await() > 0);
                    }
                }
            }
            p.periodMs = 5000;
            p.thresholdBytes = 4096;
            db.configureWriteAsync(p, java.util.concurrent.ForkJoinPool.commonPool()).join();
            Store.CommitReceipt[] receipts = new Store.CommitReceipt[4];
            byte[] data = new byte[1024];
            java.util.Arrays.fill(data, (byte)'a');
            for (int i = 0; i < 4; ++i)
                receipts[i] = db.submitPut(b("k" + i), data);
            java.util.Arrays.fill(data, (byte)'b');
            for (Store.CommitReceipt r : receipts) {
                check(r.await(3000) > 0);
                r.close();
            }
            check(db.get(b("k0"))[0] == 'a');
            p.mode = Store.WriteMode.RELIABLE;
            p.periodMs = 37;
            p.thresholdBytes = 32768;
            db.configureWrite(p);
            try (Store.Transaction tx = db.begin()) {
                tx.put(b("strict"), b("yes"));
                tx.commit();
            }
        }
        try (Store db = new Store(args[0])) {
            Store.WriteOptions p = db.writeOptions();
            check(p.mode == Store.WriteMode.RELIABLE && p.periodMs == 37 &&
                  p.thresholdBytes == 32768);
            db.resetWriteOptionsAsync(java.util.concurrent.ForkJoinPool.commonPool()).join();
            p = db.writeOptions();
            check(p.mode == Store.WriteMode.PERFORMANCE && p.periodMs == 50 &&
                  p.thresholdBytes == 1048576);
            try (Store.CommitReceipt r = db.submitPut(b("reset"), b("yes"))) {
                db.sync();
                check(r.await() > 0);
            }
        }
        try (Store db = new Store(args[0])) { check(db.writeOptions().periodMs == 50); }
        System.out.println("PASS Java periodic/strict configuration, receipts and restart");
    }
}
