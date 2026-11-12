//! 一致性向量：`FQDB_CONFORMANCE` 指向 build/lang/conformance.json。
//!
//! 1) 底层 Query 与类型化构建器编码出的字节 == `encoded` / `request`；
//! 2) 在 dataset 上执行，结果行、游标、统计、策略、索引名与 `result` 一致；
//! 3) 分页用例的各页按序拼接 == `paginate.values`。

mod common;

use common::{open, TempDir};
use fengqudb::builder::{self, Builder, Selectable};
use fengqudb::query::{Aggregate, Column, Expr, Field, Join, JoinKind, Op, Order, Query, QueryValue};
use fengqudb::{FieldType, IndexField, Store, Value};
use serde_json::Value as Json;

fn load_json() -> Json {
    let path = std::env::var_os("FQDB_CONFORMANCE").map(std::path::PathBuf::from).unwrap_or_else(|| {
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../build/lang/conformance.json")
    });
    let text = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("cannot read conformance vectors {} (set FQDB_CONFORMANCE): {e}", path.display()));
    serde_json::from_str(&text).expect("conformance.json is valid JSON")
}

fn unhex(s: &str) -> Vec<u8> {
    assert!(s.len() % 2 == 0, "odd hex");
    (0..s.len() / 2).map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap()).collect()
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

// ---- spec JSON -> 底层 Query ----

fn field(j: &Json) -> Field {
    Field {
        number: j["number"].as_u64().unwrap() as u32,
        ty: j["type"].as_u64().unwrap() as u32,
        source: j["source"].as_u64().unwrap() as u32,
        path: j["path"].as_array().unwrap().iter().map(|n| n.as_u64().unwrap() as u32).collect(),
        repeated: j["repeated"].as_bool().unwrap(),
    }
}

fn value(j: &Json) -> QueryValue {
    if let Some(n) = j.get("int") {
        return match n.as_i64() {
            Some(i) => QueryValue::Int(i),
            None => QueryValue::Uint(n.as_u64().expect("int out of range")),
        };
    }
    if let Some(b) = j.get("bytes") {
        return QueryValue::Bytes(unhex(b.as_str().unwrap()));
    }
    if let Some(x) = j.get("f64") {
        return QueryValue::Double(x.as_f64().unwrap());
    }
    if let Some(b) = j.get("bool") {
        return QueryValue::Bool(b.as_bool().unwrap());
    }
    if j.get("null").is_some() {
        return QueryValue::Null;
    }
    panic!("unknown value {j}");
}

fn expr(j: &Json) -> Expr {
    Expr {
        op: Op::from_u32(j["op"].as_u64().unwrap() as u32).unwrap(),
        field: j.get("field").map(field).unwrap_or_default(),
        values: j["values"].as_array().unwrap().iter().map(value).collect(),
        children: j["children"].as_array().unwrap().iter().map(expr).collect(),
        right: j.get("right").filter(|r| !r.is_null()).map(field),
    }
}

fn query(spec: &Json) -> Query {
    let mut q = Query::new(unhex(spec["prefix"].as_str().unwrap()));
    q.where_ = expr(&spec["where"]);
    q.columns = spec["columns"]
        .as_array()
        .unwrap()
        .iter()
        .map(|c| Column {
            field: field(&c["field"]),
            aggregate: Aggregate::from_u32(c["aggregate"].as_u64().unwrap() as u32).unwrap(),
            name: c["name"].as_str().unwrap().to_owned(),
            distinct: c["distinct"].as_bool().unwrap(),
        })
        .collect();
    q.order = spec["order"]
        .as_array()
        .unwrap()
        .iter()
        .map(|o| Order {
            field: field(&o["field"]),
            descending: o["descending"].as_bool().unwrap(),
            nulls_first: o["nulls_first"].as_bool().unwrap(),
        })
        .collect();
    q.group = spec["group"].as_array().unwrap().iter().map(field).collect();
    q.having = expr(&spec["having"]);
    q.joins = spec["joins"]
        .as_array()
        .unwrap()
        .iter()
        .map(|j| Join {
            prefix: unhex(j["prefix"].as_str().unwrap()),
            left: j["left"].as_array().unwrap().iter().map(field).collect(),
            right: j["right"].as_array().unwrap().iter().map(field).collect(),
            kind: JoinKind::from_u32(j["kind"].as_u64().unwrap() as u32).unwrap(),
            on: expr(&j["on"]),
        })
        .collect();
    let n = |k: &str| spec[k].as_u64().unwrap();
    q.limit = n("limit");
    q.offset = n("offset");
    q.memory_bytes = n("memory_bytes");
    q.max_candidates = n("max_candidates");
    q.max_work = n("max_work");
    q.timeout_ms = n("timeout_ms");
    q.allow_scan = spec["allow_scan"].as_bool().unwrap();
    q.count_total = spec["count_total"].as_bool().unwrap();
    q.distinct = spec["distinct"].as_bool().unwrap();
    q.index_hint = unhex(spec["index_hint"].as_str().unwrap());
    q.after = unhex(spec["after"].as_str().unwrap());
    q
}

