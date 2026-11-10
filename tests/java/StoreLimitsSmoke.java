import io.fengqudb.Store;
import java.nio.charset.StandardCharsets;
import java.nio.file.Paths;
import java.util.Arrays;

public final class StoreLimitsSmoke {
    static byte[] b(String value) {
        return value.getBytes(StandardCharsets.UTF_8);
    }
    static void check(boolean value) {
        if (!value)
            throw new AssertionError("Java unsigned version check failed");
    }
    public static void main(String[] args) throws Exception {
        String path = Paths.get(args[0], "main").toString();
        String base = Paths.get(args[0], "base").toString();
        String archive = Paths.get(args[0], "inc").toString();
        String restored = Paths.get(args[0], "copy").toString();
        Store.Options options = new Store.Options();
        options.create = false;
        long target;
        try (Store db = new Store(path, options)) {
            check(db.info()[0] == Long.MAX_VALUE - 2);
            long first = db.putIfChanged(b("key"), b("A")).version;
            check(first == Long.MAX_VALUE - 1);
            check(db.holdHistory(b("numeric"), first).version == Long.MAX_VALUE);
            try (Store.Watch watch = db.watch(Long.MAX_VALUE, b("key"), new Store.WatchOptions())) {
                long high = db.compareExchange(b("key"), first, b("B")).version;
                check(high == Long.MIN_VALUE && db.getEntry(b("key")).version == high);
                Store.Changes changes = watch.next();
                check(changes.version == high && changes.rows.length == 1);
                watch.ack(high);
                check(watch.next() == null);
                // All version inputs accept the exact high-bit pattern returned
                // by native metadata, even though it is a negative Java long.
                check(db.holdHistory(b("numeric"), high).version == Long.MIN_VALUE + 1);
                long from = db.backup(base).snapshotVersion;
                check(from == Long.MIN_VALUE + 1);
                target = db.compareExchange(b("key"), high, b("C")).version;
                check(target == Long.MIN_VALUE + 2);
                db.backupIncremental(archive, from);
                try (Store.Watch resumed = db.watch(high, b("key"), new Store.WatchOptions())) {
                    Store.Changes last = resumed.next();
                    check(last.version == target && Arrays.equals(last.rows[0].value, b("C")));
                    resumed.ack(target);
                }
                check(watch.next().version == target);
                watch.ack(target);
            }
            db.verify();
        }
        Store.recoverChain(base, new String[] {archive}, restored, target, 0);
        try (Store copy = new Store(restored, options)) {
            check(copy.info()[0] == target && Arrays.equals(copy.get(b("key")), b("C")));
            copy.verify();
        }
        System.out.println("PASS Java unsigned version boundary, CAS, watch/ack, history, " +
                           "incremental backup and PITR");
    }
}
