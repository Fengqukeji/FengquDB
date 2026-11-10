"""Business protobuf objects stay outside the database engine."""
import argparse
import json
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "bindings/python"))
from fengqudb import Error, Store, StoreOperation


def seed(args, schema):
    player = schema.PlayerSave(player_id=42, display_name="\u73a9\u5bb6\u2600", coins=-17,
                               tags=["mage", "\u6cd5\u5e08"], avatar_token=b"\x00\xffA\x00",
                               realm="\u4e9a\u670d", milestones=[-1, 0, 128])
    player.inventory.items.add(item_id=7, count=3)
    player.inventory.items.add(item_id=8, count=0)
    player.counters.update({"wins": -2, "quests": 7})
    raw = player.SerializeToString(deterministic=True)
    with Store.open_at(args.root / "db", "main", library=args.library) as db:
        db.configure_write(mode="reliable")  # This example uses synchronous operations.
        db.put_if_changed(b"player/42", raw)
        assert db.get(b"player/42") == raw
        zero = schema.PlayerSave(player_id=0, display_name="", coins=0)
        assert zero.HasField("player_id") and zero.HasField("display_name") and zero.HasField("coins")
        db.put_if_changed(b"player/0", zero.SerializeToString())
        db.define_index(b"by-id", b"player/", 1, 1, 3)
        db.define_index(b"by-name", b"player/", 2, 4, 3)
        db.define_index(b"by-coins", b"player/", 3, 5, 2)
        db.verify()
    player.avatar_token = bytes(range(256)) * 16
    args.fixture.write_bytes(player.SerializeToString(deterministic=True))
    print("PASS generated Python v2 seed, explicit defaults and typed indexes")


def verify(args, schema):
    with Store.open_at(args.root / "db", "main", library=args.library, create=False) as db:
        db.configure_write(mode="reliable")  # This example uses synchronous operations.
        raw = db.get(b"player/42")
        player = schema.PlayerSave.FromString(raw)
        assert player.player_id == 42 and player.coins == -15 and player.display_name == "v1-Java"
        assert player.HasField("realm") and player.realm == "\u4e9a\u670d"
        assert list(player.milestones) == [-1, 0, 128] and "csharp" in player.tags and "java" in player.tags
        assert player.counters["wins"] == -2 and player.inventory.items[1].count == 0
        assert player.avatar_token == b"\x00\xffA\x00"
        zero = schema.PlayerSave.FromString(db.get(b"player/0"))
        assert zero.HasField("player_id") and zero.player_id == 0
        assert zero.HasField("display_name") and zero.display_name == ""
        assert zero.HasField("coins") and zero.coins == 0
        for name, query, expected in (
            (b"by-id", schema.PlayerSave(player_id=0), b"player/0"),
            (b"by-name", schema.PlayerSave(display_name=""), b"player/0"),
            (b"by-coins", schema.PlayerSave(coins=-15), b"player/42"),
        ):
            rows = db.execute(StoreOperation.INDEX_LOOKUP, name, value=query.SerializeToString()).rows
            assert len(rows) == 1 and rows[0][0] == expected
        for invalid in (schema.PlayerSave(display_name="absent-id", coins=0),
                        schema.PlayerSave(player_id=123, coins=0),
                        schema.PlayerSave(player_id=42, display_name="duplicate", coins=0)):
            try:
                db.put_if_changed(b"player/invalid", invalid.SerializeToString())
            except Error as error:
                assert error.code == 20
            else:
                raise AssertionError("Required/unique constraint accepted an invalid business model")
            assert db.get(b"player/invalid") is None
        assert schema.PlayerSave.FromString(db.get(b"player/7")).HasField("coins")
        assert schema.PlayerSave.FromString(db.get(b"player/99")).realm == "next"
        assert schema.PlayerSave.FromString(db.get(b"player/101")).realm == "desktop-stub"
        db.compact()
        assert db.get(b"player/42") == raw, "Database compaction re-encoded business protobuf bytes"
        db.verify()
    with Store.open_at(args.root / "db", "main", library=args.library, create=False) as db:
        db.configure_write(mode="reliable")  # This example uses synchronous operations.
        assert schema.PlayerSave.FromString(db.get(b"player/42")).realm == "\u4e9a\u670d"
    print("PASS generated v2 after C#/Java old writers, constraints, compact and restart")


def measure(name, count, units, operation):
    checksum = 0
    for _ in range(min(count, 10000)):
        checksum ^= operation()
    samples = []
    started = time.perf_counter_ns()
    for i in range(count):
        if i & 63 == 0:
            before = time.perf_counter_ns()
            checksum += operation()
            samples.append((time.perf_counter_ns() - before) / 1000)
        else:
            checksum += operation()
    seconds = (time.perf_counter_ns() - started) / 1e9
    samples.sort()
    print(json.dumps(dict(language="python", workload=name, calls=count, units_per_call=units,
                          ops_per_second=count * units / seconds,
                          p99_us=samples[int((len(samples) - 1) * .99)], checksum=checksum)), flush=True)


def bench(args, schema):
    raw = args.fixture.read_bytes()
    model = schema.PlayerSave.FromString(raw)
    assert model.player_id == 42 and len(model.avatar_token) == 4096
    score = lambda value: value[0] + value[-1]
    model_score = lambda value: value.player_id + len(value.avatar_token)
    with Store.open_at(args.root / "db", "main", library=args.library) as db:
        db.configure_write(mode="reliable")  # This example uses synchronous operations.
        keys = [("bench/" + str(i)).encode() for i in range(32)]
        db.put_many([(key, raw) for key in keys])
        with db.prepare_key(keys[0]) as prepared:
            reuse = bytearray(len(raw))
            measure("serialize", args.iterations, 1, lambda: score(model.SerializeToString()))
            measure("parse", args.iterations, 1, lambda: model_score(schema.PlayerSave.FromString(raw)))
            measure("get_copy", args.iterations, 1, lambda: score(db.get(keys[0])))
            def into():
                assert prepared.get_into(reuse) == len(raw)
                return score(reuse)
            measure("prepared_get_into", args.iterations, 1, into)
            with prepared.bind_buffer(reuse) as reader:
                def bound():
                    assert reader.read() == len(raw)
                    return score(reuse)
                measure("prepared_bound_buffer", args.iterations, 1, bound)
            measure("get_parse", args.iterations, 1, lambda: model_score(schema.PlayerSave.FromString(db.get(keys[0]))))
            measure("get_many_32", max(8192, args.iterations // 16), 32,
                    lambda: sum(score(value) for value in db.get_many(keys)))
        db.verify()
        assert db.get(keys[31]) == raw


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("mode", choices=["seed", "verify", "bench"])
    parser.add_argument("--generated", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--fixture", type=Path)
    parser.add_argument("--iterations", type=int, default=100000)
    args = parser.parse_args()
    if args.iterations < 10000:
        parser.error("Use at least 10000 benchmark iterations")
    sys.path.insert(0, str(args.generated))
    import player_v2_pb2 as schema
    {"seed": seed, "verify": verify, "bench": bench}[args.mode](args, schema)


if __name__ == "__main__":
    main()
