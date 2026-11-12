//! 底层查询模型与编码，字节与 `bindings/python/fengqudb_select.py` 逐字节一致。

use crate::error::{Error, Result};
use crate::result::QueryResult;
use crate::store::{Store, Transaction};
use crate::wire::{put_bytes, put_uint, put_varint};

/// 表达式操作符。
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
#[repr(u32)]
pub enum Op {
    #[default]
    True = 0,
    False = 1,
    And = 2,
    Or = 3,
    Not = 4,
    Eq = 5,
    Ne = 6,
    Lt = 7,
    Le = 8,
    Gt = 9,
    Ge = 10,
    In = 11,
    Exists = 12,
    IsNull = 13,
    Contains = 14,
    StartsWith = 15,
    EndsWith = 16,
    Like = 17,
    Regex = 18,
    IContains = 19,
    ILike = 20,
    IRegex = 21,
    Fuzzy = 22,
}

impl Op {
    /// 由数值构造（一致性向量使用数值）。
    pub fn from_u32(v: u32) -> Result<Op> {
        use Op::*;
        const ALL: [Op; 23] = [
            True, False, And, Or, Not, Eq, Ne, Lt, Le, Gt, Ge, In, Exists, IsNull, Contains, StartsWith, EndsWith,
            Like, Regex, IContains, ILike, IRegex, Fuzzy,
        ];
        ALL.get(v as usize).copied().ok_or(Error::Invalid("unknown operator"))
    }
}

/// 聚合函数。
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
#[repr(u32)]
pub enum Aggregate {
    #[default]
    None = 0,
    CountAll = 1,
    Count = 2,
    Sum = 3,
    Min = 4,
    Max = 5,
    Avg = 6,
}

impl Aggregate {
    pub fn from_u32(v: u32) -> Result<Aggregate> {
        use Aggregate::*;
        [None, CountAll, Count, Sum, Min, Max, Avg].get(v as usize).copied().ok_or(Error::Invalid("unknown aggregate"))
    }
}

/// 连接类型。
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
#[repr(u32)]
pub enum JoinKind {
    #[default]
    Inner = 0,
    Left = 1,
    Semi = 2,
    Anti = 3,
}

impl JoinKind {
    pub fn from_u32(v: u32) -> Result<JoinKind> {
        use JoinKind::*;
        [Inner, Left, Semi, Anti].get(v as usize).copied().ok_or(Error::Invalid("unknown join kind"))
    }
}

/// 查询字面量（`QueryValue`）：null / uint / sint(zigzag) / double / bytes / bool。
#[derive(Clone, Debug, PartialEq)]
pub enum QueryValue {
    Null,
    Uint(u64),
    /// 负数按 zigzag 编码；非负数与 Python 一致，编码为 `Uint`。
    Int(i64),
    Double(f64),
    Bytes(Vec<u8>),
    Bool(bool),
}

impl QueryValue {
    pub(crate) fn encode_into(&self, out: &mut Vec<u8>) -> Result<()> {
        match self {
            QueryValue::Null => put_uint(out, 1, 0),
            QueryValue::Bool(b) => {
                put_uint(out, 1, 5);
                put_uint(out, 2, u64::from(*b));
            }
            QueryValue::Uint(v) => {
                put_uint(out, 1, 1);
                put_uint(out, 2, *v);
            }
            QueryValue::Int(v) if *v >= 0 => {
                put_uint(out, 1, 1);
                put_uint(out, 2, *v as u64);
            }
            QueryValue::Int(v) => {
                put_uint(out, 1, 2);
                put_uint(out, 3, ((*v << 1) ^ (*v >> 63)) as u64);
            }
            QueryValue::Double(v) => {
                if !v.is_finite() {
                    return Err(Error::Invalid("non-finite query number"));
                }
                put_uint(out, 1, 3);
                out.push(0x21);
                out.extend_from_slice(&v.to_le_bytes());
            }
            QueryValue::Bytes(v) => {
                put_uint(out, 1, 4);
                put_bytes(out, 5, v);
            }
        }
        Ok(())
    }
}

macro_rules! value_from {
    ($($t:ty => |$v:ident| $e:expr),* $(,)?) => {$(
        impl From<$t> for QueryValue { fn from($v: $t) -> Self { $e } }
    )*};
}
value_from! {
    u64 => |v| QueryValue::Uint(v),
    u32 => |v| QueryValue::Uint(u64::from(v)),
    i64 => |v| QueryValue::Int(v),
    i32 => |v| QueryValue::Int(i64::from(v)),
    f64 => |v| QueryValue::Double(v),
    bool => |v| QueryValue::Bool(v),
    &str => |v| QueryValue::Bytes(v.as_bytes().to_vec()),
    String => |v| QueryValue::Bytes(v.into_bytes()),
    &[u8] => |v| QueryValue::Bytes(v.to_vec()),
    Vec<u8> => |v| QueryValue::Bytes(v),
}

