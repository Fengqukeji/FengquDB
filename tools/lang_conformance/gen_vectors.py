"""生成跨语言一致性测试集 conformance.json。

用法: python gen_vectors.py LIBRARY OUT.json
Python 绑定在同一份核心库上执行全部查询，把数据集、索引、查询规格、请求字节(hex)
和期望结果固化下来。各语言绑定必须: 1) 用自己的查询模型从 spec 重新编码出相同的字节；
2) 在同一数据集上得到相同结果。编码规则见 bindings/python/fengqudb_select.py。
"""
import json, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "bindings/python"))
sys.path.insert(0, str(ROOT / "tests"))
from fengqudb import Store, _feature_request  # noqa: E402
from fengqudb_select import (Query, Field, Expr, Op, Column, Order, Join, JoinKind,  # noqa: E402
                             Aggregate, _u, _b)
from store_select_bindings import document  # noqa: E402


def doc(i):
    # 在原测试文档上追加 repeated 字段 12（非 packed），用于 any() 查询。
    return document(i) + _u(12, i % 5) + _u(12, (i + 2) % 5)


def owner(i):
    return _u(1, i) + _u(2, i % 16) + _u(3, i * 10)


# ---- 值 / 规格 的 JSON 表示 ----
def jv(v):
    if v is None: return {"null": True}
    if isinstance(v, bool): return {"bool": v}
    if isinstance(v, int): return {"int": v}
    if isinstance(v, float): return {"f64": v}
    if isinstance(v, str): v = v.encode()
    return {"bytes": bytes(v).hex()}


def un_jv(d):
    if "null" in d: return None
    if "bool" in d: return d["bool"]
    if "int" in d: return d["int"]
    if "f64" in d: return d["f64"]
    return bytes.fromhex(d["bytes"])


def field_spec(f):
    return {"number": f.number, "type": f.type, "source": f.source, "path": list(f.path), "repeated": f.repeated}


def expr_spec(e):
    out = {"op": int(e.op), "values": [jv(v) for v in e.values], "children": [expr_spec(c) for c in e.children]}
    if e.op >= 5: out["field"] = field_spec(e.field)
    if e.right is not None: out["right"] = field_spec(e.right)
    return out


def query_spec(q):
    return {
        "prefix": q.prefix.hex(), "where": expr_spec(q.where),
        "columns": [{"field": field_spec(c.field), "aggregate": int(c.aggregate), "name": c.name,
                     "distinct": c.distinct} for c in q.columns],
        "order": [{"field": field_spec(o.field), "descending": o.descending, "nulls_first": o.nulls_first}
                  for o in q.order],
        "group": [field_spec(f) for f in q.group], "having": expr_spec(q.having),
        "joins": [{"prefix": j.prefix.hex(), "left": [field_spec(f) for f in j.left],
                   "right": [field_spec(f) for f in j.right], "kind": int(j.kind), "on": expr_spec(j.on)}
                  for j in q.joins],
        "limit": q.limit, "offset": q.offset, "memory_bytes": q.memory_bytes,
        "max_candidates": q.max_candidates, "max_work": q.max_work, "timeout_ms": q.timeout_ms,
        "allow_scan": q.allow_scan, "count_total": q.count_total, "distinct": q.distinct,
        "index_hint": q.index_hint.hex(), "after": q.after.hex(),
    }


# ---- 参考实现: spec -> Query 对象 (用来证明 spec 完整无歧义) ----
def un_field(d):
    return Field(d["number"], d["type"], d["source"], tuple(d["path"]), d["repeated"])


def un_expr(d):
    return Expr(Op(d["op"]), un_field(d["field"]) if "field" in d else Field(),
                tuple(un_jv(v) for v in d["values"]), tuple(un_expr(c) for c in d["children"]),
                un_field(d["right"]) if "right" in d else None)


def un_query(d):
    return Query(
        bytes.fromhex(d["prefix"]), un_expr(d["where"]),
        [Column(un_field(c["field"]), Aggregate(c["aggregate"]), c["name"], c["distinct"]) for c in d["columns"]],
        [Order(un_field(o["field"]), o["descending"], o["nulls_first"]) for o in d["order"]],
        [un_field(f) for f in d["group"]], un_expr(d["having"]),
        [Join(bytes.fromhex(j["prefix"]), tuple(un_field(f) for f in j["left"]),
              tuple(un_field(f) for f in j["right"]), JoinKind(j["kind"]), un_expr(j["on"])) for j in d["joins"]],
        d["limit"], d["offset"], d["memory_bytes"], d["max_candidates"], d["max_work"], d["timeout_ms"],
        d["allow_scan"], d["count_total"], d["distinct"], bytes.fromhex(d["index_hint"]), bytes.fromhex(d["after"]))


def result_spec(r):
    return {"rows": [{"key": row["key"].hex(), "values": [jv(v) for v in row["values"]]} for row in r.rows],
            "cursor": r.cursor.hex(),
            "matched": r.stats.get("matched"), "complete": r.stats.get("complete"),
            "result_total": r.stats.get("result_total"),
            "strategies": [a.get("strategy") for a in r.access], "indexes": [a.get("index") for a in r.access]}