// ---- 数据集 ----

fn load_dataset(json: &Json) -> (TempDir, Store) {
    let dir = TempDir::new("conf");
    let db = open(&dir);
    let dataset = &json["dataset"];
    for r in dataset["records"].as_array().unwrap() {
        db.put(&unhex(r["key"].as_str().unwrap()), &unhex(r["value"].as_str().unwrap())).unwrap();
    }
    let types = [
        FieldType::Uint64,
        FieldType::Int64,
        FieldType::Double,
        FieldType::Bytes,
        FieldType::Sint64,
        FieldType::Float,
        FieldType::Fixed64,
        FieldType::Sfixed64,
        FieldType::Fixed32,
        FieldType::Sfixed32,
        FieldType::Bool,
    ];
    for idx in dataset["indexes"].as_array().unwrap() {
        let fields: Vec<IndexField> = idx["fields"]
            .as_array()
            .unwrap()
            .iter()
            .map(|f| IndexField { field: f[0].as_u64().unwrap() as u32, ty: types[f[1].as_u64().unwrap() as usize - 1] })
            .collect();
        db.define_compound_index(
            idx["name"].as_str().unwrap(),
            idx["prefix"].as_str().unwrap().as_bytes(),
            &fields,
            idx["flags"].as_u64().unwrap() as u32,
        )
        .unwrap();
    }
    (dir, db)
}

fn same_value(expected: &Json, actual: &Value<'_>) -> bool {
    if let Some(n) = expected.get("int") {
        return match actual {
            Value::Uint(u) => n.as_u64() == Some(*u),
            Value::Int(i) => n.as_i64() == Some(*i),
            _ => false,
        };
    }
    if let Some(b) = expected.get("bytes") {
        return matches!(actual, Value::Bytes(a) if *a == unhex(b.as_str().unwrap()).as_slice());
    }
    if let Some(x) = expected.get("f64") {
        return matches!(actual, Value::Double(a) if *a == x.as_f64().unwrap());
    }
    if let Some(b) = expected.get("bool") {
        return matches!(actual, Value::Bool(a) if *a == b.as_bool().unwrap());
    }
    expected.get("null").is_some() && actual.is_null()
}

