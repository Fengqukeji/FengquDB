using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Google.Protobuf;
using FengquDB;
using FengquDB.Examples;
using OldPlayer = FengquDB.Examples.V1.PlayerSave;
using Player = FengquDB.Examples.V2.PlayerSave;

static class Program {
    static ulong sink;
    static byte[] Bytes(string value) => Encoding.UTF8.GetBytes(value);
    static void Check(bool value) {
        if (!value)
            throw new InvalidOperationException("Generated C# model check failed");
    }
    static void Rewrite(string root) {
        using var db =
            Store.OpenAt(Path.Combine(root, "db"), "main", new StoreOptions { Create = false });
        db.ConfigureWrite(
            new StoreWriteOptions { Mode = StoreWriteMode.Reliable }); // Synchronous example.

        var player = OldPlayer.Parser.ParseFrom(db.Get(Bytes("player/42"))!);
        Check(player.HasPlayerId && player.PlayerId == 42 && player.HasCoins &&
              player.Coins == -17);
        Check(player.DisplayName == "\u73a9\u5bb6\u2600" && player.Tags.Contains("\u6cd5\u5e08"));
        Check(player.Inventory.Items[0].ItemId == 7 && player.Inventory.Items[0].Count == 3);
        Check(player.Counters["wins"] == -2 && player.AvatarToken[1] == 255);
        player.Coins++;
        player.DisplayName = "v1-C#";
        player.Tags.Add("csharp");
        db.PutIfChanged(Bytes("player/42"), player.ToByteArray());
        var zero = OldPlayer.Parser.ParseFrom(db.Get(Bytes("player/0"))!);
        Check(zero.HasPlayerId && zero.PlayerId == 0 && zero.HasDisplayName &&
              zero.DisplayName == "");
        Check(zero.HasCoins && zero.Coins == 0);
        db.PutIfChanged(
            Bytes("player/7"),
            new OldPlayer { PlayerId = 7, DisplayName = "worker", Coins = 0 }.ToByteArray());
        db.Verify();
        Console.WriteLine("PASS generated C# v1 read/edit/write with unknown-field preservation");
    }
    static void Inspect(string root) {
        using var saves = new PlayerSaves(root, create: false);
        var player = saves.Load(42)!;
        Check(player.HasRealm && player.Realm == "\u4e9a\u670d" && player.Coins == -15);
        Check(player.Milestones.SequenceEqual(new long[] { -1, 0, 128 }));
        saves.Save(new Player { PlayerId = 99, DisplayName = "typed", Coins = 0, Realm = "next" });
        Check(saves.Load(99)!.Realm == "next");
        Console.WriteLine("PASS typed PlayerSaves helper and v2 fields after old writers");
    }
#if FQDB_UNITY_SMOKE
    static void UnitySmoke(string root) {
        UnityEngine.Application.persistentDataPath = root;
        var component = new TypedFengquSave();
        var flags =
            System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
        typeof(TypedFengquSave).GetMethod("Awake", flags)!.Invoke(component, null);
        Check(component.LoadPlayer()!.PlayerId == 42);
        component.SavePlayer(new Player { PlayerId = 101, DisplayName = "unity-example", Coins = 0,
                                          Realm = "desktop-stub" });
        typeof(TypedFengquSave).GetMethod("OnDestroy", flags)!.Invoke(component, null);
        bool rejected = false;
        try {
            component.LoadPlayer();
        } catch (InvalidOperationException) {
            rejected = true;
        }
        Check(rejected);
        Console.WriteLine("PASS Unity component compiled with desktop stubs, lifecycle, typed " +
                          "save and closed guard");
    }
#endif
    static ulong Score(byte[] value) => (ulong)(value[0] + value[value.Length - 1]);
    static ulong Score(Player value) => value.PlayerId + (ulong)value.AvatarToken.Length;
    static void Measure(string name, int count, int units, Func<ulong> operation) {
        for (int i = 0; i < Math.Min(count, 10000); ++i)
            sink ^= operation();
        var samples = new List<double>((count + 63) / 64);
        ulong checksum = 0;
        var timer = Stopwatch.StartNew();
        for (int i = 0; i < count; ++i) {
            if ((i & 63) == 0) {
                var start = Stopwatch.GetTimestamp();
                checksum += operation();
                samples.Add((Stopwatch.GetTimestamp() - start) * 1000000.0 / Stopwatch.Frequency);
            } else
                checksum += operation();
        }
        var seconds = timer.Elapsed.TotalSeconds;
        samples.Sort();
        sink ^= checksum;
        Console.WriteLine(JsonSerializer.Serialize(
            new { language = "csharp", workload = name, calls = count, units_per_call = units,
                  ops_per_second = count * (double)units / seconds,
                  p99_us = samples[(int)((samples.Count - 1) * .99)], checksum }));
    }
    static void Bench(string root, string fixture, int count) {
        var payload = File.ReadAllBytes(fixture);
        var model = Player.Parser.ParseFrom(payload);
        Check(model.PlayerId == 42 && model.AvatarToken.Length == 4096);
        using var db = Store.OpenAt(Path.Combine(root, "db"), "main");
        db.ConfigureWrite(
            new StoreWriteOptions { Mode = StoreWriteMode.Reliable }); // Synchronous example.

        var keys = Enumerable.Range(0, 32).Select(i => Bytes("bench/" + i)).ToArray();
        db.PutMany(keys.Select(key => new StorePut(key, payload)).ToArray());
        using var prepared = db.PrepareKey(keys[0]);
        var reuse = new byte[payload.Length];
        Measure("serialize", count, 1, () => Score(model.ToByteArray()));
        Measure("parse", count, 1, () => Score(Player.Parser.ParseFrom(payload)));
        Measure("get_copy", count, 1, () => Score(db.Get(keys[0])!));
        Measure("prepared_get_into", count, 1, () => {
            Check(prepared.GetInto(reuse) == (ulong)payload.Length);
            return Score(reuse);
        });
        Measure("get_parse", count, 1, () => Score(Player.Parser.ParseFrom(db.Get(keys[0])!)));
        Measure("get_many_32", Math.Max(8192, count / 16), 32, () => {
            ulong result = 0;
            foreach (var bytes in db.GetMany(keys))
                result += Score(bytes!);
            return result;
        });
        foreach (var width in new[] { 1, 32 }) {
            using var reader = db.PrepareReader(keys.Take(width).ToArray());
            StoreReadAction<ulong> action = batch => {
                Check(batch.Count == width);
                ulong sum = 0;
                for (int i = 0; i < batch.Count; ++i) {
                    var row = batch[i];
                    Check(row.Found);
                    var bytes = row.Value;
                    sum += (ulong)(bytes[0] + bytes[bytes.Length - 1]);
                }
                return sum;
            };
            if (width == 1)
                Measure("shared_reader_parse", count, 1,
                        () => reader.Read(batch => Score(Player.Parser.ParseFrom(batch[0].Value))));
            Measure(width == 1 ? "shared_reader" : "shared_reader_32",
                    width == 1 ? count : Math.Max(8192, count / 16), width,
                    () => reader.Read(action));
        }
        db.Verify();
        Check(db.Get(keys[31])!.SequenceEqual(payload));
        GC.KeepAlive(sink);
    }
    static void Main(string[] args) {
        var library = Path.GetFullPath(args[2]);
        NativeLibrary.SetDllImportResolver(typeof(Store).Assembly,
                                           (name, assembly, flags) =>
                                               name == "fengqudb" ? NativeLibrary.Load(library)
                                                                  : IntPtr.Zero);
        if (args[0] == "rewrite")
            Rewrite(args[1]);
        else if (args[0] == "inspect")
            Inspect(args[1]);
#if FQDB_UNITY_SMOKE
        else if (args[0] == "unity-smoke")
            UnitySmoke(args[1]);
#endif
        else if (args[0] == "bench")
            Bench(args[1], args[3], int.Parse(args[4]));
        else
            throw new ArgumentException("Unknown mode");
    }
}