impl<const N: usize> From<&[u8; N]> for QueryValue {
    fn from(v: &[u8; N]) -> Self {
        QueryValue::Bytes(v.to_vec())
    }
}

/// 字段引用。`ty` 是 protobuf 类型号（0 表示整条记录/输出列引用，4 表示 bytes/键）。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Field {
    pub number: u32,
    pub ty: u32,
    pub source: u32,
    pub path: Vec<u32>,
    /// protobuf repeated 字段：任一元素满足即为真，只能用于主源 WHERE 叶子条件。
    pub repeated: bool,
}

impl Default for Field {
    fn default() -> Self {
        Field { number: 0, ty: 4, source: 0, path: Vec::new(), repeated: false }
    }
}

impl Field {
    pub fn new(number: u32, ty: u32, source: u32) -> Field {
        Field { number, ty, source, path: Vec::new(), repeated: false }
    }

    /// 记录键。
    pub fn key(source: u32) -> Field {
        Field { source, ..Field::default() }
    }

    /// 整条记录。
    pub fn record(source: u32) -> Field {
        Field { ty: 0, source, ..Field::default() }
    }

    /// 输出列引用（HAVING / ORDER BY 聚合用），`index` 从 0 开始。
    pub fn column(index: u32) -> Field {
        Field::new(index + 1, 0, 255)
    }

    /// 返回标记为 repeated 的副本（`.any()`）。
    pub fn any(&self) -> Field {
        Field { repeated: true, ..self.clone() }
    }

    pub(crate) fn encode_into(&self, out: &mut Vec<u8>) {
        put_uint(out, 1, u64::from(self.source));
        put_uint(out, 2, u64::from(self.ty));
        if self.path.is_empty() {
            if self.number != 0 {
                put_uint(out, 3, u64::from(self.number));
            }
        } else {
            for n in &self.path {
                put_uint(out, 3, u64::from(*n));
            }
        }
        if self.repeated {
            put_uint(out, 4, 1);
        }
    }
}

fn nested(out: &mut Vec<u8>, number: u32, f: impl FnOnce(&mut Vec<u8>) -> Result<()>) -> Result<()> {
    let mut tmp = Vec::new();
    f(&mut tmp)?;
    put_bytes(out, number, &tmp);
    Ok(())
}

/// 条件表达式树。
#[derive(Clone, Debug, PartialEq)]
pub struct Expr {
    pub op: Op,
    pub field: Field,
    pub values: Vec<QueryValue>,
    pub children: Vec<Expr>,
    pub right: Option<Field>,
}

impl Default for Expr {
    fn default() -> Self {
        Expr { op: Op::True, field: Field::default(), values: Vec::new(), children: Vec::new(), right: None }
    }
}

impl Expr {
    /// 叶子条件：`field op values...`。
    pub fn test(op: Op, field: Field, values: Vec<QueryValue>) -> Expr {
        Expr { op, field, values, ..Expr::default() }
    }

    /// 字段对字段的比较。
    pub fn compare_field(op: Op, field: Field, other: Field) -> Expr {
        Expr { op, field, right: Some(other), ..Expr::default() }
    }

    /// 逻辑组合（AND/OR/NOT）。
    pub fn logic(op: Op, children: Vec<Expr>) -> Expr {
        Expr { op, children, ..Expr::default() }
    }

    pub(crate) fn encode_into(&self, out: &mut Vec<u8>, depth: u32) -> Result<()> {
        if depth > 64 {
            return Err(Error::Invalid("query expression too deep"));
        }
        put_uint(out, 1, u64::from(self.op as u32));
        if self.op as u32 >= 5 {
            nested(out, 2, |b| {
                self.field.encode_into(b);
                Ok(())
            })?;
        }
        for v in &self.values {
            nested(out, 3, |b| v.encode_into(b))?;
        }
        for c in &self.children {
            nested(out, 4, |b| c.encode_into(b, depth + 1))?;
        }
        if let Some(right) = &self.right {
            nested(out, 5, |b| {
                right.encode_into(b);
                Ok(())
            })?;
        }
        Ok(())
    }
}