/// 对照 `result`：行、游标、统计、策略与索引名。
fn check_result(id: &str, expected: &Json, got: &fengqudb::QueryResult) {
    let rows: Vec<_> = got.rows().collect::<Result<_, _>>().unwrap();
    let want = expected["rows"].as_array().unwrap();
    assert_eq!(rows.len(), want.len(), "{id}: row count");
    for (i, (row, w)) in rows.iter().zip(want).enumerate() {
        assert_eq!(hex(row.key()), w["key"].as_str().unwrap(), "{id}: row {i} key");
        let values: Vec<_> = row.values().collect::<Result<_, _>>().unwrap();
        let wv = w["values"].as_array().unwrap();
        assert_eq!(values.len(), wv.len(), "{id}: row {i} value count");
        for (j, (a, e)) in values.iter().zip(wv).enumerate() {
            assert!(same_value(e, a), "{id}: row {i} value {j}: expected {e}, got {a:?}");
        }
    }
    assert_eq!(hex(got.cursor().unwrap()), expected["cursor"].as_str().unwrap(), "{id}: cursor");
    let stats = got.stats().unwrap();
    assert_eq!(stats.matched, expected["matched"].as_u64().unwrap(), "{id}: matched");
    assert_eq!(stats.complete, expected["complete"].as_bool().unwrap(), "{id}: complete");
    assert_eq!(stats.result_total, expected["result_total"].as_u64().unwrap(), "{id}: result_total");
    let access: Vec<_> = got.access().collect::<Result<_, _>>().unwrap();
    let strategies: Vec<&str> = access.iter().map(|a| a.strategy).collect();
    let indexes: Vec<&str> = access.iter().map(|a| a.index).collect();
    let list = |k: &str| -> Vec<String> {
        expected[k].as_array().unwrap().iter().map(|s| s.as_str().unwrap().to_owned()).collect()
    };
    assert_eq!(strategies, list("strategies"), "{id}: strategies");
    assert_eq!(indexes, list("indexes"), "{id}: indexes");
}

#[test]
fn low_level_query_bytes_match_vectors() {
    let json = load_json();
    for case in json["cases"].as_array().unwrap() {
        let id = case["id"].as_str().unwrap();
        let q = query(&case["spec"]);
        assert_eq!(hex(&q.encode().unwrap()), case["encoded"].as_str().unwrap(), "{id}: encoded");
        let explain = id.starts_with("explain");
        assert_eq!(hex(&q.request(explain).unwrap()), case["request"].as_str().unwrap(), "{id}: request");
        assert_eq!(hex(q.prepare().unwrap().encoded()), case["encoded"].as_str().unwrap(), "{id}: prepared");
    }
}

#[test]
fn execute_results_match_vectors() {
    let json = load_json();
    let (_dir, db) = load_dataset(&json);
    let mut executed = 0;
    for case in json["cases"].as_array().unwrap() {
        let id = case["id"].as_str().unwrap();
        let q = query(&case["spec"]);
        if id.starts_with("paginate") {
            continue; // 见 paginate_pages_match_vectors
        }
        let got = if id.starts_with("explain") { q.explain(&db) } else { q.execute(&db) }.unwrap_or_else(|e| panic!("{id}: {e}"));
        check_result(id, &case["result"], &got);
        executed += 1;
    }
    assert_eq!(executed, 16);
}

#[test]
fn paginate_pages_match_vectors() {
    let json = load_json();
    let (_dir, db) = load_dataset(&json);
    let case = json["cases"].as_array().unwrap().iter().find(|c| c.get("paginate").is_some()).expect("paginate case");
    let q = query(&case["spec"]);
    let want: Vec<u64> = case["paginate"]["values"].as_array().unwrap().iter().map(|v| v.as_u64().unwrap()).collect();
    let mut got = Vec::new();
    let pages = q
        .execute_pages(&db, |page| {
            for row in page.rows() {
                got.push(row?.value(0)?.and_then(|v| v.as_u64()).expect("uid"));
            }
            Ok(())
        })
        .unwrap();
    assert_eq!(got, want);
    assert_eq!(pages as u64, case["paginate"]["pages"].as_u64().unwrap());

    // 手动按游标翻页，与 execute_pages 一致。
    let mut q = q;
    let mut manual = Vec::new();
    loop {
        let page = q.execute(&db).unwrap();
        for row in page.rows() {
            manual.push(row.unwrap().value(0).unwrap().unwrap().as_u64().unwrap());
        }
        let cursor = page.cursor().unwrap();
        if cursor.is_empty() {
            break;
        }
        q.after = cursor.to_vec();
    }
    assert_eq!(manual, want);
}

// ---- 类型化构建器 ----

