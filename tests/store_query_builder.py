"""Typed Python query builder (fengqudb_query) regressions; caller supplies a fresh fixture path."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bindings/python"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from fengqudb import Store
from fengqudb_select import Query, Field, Op, Column, Order
from fengqudb_query import u64, s64, text, from_, count, sum_, avg
from store_select_bindings import document


def run(root, library):
    root = Path(root); assert not root.exists(); root.mkdir(parents=True)
    with Store(root / "main", library=library) as db:
        db.put_many([(b"p/" + str(i).encode(), document(i)) for i in range(64)], force_reliable=True)
        db.define_compound_index(b"class-level", b"p/", [(2, 4), (3, 1)], flags=2)
        uid, cls, level, gold, opt = u64(1), text(2), u64(3), s64(4), u64(10)

        # Same request as the hand-written low-level query.
        top = (from_("p/").where((cls == "mage") & ((level >= 5) & (level <= 15)))
               .select(uid, level).order_by(level.desc()).limit(7))
        manual = Query(b"p/", where=Field(2, 4).test(Op.EQ, "mage") & (Field(3, 1).test(Op.GE, 5) &
                       Field(3, 1).test(Op.LE, 15)), columns=[Column(Field(1, 1)), Column(Field(3, 1))],
                       order=[Order(Field(3, 1), True)], limit=7)
        assert top.build().encode() == manual.encode(), "builder encoding differs"

        # repeated: .any() encodes the same request as a hand-written repeated field.
        items = u64(12)
        typed_any = from_("p/").where(items.any() == 5).select(uid).limit(20)
        manual_any = Query(b"p/", where=Field(12, 1).any().test(Op.EQ, 5), columns=[Column(Field(1, 1))], limit=20)
        assert typed_any.build().encode() == manual_any.encode(), "repeated builder encoding differs"
        assert typed_any.build().encode() != from_("p/").where(items == 5).select(uid).limit(20).build().encode(), \
            "repeated flag missing from builder encoding"

        # Typed rows by column object.
        rows = top.run(db)
        expected = sorted([i for i in range(64) if i % 2 and 5 <= i % 20 <= 15],
                          key=lambda i: (i % 20, b"p/" + str(i).encode()), reverse=True)[:7]
        assert [r[uid] for r in rows] == expected, ([r[uid] for r in rows], expected)
        assert all(5 <= r[level] <= 15 for r in rows)
        assert len(top.prepare().run(db)) == 7
        assert len(from_("p/").where((cls == "mage") & level.between(5, 15)).select(uid).limit(100).run(db)) == len(
            [i for i in range(64) if i % 2 and 5 <= i % 20 <= 15])

        # Aggregates with HAVING and ORDER BY on selected aggregates.
        groups = (from_("p/").group_by(cls).select(cls, count(), sum_(gold))
                  .having(sum_(gold) > 8000).order_by(cls)).run(db)
        assert len(groups) == 1 and groups[0][cls] == b"mage" and groups[0][count()] == 32 and \
            groups[0][sum_(gold)] == 8064, [(g[cls], g[count()], g[sum_(gold)]) for g in groups]
        by_avg = from_("p/").group_by(cls).select(cls, avg(gold)).order_by(avg(gold).desc()).run(db)
        assert [g[cls] for g in by_avg] == [b"mage", b"warrior"]
        try:
            from_("p/").group_by(cls).select(cls).order_by(sum_(gold).desc()).build()
            raise AssertionError("ordering by an unselected aggregate accepted")
        except ValueError:
            pass

        # NULL, IN, LIKE, NOT.
        missing = from_("p/").where(opt.is_null()).select(uid, opt).limit(100).run(db)
        assert len(missing) == 22 and missing[0][opt] is None
        assert len(from_("p/").where(uid.in_([3, 8, 3])).select(uid).run(db)) == 2
        assert len(from_("p/").where(text(5).like("Name-1%")).select(uid).run(db)) == 11
        assert len(from_("p/").where(~(cls == "mage")).select(uid).run(db)) == 32
        try:
            missing[0][gold]; raise AssertionError("unselected column read")
        except KeyError:
            pass

        # Cursor pagination.
        page = from_("p/").select(uid).order_by(gold.desc()).limit(9)
        seen = []
        while True:
            r = page.run(db)
            seen += [row[uid] for row in r]
            if not r.cursor: break
            page.after(r.cursor)
        assert seen == list(range(63, -1, -1)), seen

        # Misuse is reported.
        try:
            if cls == "mage": pass
            raise AssertionError("condition used as bool")
        except TypeError:
            pass
        try:
            from_("p/").where(count() > 1); raise AssertionError("aggregate accepted in where()")
        except ValueError:
            pass

        # Store queries run on a snapshot; transactions see private writes.
        everything = from_("p/").select(uid).limit(4096)
        with db.transaction() as tx:
            tx.put(b"p/100", document(100))
            assert len(everything.run(tx)) == 65 and len(everything.run(db)) == 64
            tx.rollback() if hasattr(tx, "rollback") else None
    print("PASS Python typed query builder encoding, rows, aggregates, nulls, paging and misuse")


if __name__ == "__main__": run(sys.argv[1], sys.argv[2])