def queries():
    level, gold, uid, name, kind = Field(3, 1), Field(4, 5), Field(1, 1), Field(5, 4), Field(2, 4)
    o_uid = Field(2, 1, 1)
    out = []

    def add(qid, q, **extra):
        out.append(dict(id=qid, query=q, **extra))

    add("topn_mage_level", Query(b"p/", where=kind.test(Op.EQ, b"mage") & level.test(Op.GE, 5) & level.test(Op.LE, 15),
                                 columns=[Column(uid), Column(level)], order=[Order(level, True)], limit=7,
                                 count_total=True))
    add("or_not_in", Query(b"p/", where=(kind.test(Op.EQ, b"mage") | kind.test(Op.EQ, b"warrior")) &
                           ~gold.test(Op.LT, 0) & uid.test(Op.IN, 10, 20, 30), columns=[Column(uid)],
                           order=[Order(uid)]))
    add("regex", Query(b"p/", where=name.test(Op.REGEX, b"^Name-(?:1[0-9]|2[0-3])$"), columns=[Column(uid)],
                       order=[Order(uid)]))
    add("like", Query(b"p/", where=name.test(Op.LIKE, b"Name-1_"), columns=[Column(uid)], order=[Order(uid)]))
    add("icontains", Query(b"p/", where=name.test(Op.ICONTAINS, b"NAME-63"), columns=[Column(uid)],
                           order=[Order(uid)]))
    add("not_optional_eq", Query(b"p/", where=~Field(10, 1).test(Op.EQ, 3), columns=[Column(uid)],
                                 order=[Order(uid)], limit=100))
    add("is_null", Query(b"p/", where=Field(10, 1).test(Op.IS_NULL), columns=[Column(uid)], order=[Order(uid)],
                         limit=100))
    add("nested_path", Query(b"p/", columns=[Column(Field(0, 1, path=(8, 1)))], order=[Order(uid)], limit=3))
    add("compare_fields", Query(b"p/", where=level.compare_field(Op.LT, uid), columns=[Column(uid)],
                                order=[Order(uid)], limit=100))
    add("repeated_any", Query(b"p/", where=Field(12, 1, 0, (), True).test(Op.EQ, 3), columns=[Column(uid)],
                              order=[Order(uid)], limit=100))
    add("aggregates", Query(b"p/", columns=[Column(kind), Column(uid, Aggregate.COUNT_ALL),
                                            Column(gold, Aggregate.SUM), Column(gold, Aggregate.MIN),
                                            Column(gold, Aggregate.MAX), Column(gold, Aggregate.AVG)],
                            group=[kind], order=[Order(Field.column(0))],
                            having=Field.column(1).test(Op.GT, 20)))
    for jid, kind_ in (("join_inner", JoinKind.INNER), ("join_left", JoinKind.LEFT)):
        add(jid, Query(b"p/", columns=[Column(uid), Column(Field(3, 1, 1))],
                       joins=[Join(b"o/", (uid,), (o_uid,), kind_)], order=[Order(uid), Order(Field(3, 1, 1))],
                       limit=200))
    for jid, kind_ in (("join_semi", JoinKind.SEMI), ("join_anti", JoinKind.ANTI)):
        add(jid, Query(b"p/", columns=[Column(uid)], joins=[Join(b"o/", (uid,), (o_uid,), kind_)],
                       order=[Order(uid)], limit=200))
    add("paginate_gold_desc", Query(b"p/", columns=[Column(uid)], order=[Order(gold, True)], limit=9),
        paginate=True)
    return out


def run(library, out_path):
    with tempfile.TemporaryDirectory() as tmp:
        with Store(Path(tmp) / "db", library=library) as db:
            db.put_many([(b"p/" + str(i).encode(), doc(i)) for i in range(64)], force_reliable=True)
            db.put_many([(b"o/" + str(i).encode(), owner(i)) for i in range(32)], force_reliable=True)
            indexes = [
                dict(name="class-level", prefix="p/", fields=[[2, 4], [3, 1]], flags=2),
                dict(name="uid", prefix="p/", fields=[[1, 1]], flags=3),
                dict(name="order-owner", prefix="o/", fields=[[2, 1]], flags=2),
            ]
            for ix in indexes:
                db.define_compound_index(ix["name"].encode(), ix["prefix"].encode(),
                                         [tuple(f) for f in ix["fields"]], flags=ix["flags"])
            cases = []
            for item in queries():
                q = item["query"]
                spec = query_spec(q)
                encoded = q.encode()
                assert un_query(spec).encode() == encoded, item["id"] + ": spec is not self-describing"
                case = {"id": item["id"], "spec": spec, "encoded": encoded.hex(),
                        "request": _feature_request(53, b"", b"", encoded).hex()}
                if item.get("paginate"):
                    seq = []
                    pages = 0
                    while True:
                        r = q.execute(db)
                        pages += 1
                        seq += [row["values"][0] for row in r.rows]
                        if not r.cursor: break
                        q.after = r.cursor
                    q.after = b""
                    case["paginate"] = {"values": seq, "pages": pages}
                    case["result"] = None
                else:
                    r = q.execute(db)
                    case["result"] = result_spec(r)
                cases.append(case)
            first = queries()[0]["query"]
            ex = first.explain(db)
            cases.append({"id": "explain_topn", "spec": query_spec(first), "encoded": first.encode().hex(),
                          "request": _feature_request(54, b"", b"", first.encode()).hex(),
                          "result": result_spec(ex)})
    doc_out = {
        "version": 1,
        "dataset": {
            "records": [{"key": (b"p/" + str(i).encode()).hex(), "value": doc(i).hex()} for i in range(64)] +
                       [{"key": (b"o/" + str(i).encode()).hex(), "value": owner(i).hex()} for i in range(32)],
            "indexes": indexes},
        "cases": cases}
    Path(out_path).write_text(json.dumps(doc_out, indent=1), encoding="utf-8")
    print("cases", len(cases), "records", len(doc_out["dataset"]["records"]), "->", out_path)


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