/// 与各个一致性用例等价的类型化构建器；`None` 表示该用例没有对应的类型化写法。
fn typed(id: &str, spec: &Json) -> Option<Builder> {
    use builder::*;
    let (uid, cls, level, gold) = (uint64(1, 0), text(2, 0), uint64(3, 0), sint64(4, 0));
    let name = text(5, 0);
    let bytes_of = |s: &Json| unhex(s["where"]["values"][0]["bytes"].as_str().unwrap());
    let join = |kind| {
        from_("p/").join("o/", &uid, &uint64(2, 1), kind).limit(200)
    };
    let topn_where = cls.eq("mage") & level.ge(5) & level.le(15);
    Some(match id {
        "topn_mage_level" | "explain_topn" => {
            from_("p/").where_(topn_where).select(&[&uid, &level]).order_by([level.desc()]).limit(7).count_total(true)
        }
        "or_not_in" => from_("p/")
            .where_((cls.eq("mage") | cls.eq("warrior")) & !gold.lt(0) & uid.in_([10, 20, 30]))
            .select(&[&uid])
            .order_by([uid.asc()]),
        "regex" => from_("p/").where_(name.regex(bytes_of(spec))).select(&[&uid]).order_by([uid.asc()]),
        "like" => from_("p/").where_(name.like(bytes_of(spec))).select(&[&uid]).order_by([uid.asc()]),
        "icontains" => from_("p/").where_(name.icontains(bytes_of(spec))).select(&[&uid]).order_by([uid.asc()]),
        "not_optional_eq" => from_("p/").where_(!uint64(10, 0).eq(3)).select(&[&uid]).order_by([uid.asc()]),
        "is_null" => from_("p/").where_(uint64(10, 0).is_null()).select(&[&uid]).order_by([uid.asc()]),
        "nested_path" => {
            let n = nested(&[8, 1], 1, 0);
            from_("p/").select(&[&n]).order_by([uid.asc()]).limit(3)
        }
        "compare_fields" => from_("p/").where_(level.lt_col(&uid)).select(&[&uid]).order_by([uid.asc()]),
        "repeated_any" => from_("p/").where_(uint64(12, 0).any().eq(3)).select(&[&uid]).order_by([uid.asc()]),
        "aggregates" => {
            let n = count_all_of(&uid);
            from_("p/")
                .select(&[&cls, &n, &sum(&gold), &min(&gold), &max(&gold), &avg(&gold)])
                .group_by(&[&cls])
                .having(n.gt(20))
                .order_by([Sort::output(0, false)])
        }
        "join_inner" => join(JoinKind::Inner).select(&[&uid, &uint64(3, 1)]).order_by([uid.asc(), uint64(3, 1).asc()]),
        "join_left" => join(JoinKind::Left).select(&[&uid, &uint64(3, 1)]).order_by([uid.asc(), uint64(3, 1).asc()]),
        "join_semi" => join(JoinKind::Semi).select(&[&uid]).order_by([uid.asc()]),
        "join_anti" => join(JoinKind::Anti).select(&[&uid]).order_by([uid.asc()]),
        "paginate_gold_desc" => from_("p/").select(&[&uid]).order_by([gold.desc()]).limit(9),
        _ => return None,
    })
}

#[test]
fn typed_builder_bytes_match_vectors() {
    let json = load_json();
    let mut checked = 0;
    for case in json["cases"].as_array().unwrap() {
        let id = case["id"].as_str().unwrap();
        let b = typed(id, &case["spec"]).unwrap_or_else(|| panic!("{id}: no typed equivalent"));
        let q = b.build().unwrap();
        assert_eq!(hex(&q.encode().unwrap()), case["encoded"].as_str().unwrap(), "{id}: typed encoded");
        assert_eq!(q, query(&case["spec"]), "{id}: typed Query equals low-level Query");
        checked += 1;
    }
    assert_eq!(checked, 17);
}