/// 输出列。
#[derive(Clone, Debug, PartialEq, Eq, Default)]
pub struct Column {
    pub field: Field,
    pub aggregate: Aggregate,
    pub name: String,
    pub distinct: bool,
}

impl Column {
    pub fn new(field: Field) -> Column {
        Column { field, ..Column::default() }
    }

    pub(crate) fn encode_into(&self, out: &mut Vec<u8>) {
        let mut f = Vec::new();
        self.field.encode_into(&mut f);
        put_bytes(out, 1, &f);
        put_uint(out, 2, u64::from(self.aggregate as u32));
        put_bytes(out, 3, self.name.as_bytes());
        put_uint(out, 4, u64::from(self.distinct));
    }
}

/// 排序项。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Order {
    pub field: Field,
    pub descending: bool,
    pub nulls_first: bool,
}

impl Order {
    pub fn new(field: Field) -> Order {
        Order { field, descending: false, nulls_first: true }
    }

    pub(crate) fn encode_into(&self, out: &mut Vec<u8>) {
        let mut f = Vec::new();
        self.field.encode_into(&mut f);
        put_bytes(out, 1, &f);
        put_uint(out, 2, u64::from(self.descending));
        put_uint(out, 3, u64::from(self.nulls_first));
    }
}

/// 连接。
#[derive(Clone, Debug, PartialEq)]
pub struct Join {
    pub prefix: Vec<u8>,
    pub left: Vec<Field>,
    pub right: Vec<Field>,
    pub kind: JoinKind,
    pub on: Expr,
}

impl Join {
    pub fn new(prefix: Vec<u8>, left: Vec<Field>, right: Vec<Field>, kind: JoinKind) -> Join {
        Join { prefix, left, right, kind, on: Expr::default() }
    }

    pub(crate) fn encode_into(&self, out: &mut Vec<u8>) -> Result<()> {
        put_bytes(out, 1, &self.prefix);
        for (number, fields) in [(2, &self.left), (3, &self.right)] {
            for f in fields {
                nested(out, number, |b| {
                    f.encode_into(b);
                    Ok(())
                })?;
            }
        }
        put_uint(out, 4, u64::from(self.kind as u32));
        nested(out, 5, |b| self.on.encode_into(b, 0))
    }
}

/// 底层查询结构。默认值与 Python `Query` 相同。
#[derive(Clone, Debug, PartialEq)]
pub struct Query {
    pub prefix: Vec<u8>,
    pub where_: Expr,
    pub columns: Vec<Column>,
    pub order: Vec<Order>,
    pub group: Vec<Field>,
    pub having: Expr,
    pub joins: Vec<Join>,
    pub limit: u64,
    pub offset: u64,
    pub memory_bytes: u64,
    pub max_candidates: u64,
    pub max_work: u64,
    pub timeout_ms: u64,
    pub allow_scan: bool,
    pub count_total: bool,
    pub distinct: bool,
    pub index_hint: Vec<u8>,
    pub after: Vec<u8>,
}

impl Query {
    pub fn new(prefix: impl Into<Vec<u8>>) -> Query {
        Query {
            prefix: prefix.into(),
            where_: Expr::default(),
            columns: Vec::new(),
            order: Vec::new(),
            group: Vec::new(),
            having: Expr::default(),
            joins: Vec::new(),
            limit: 100,
            offset: 0,
            memory_bytes: 16 << 20,
            max_candidates: 1_000_000,
            max_work: 100_000_000,
            timeout_ms: 1000,
            allow_scan: true,
            count_total: false,
            distinct: false,
            index_hint: Vec::new(),
            after: Vec::new(),
        }
    }

    /// 编码为 `Query` 消息（请求的 value 字段）。
    pub fn encode(&self) -> Result<Vec<u8>> {
        let mut out = Vec::new();
        put_bytes(&mut out, 1, &self.prefix);
        nested(&mut out, 2, |b| self.where_.encode_into(b, 0))?;
        for c in &self.columns {
            nested(&mut out, 3, |b| {
                c.encode_into(b);
                Ok(())
            })?;
        }
        for o in &self.order {
            nested(&mut out, 4, |b| {
                o.encode_into(b);
                Ok(())
            })?;
        }
        for g in &self.group {
            nested(&mut out, 5, |b| {
                g.encode_into(b);
                Ok(())
            })?;
        }
        nested(&mut out, 6, |b| self.having.encode_into(b, 0))?;
        for j in &self.joins {
            nested(&mut out, 7, |b| j.encode_into(b))?;
        }
        let numbers = [
            self.limit,
            self.offset,
            self.memory_bytes,
            self.max_candidates,
            self.max_work,
            self.timeout_ms,
            u64::from(self.allow_scan),
            u64::from(self.count_total),
            u64::from(self.distinct),
        ];
        for (n, v) in (8u32..).zip(numbers) {
            put_uint(&mut out, n, v);
        }
        put_bytes(&mut out, 17, &self.index_hint);
        put_bytes(&mut out, 18, &self.after);
        Ok(out)
    }

