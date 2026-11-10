import io.fengqudb.Store;
import io.fengqudb.examples.v1.PlayerV1Schema;
import io.fengqudb.examples.v2.PlayerV2Schema;
import java.nio.charset.StandardCharsets;
import java.nio.ByteBuffer;
import java.nio.file.*;
import java.util.*;
import java.util.function.LongSupplier;

public final class PlayerStoreExample {
    static volatile long sink;
    static byte[] bytes(String value) {
        return value.getBytes(StandardCharsets.UTF_8);
    }
    static void check(boolean value) {
        if (!value)
            throw new AssertionError("Generated Java model check failed");
    }
    static void rewrite(String root) throws Exception {
        Store.Options options = new Store.Options();
        options.create = false;
        try (Store db = Store.openAt(Paths.get(root, "db").toString(), "main", options)) {
            Store.WriteOptions write = new Store.WriteOptions();
            write.mode = Store.WriteMode.RELIABLE;
            db.configureWrite(write);
            PlayerV1Schema.PlayerSave player =
                PlayerV1Schema.PlayerSave.parseFrom(db.get(bytes("player/42")));
            check(player.hasPlayerId() && player.getPlayerId() == 42 && player.hasCoins() &&
                  player.getCoins() == -16);
            check(player.getDisplayName().equals("v1-C#") &&
                  player.getTagsList().contains("csharp"));
            check(player.getInventory().getItems(0).getItemId() == 7 &&
                  player.getCountersOrThrow("wins") == -2);
            check((player.getAvatarToken().byteAt(1) & 255) == 255);
            PlayerV1Schema.PlayerSave changed = player.toBuilder()
                                                    .setCoins(player.getCoins() + 1)
                                                    .setDisplayName("v1-Java")
                                                    .addTags("java")
                                                    .build();
            db.putIfChanged(bytes("player/42"), changed.toByteArray());
            PlayerV1Schema.PlayerSave zero =
                PlayerV1Schema.PlayerSave.parseFrom(db.get(bytes("player/0")));
            check(zero.hasPlayerId() && zero.getPlayerId() == 0 && zero.hasCoins() &&
                  zero.getCoins() == 0);
            check(zero.hasDisplayName() && zero.getDisplayName().isEmpty());
            db.verify();
        }
        System.out.println(
            "PASS generated Java v1 read/edit/write with unknown-field preservation");
    }
    static long score(byte[] value) {
        return (value[0] & 255) + (value[value.length - 1] & 255);
    }
    static long score(PlayerV2Schema.PlayerSave value) {
        return value.getPlayerId() + value.getAvatarToken().size();
    }
    static void measure(String name, int count, int units, LongSupplier operation) {
        for (int i = 0; i < Math.min(count, 10000); ++i)
            sink ^= operation.getAsLong();
        double[] samples = new double[(count + 63) / 64];
        int sampled = 0;
        long checksum = 0, started = System.nanoTime();
        for (int i = 0; i < count; ++i) {
            if ((i & 63) == 0) {
                long before = System.nanoTime();
                checksum += operation.getAsLong();
                samples[sampled++] = (System.nanoTime() - before) / 1000.0;
            } else
                checksum += operation.getAsLong();
        }
        double seconds = (System.nanoTime() - started) / 1e9;
        Arrays.sort(samples);
        sink ^= checksum;
        System.out.printf(
            Locale.ROOT,
            "{\"language\":\"java\",\"workload\":\"%s\",\"calls\":%d,\"units_per_"
                + "call\":%d,\"ops_per_second\":%.3f,\"p99_us\":%.3f,\"checksum\":%d}%n",
            name, count, units, count * (double)units / seconds,
            samples[(int)((samples.length - 1) * .99)], checksum);
    }
    static void bench(String root, String fixture, int count) throws Exception {
        byte[] payload = Files.readAllBytes(Paths.get(fixture));
        PlayerV2Schema.PlayerSave model = PlayerV2Schema.PlayerSave.parseFrom(payload);
        check(model.getPlayerId() == 42 && model.getAvatarToken().size() == 4096);
        try (Store db = Store.openAt(Paths.get(root, "db").toString(), "main")) {
            Store.WriteOptions write = new Store.WriteOptions();
            write.mode = Store.WriteMode.RELIABLE;
            db.configureWrite(write);
            byte[][] keys = new byte[32][];
            Store.Put[] rows = new Store.Put[32];
            for (int i = 0; i < 32; ++i) {
                keys[i] = bytes("bench/" + i);
                rows[i] = new Store.Put(keys[i], payload);
            }
            db.putMany(rows);
            try (Store.PreparedKey prepared = db.prepareKey(keys[0])) {
                byte[] reuse = new byte[payload.length];
                measure("serialize", count, 1, () -> score(model.toByteArray()));
                measure("parse", count, 1, () -> {
                    try {
                        return score(PlayerV2Schema.PlayerSave.parseFrom(payload));
                    } catch (com.google.protobuf.InvalidProtocolBufferException e) {
                        throw new IllegalStateException(e);
                    }
                });
                measure("get_copy", count, 1, () -> score(db.get(keys[0])));
                measure("prepared_get_into", count, 1, () -> {
                    check(prepared.getInto(reuse) == payload.length);
                    return score(reuse);
                });
                ByteBuffer direct = ByteBuffer.allocateDirect(payload.length);
                measure("prepared_direct_buffer", count, 1, () -> {
                    check(prepared.getIntoDirect(direct) == payload.length);
                    return (direct.get(0) & 255) + (direct.get(payload.length - 1) & 255);
                });
                measure("get_parse", count, 1, () -> {
                    try {
                        return score(PlayerV2Schema.PlayerSave.parseFrom(db.get(keys[0])));
                    } catch (com.google.protobuf.InvalidProtocolBufferException e) {
                        throw new IllegalStateException(e);
                    }
                });
                measure("get_many_32", Math.max(8192, count / 16), 32, () -> {
                    long result = 0;
                    for (byte[] value : db.getMany(keys))
                        result += score(value);
                    return result;
                });
            }
            db.verify();
            check(Arrays.equals(db.get(keys[31]), payload));
        }
    }
    public static void main(String[] args) throws Exception {
        if (args[0].equals("rewrite"))
            rewrite(args[1]);
        else if (args[0].equals("bench"))
            bench(args[1], args[2], Integer.parseInt(args[3]));
        else
            throw new IllegalArgumentException("Unknown mode");
    }
}