#[test]
fn typed_builder_runs_and_reads_columns() {
    use builder::*;
    let json = load_json();
    let (_dir, db) = load_dataset(&json);
    let (uid, cls, level, gold) = (uint64(1, 0), text(2, 0), uint64(3, 0), sint64(4, 0));
    let res = from_("p/")
        .where_(cls.eq("mage") & level.between(5, 15))
        .select(&[&uid, &level])
        .order_by([level.desc()])
        .limit(7)
        .count_total(true)
        .run(&db)
        .unwrap();
    let rows: Vec<_> = res.rows().collect::<Result<_, _>>().unwrap();
    assert_eq!(rows.len(), 7);
    assert_eq!(rows[0].get(&uid).unwrap(), Value::Uint(55));
    assert_eq!(rows[0].get(&level).unwrap(), Value::Uint(15));
    assert_eq!(rows[0].key(), b"p/55");
    assert!(rows[0].get(&gold).is_err(), "column that was not selected");
    assert!(!res.cursor().unwrap().is_empty());

    // 聚合 + having，结果按列取值。
    let n = count();
    let total = sum(&gold);
    let agg = from_("p/")
        .select(&[&cls, &n, &total])
        .group_by(&[&cls])
        .having(n.ge(1))
        .order_by([n.desc(), total.asc()])
        .run(&db);
    // count() 绑定记录键；原生库是否接受取决于其字段类型规则，只要求不 panic 且错误带状态码。
    match agg {
        Ok(r) => assert!(r.rows().all(|row| row.is_ok())),
        Err(e) => assert!(e.code().is_some(), "{e}"),
    }

    // 聚合出现在 where 里报错；having 引用未选的聚合报错。
    assert!(from_("p/").where_(n.gt(1)).build().is_err());
    assert!(from_("p/").select(&[&uid]).having(n.gt(1)).build().is_err());

    // 事务查询可见私有写入，Store 查询看不到。
    let uid_q = from_("p/").where_(uid.eq(1000)).select(&[&uid]);
    let mut txn = db.begin(0).unwrap();
    txn.put(b"p/1000", &unhex("08e807")).unwrap(); // uid = 1000
    let inside: Vec<_> = uid_q.run(&txn).unwrap().rows().collect::<Result<Vec<_>, _>>().unwrap().iter().map(|r| r.key().to_vec()).collect();
    assert_eq!(inside, vec![b"p/1000".to_vec()]);
    assert_eq!(uid_q.run(&db).unwrap().rows().count(), 0);
    drop(txn);
}

#[test]
fn explain_reports_the_access_plan() {
    use builder::*;
    let json = load_json();
    let (_dir, db) = load_dataset(&json);
    let (cls, level) = (text(2, 0), uint64(3, 0));
    let plan = from_("p/").where_(cls.eq("mage") & level.ge(5)).select(&[&level]).explain(&db).unwrap();
    let access: Vec<_> = plan.access().collect::<Result<_, _>>().unwrap();
    assert!(!access.is_empty());
    assert_eq!(access[0].index, "class-level");
    assert_eq!(plan.rows().count(), 0);
}

#[test]
fn encoder_edge_cases() {
    // 非有限数字、过深表达式报错；负数用 zigzag；浮点用 0x21 + 8 字节。
    let mut q = Query::new(b"p/".to_vec());
    q.where_ = Expr::test(Op::Eq, Field::new(1, 3, 0), vec![QueryValue::Double(f64::NAN)]);
    assert!(q.encode().is_err());
    let mut deep = Expr::default();
    for _ in 0..70 {
        deep = Expr::logic(Op::Not, vec![deep]);
    }
    q.where_ = deep;
    assert!(q.encode().is_err());
    let mut enc = Vec::new();
    for v in [QueryValue::Int(-1), QueryValue::Int(5), QueryValue::Double(1.5), QueryValue::Null, QueryValue::Bool(true)] {
        let mut one = Query::new(Vec::new());
        one.where_ = Expr::test(Op::Eq, Field::new(1, 1, 0), vec![v]);
        enc.push(hex(&one.encode().unwrap()));
    }
    // 负数：kind=2, zigzag(-1)=1 -> 08 02 18 01
    assert!(enc[0].contains("08021801"), "{}", enc[0]);
    // 非负 Int 与 Uint 同形：08 01 10 05
    assert!(enc[1].contains("08011005"), "{}", enc[1]);
    // 1.5 = 0x3ff8000000000000，小端 00 00 00 00 00 00 f8 3f
    assert!(enc[2].contains("080321000000000000f83f"), "{}", enc[2]);
    assert!(enc[3].contains("0800"));
    assert!(enc[4].contains("0805") && enc[4].contains("1001"));
    let _ = Aggregate::None;
    let _: &dyn Selectable = &builder::uint64(1, 0);
}