    /// 完整的外层请求（SELECT 或 EXPLAIN）。
    pub fn request(&self, explain: bool) -> Result<Vec<u8>> {
        Ok(feature_request(if explain { OP_EXPLAIN } else { OP_SELECT }, &[], &[], &self.encode()?))
    }

    /// 编码一次，供重复执行。
    pub fn prepare(&self) -> Result<PreparedQuery> {
        Ok(PreparedQuery::new(self.encode()?))
    }

    /// 执行查询。
    pub fn execute(&self, db: &impl SelectTarget) -> Result<QueryResult> {
        self.prepare()?.execute(db)
    }

    /// 返回访问计划。
    pub fn explain(&self, db: &impl SelectTarget) -> Result<QueryResult> {
        self.prepare()?.explain(db)
    }

    /// 逐页执行：每页回调一次，随后用游标设置 `after` 继续，直到游标为空。返回页数。
    pub fn execute_pages(&self, db: &impl SelectTarget, mut on_page: impl FnMut(&QueryResult) -> Result<()>) -> Result<usize> {
        let mut query = self.clone();
        let mut pages = 0;
        loop {
            let page = query.execute(db)?;
            on_page(&page)?;
            pages += 1;
            let cursor = page.cursor()?;
            if cursor.is_empty() {
                return Ok(pages);
            }
            query.after = cursor.to_vec();
        }
    }
}

const OP_SELECT: u64 = 53;
const OP_EXPLAIN: u64 = 54;

/// `_feature_request(operation, scope, key, value)` 的外层请求编码。
pub fn feature_request(operation: u64, scope: &[u8], key: &[u8], value: &[u8]) -> Vec<u8> {
    let mut out = vec![0x08];
    put_varint(&mut out, operation);
    put_bytes(&mut out, 2, scope);
    put_bytes(&mut out, 3, key);
    put_bytes(&mut out, 4, value);
    out.push(0x29);
    out.extend_from_slice(&0i64.to_le_bytes());
    out.push(0x31);
    out.extend_from_slice(&0f64.to_le_bytes());
    out.push(0x39);
    out.extend_from_slice(&256u64.to_le_bytes());
    out.push(0x40);
    put_varint(&mut out, 0);
    out
}

/// 已编码一次的查询，可重复执行。
#[derive(Clone, Debug)]
pub struct PreparedQuery {
    encoded: Vec<u8>,
    select_request: Vec<u8>,
    explain_request: Vec<u8>,
}

impl PreparedQuery {
    /// 由 `Query::encode` 的结果构造。
    pub fn new(encoded: Vec<u8>) -> PreparedQuery {
        PreparedQuery {
            select_request: feature_request(OP_SELECT, &[], &[], &encoded),
            explain_request: feature_request(OP_EXPLAIN, &[], &[], &encoded),
            encoded,
        }
    }

    pub fn encoded(&self) -> &[u8] {
        &self.encoded
    }

    /// 执行并返回在原生缓冲上惰性解析的结果。
    pub fn execute(&self, db: &impl SelectTarget) -> Result<QueryResult> {
        db.select_request(&self.select_request)
    }

    pub fn explain(&self, db: &impl SelectTarget) -> Result<QueryResult> {
        db.select_request(&self.explain_request)
    }
}

/// 能执行 SELECT/EXPLAIN 请求的对象：`Store` 在只读快照上执行，`Transaction` 可见私有写入。
pub trait SelectTarget {
    /// 执行完整外层请求（`feature_request` 的输出）。
    fn select_request(&self, request: &[u8]) -> Result<QueryResult>;
}

impl SelectTarget for Store {
    fn select_request(&self, request: &[u8]) -> Result<QueryResult> {
        QueryResult::new(self.select_raw(request)?)
    }
}

impl SelectTarget for Transaction<'_> {
    fn select_request(&self, request: &[u8]) -> Result<QueryResult> {
        QueryResult::new(self.execute_raw(request)?)
    }
}
