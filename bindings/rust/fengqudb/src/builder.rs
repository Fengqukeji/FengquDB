//! 类型化查询构建器，等价于 Python `fengqudb_query`：
//! `from_(prefix).where_(..).select(..).order_by(..).limit(..)`。
//!
//! Rust 不能重载 `==`/`<`，所以比较用方法（`eq/ne/lt/le/gt/ge`），组合用 `&`、`|`、`!`。
//! 聚合引用（HAVING / 聚合排序）在 `build()` 时解析为输出列号。

use std::ops::{BitAnd, BitOr, Not};

use crate::error::{Error, Result};
use crate::query::{
    Aggregate, Column, Expr, Field, Join, JoinKind, Op, Order, PreparedQuery, Query, QueryValue, SelectTarget,
};
use crate::result::{QueryResult, Value};

/// 字面量转换：与 Python `_literal(cast, v)` 一致，只在数值之间转换。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Cast {
    None,
    Int,
    Float,
}

impl Cast {
    fn apply(self, v: QueryValue) -> QueryValue {
        match (self, v) {
            (Cast::Int, QueryValue::Double(x)) => QueryValue::Int(x as i64),
            (Cast::Float, QueryValue::Uint(x)) => QueryValue::Double(x as f64),
            (Cast::Float, QueryValue::Int(x)) => QueryValue::Double(x as f64),
            (_, v) => v,
        }
    }
}

/// 条件，在构建时按已选列解析。
#[derive(Clone, Debug)]
pub struct Cond {
    node: Node,
}

#[derive(Clone, Debug)]
enum Node {
    Test { op: Op, field: Field, values: Vec<QueryValue> },
    Cmp { op: Op, left: Field, right: Field },
    Having { op: Op, column: Column, value: QueryValue },
    And(Box<Node>, Box<Node>),
    Or(Box<Node>, Box<Node>),
    Not(Box<Node>),
}

impl Node {
    fn is_aggregate(&self) -> bool {
        match self {
            Node::Having { .. } => true,
            Node::And(a, b) | Node::Or(a, b) => a.is_aggregate() || b.is_aggregate(),
            Node::Not(a) => a.is_aggregate(),
            _ => false,
        }
    }

    fn build(&self, columns: &[Column]) -> Result<Expr> {
        Ok(match self {
            Node::Test { op, field, values } => Expr::test(*op, field.clone(), values.clone()),
            Node::Cmp { op, left, right } => Expr::compare_field(*op, left.clone(), right.clone()),
            Node::Having { op, column, value } => {
                Expr::test(*op, Field::column(output_index(columns, column)?), vec![value.clone()])
            }
            Node::And(a, b) => Expr::logic(Op::And, vec![a.build(columns)?, b.build(columns)?]),
            Node::Or(a, b) => Expr::logic(Op::Or, vec![a.build(columns)?, b.build(columns)?]),
            Node::Not(a) => Expr::logic(Op::Not, vec![a.build(columns)?]),
        })
    }
}

fn output_index(columns: &[Column], column: &Column) -> Result<u32> {
    columns
        .iter()
        .position(|c| c.field == column.field && c.aggregate == column.aggregate && c.distinct == column.distinct)
        .map(|i| i as u32)
        .ok_or(Error::Invalid("aggregate used in having/order_by must also be selected"))
}

impl Cond {
    fn from(node: Node) -> Cond {
        Cond { node }
    }

    /// 是否引用了聚合（只能用于 `having`）。
    pub fn is_aggregate(&self) -> bool {
        self.node.is_aggregate()
    }
}

impl BitAnd for Cond {
    type Output = Cond;
    fn bitand(self, rhs: Cond) -> Cond {
        Cond::from(Node::And(Box::new(self.node), Box::new(rhs.node)))
    }
}

impl BitOr for Cond {
    type Output = Cond;
    fn bitor(self, rhs: Cond) -> Cond {
        Cond::from(Node::Or(Box::new(self.node), Box::new(rhs.node)))
    }
}

impl Not for Cond {
    type Output = Cond;
    fn not(self) -> Cond {
        Cond::from(Node::Not(Box::new(self.node)))
    }
}

/// 排序项，在构建时解析。
#[derive(Clone, Debug)]
pub struct Sort {
    target: SortTarget,
    descending: bool,
    nulls_first: bool,
}

#[derive(Clone, Debug)]
enum SortTarget {
    Field(Field),
    Aggregate(Column),
    Output(u32),
}

impl Sort {
    /// 按输出列序号（从 0 开始）排序。
    pub fn output(index: u32, descending: bool) -> Sort {
        Sort { target: SortTarget::Output(index), descending, nulls_first: true }
    }

    /// 设置 NULL 排在前面（默认）或后面。
    pub fn nulls_first(mut self, first: bool) -> Sort {
        self.nulls_first = first;
        self
    }

    fn build(&self, columns: &[Column]) -> Result<Order> {
        let field = match &self.target {
            SortTarget::Field(f) => f.clone(),
            SortTarget::Aggregate(c) => Field::column(output_index(columns, c)?),
            SortTarget::Output(i) => Field::column(*i),
        };
        Ok(Order { field, descending: self.descending, nulls_first: self.nulls_first })
    }
}

/// 可放入 `select` 的项：`Col` 与 `Agg`。
pub trait Selectable {
    fn column(&self) -> Column;
}

/// 带类型的字段引用（0 = 主前缀，1..7 = 按序的连接源）。
#[derive(Clone, Debug)]
pub struct Col {
    field: Field,
    cast: Cast,
}

impl Selectable for Col {
    fn column(&self) -> Column {
        Column::new(self.field.clone())
    }
}

impl Col {
    fn test(&self, op: Op, values: Vec<QueryValue>) -> Cond {
        let values = values.into_iter().map(|v| if matches!(v, QueryValue::Bool(_)) { v } else { self.cast.apply(v) }).collect();
        Cond::from(Node::Test { op, field: self.field.clone(), values })
    }

    fn one(&self, op: Op, v: impl Into<QueryValue>) -> Cond {
        self.test(op, vec![v.into()])
    }

    fn cmp_col(&self, op: Op, other: &Col) -> Cond {
        Cond::from(Node::Cmp { op, left: self.field.clone(), right: other.field.clone() })
    }

    /// 标记为 protobuf repeated 字段：任一元素满足即为真。
    pub fn any(&self) -> Col {
        Col { field: self.field.any(), cast: self.cast }
    }

    pub fn eq(&self, v: impl Into<QueryValue>) -> Cond {
        self.one(Op::Eq, v)
    }
    pub fn ne(&self, v: impl Into<QueryValue>) -> Cond {
        self.one(Op::Ne, v)
    }
    pub fn lt(&self, v: impl Into<QueryValue>) -> Cond {
        self.one(Op::Lt, v)
    }
    pub fn le(&self, v: impl Into<QueryValue>) -> Cond {
        self.one(Op::Le, v)
    }
    pub fn gt(&self, v: impl Into<QueryValue>) -> Cond {
        self.one(Op::Gt, v)
    }
    pub fn ge(&self, v: impl Into<QueryValue>) -> Cond {
        self.one(Op::Ge, v)
    }

    /// 与另一列比较。
    pub fn eq_col(&self, other: &Col) -> Cond {
        self.cmp_col(Op::Eq, other)
    }
    pub fn ne_col(&self, other: &Col) -> Cond {
        self.cmp_col(Op::Ne, other)
    }
    pub fn lt_col(&self, other: &Col) -> Cond {
        self.cmp_col(Op::Lt, other)
    }
    pub fn le_col(&self, other: &Col) -> Cond {
        self.cmp_col(Op::Le, other)
    }
    pub fn gt_col(&self, other: &Col) -> Cond {
        self.cmp_col(Op::Gt, other)
    }
    pub fn ge_col(&self, other: &Col) -> Cond {
        self.cmp_col(Op::Ge, other)
    }

    /// `low <= x <= high`，编码为 `AND(GE, LE)`。
    pub fn between(&self, low: impl Into<QueryValue>, high: impl Into<QueryValue>) -> Cond {
        self.ge(low) & self.le(high)
    }
    /// `IN (values...)`。
    pub fn in_<V: Into<QueryValue>>(&self, values: impl IntoIterator<Item = V>) -> Cond {
        self.test(Op::In, values.into_iter().map(Into::into).collect())
    }
    pub fn is_null(&self) -> Cond {
        self.test(Op::IsNull, Vec::new())
    }
    pub fn exists(&self) -> Cond {
        self.test(Op::Exists, Vec::new())
    }
    pub fn contains(&self, s: impl Into<QueryValue>) -> Cond {
        self.one(Op::Contains, s)
    }
    pub fn starts_with(&self, s: impl Into<QueryValue>) -> Cond {
        self.one(Op::StartsWith, s)
    }
    pub fn ends_with(&self, s: impl Into<QueryValue>) -> Cond {
        self.one(Op::EndsWith, s)
    }
    pub fn like(&self, s: impl Into<QueryValue>) -> Cond {
        self.one(Op::Like, s)
    }
    pub fn regex(&self, s: impl Into<QueryValue>) -> Cond {
        self.one(Op::Regex, s)
    }
    pub fn icontains(&self, s: impl Into<QueryValue>) -> Cond {
        self.one(Op::IContains, s)
    }
    pub fn ilike(&self, s: impl Into<QueryValue>) -> Cond {
        self.one(Op::ILike, s)
    }
    /// 整字段 UTF-8 编辑距离（0..8）。
    pub fn fuzzy(&self, s: impl Into<QueryValue>, distance: u32) -> Cond {
        Cond::from(Node::Test {
            op: Op::Fuzzy,
            field: self.field.clone(),
            values: vec![s.into(), QueryValue::Uint(u64::from(distance))],
        })
    }

    pub fn asc(&self) -> Sort {
        Sort { target: SortTarget::Field(self.field.clone()), descending: false, nulls_first: true }
    }
    pub fn desc(&self) -> Sort {
        Sort { target: SortTarget::Field(self.field.clone()), descending: true, nulls_first: true }
    }
}

/// 聚合输出列；比较只能用于 `having`。
#[derive(Clone, Debug)]
pub struct Agg {
    column: Column,
}

impl Selectable for Agg {
    fn column(&self) -> Column {
        self.column.clone()
    }
}

impl Agg {
    fn cmp(&self, op: Op, v: impl Into<QueryValue>) -> Cond {
        Cond::from(Node::Having { op, column: self.column.clone(), value: v.into() })
    }
    pub fn eq(&self, v: impl Into<QueryValue>) -> Cond {
        self.cmp(Op::Eq, v)
    }
    pub fn ne(&self, v: impl Into<QueryValue>) -> Cond {
        self.cmp(Op::Ne, v)
    }
    pub fn lt(&self, v: impl Into<QueryValue>) -> Cond {
        self.cmp(Op::Lt, v)
    }
    pub fn le(&self, v: impl Into<QueryValue>) -> Cond {
        self.cmp(Op::Le, v)
    }
    pub fn gt(&self, v: impl Into<QueryValue>) -> Cond {
        self.cmp(Op::Gt, v)
    }
    pub fn ge(&self, v: impl Into<QueryValue>) -> Cond {
        self.cmp(Op::Ge, v)
    }
    pub fn asc(&self) -> Sort {
        Sort { target: SortTarget::Aggregate(self.column.clone()), descending: false, nulls_first: true }
    }
    pub fn desc(&self) -> Sort {
        Sort { target: SortTarget::Aggregate(self.column.clone()), descending: true, nulls_first: true }
    }
}

// 字段工厂：名字是 protobuf 线类型。
fn col(number: u32, ty: u32, source: u32, cast: Cast) -> Col {
    Col { field: Field::new(number, ty, source), cast }
}
pub fn uint64(n: u32, source: u32) -> Col {
    col(n, 1, source, Cast::Int)
}
pub fn int64(n: u32, source: u32) -> Col {
    col(n, 2, source, Cast::Int)
}
pub fn double(n: u32, source: u32) -> Col {
    col(n, 3, source, Cast::Float)
}
pub fn text(n: u32, source: u32) -> Col {
    col(n, 4, source, Cast::None)
}
pub fn sint64(n: u32, source: u32) -> Col {
    col(n, 5, source, Cast::Int)
}
pub fn float32(n: u32, source: u32) -> Col {
    col(n, 6, source, Cast::Float)
}
pub fn fixed64(n: u32, source: u32) -> Col {
    col(n, 7, source, Cast::Int)
}
pub fn sfixed64(n: u32, source: u32) -> Col {
    col(n, 8, source, Cast::Int)
}
pub fn fixed32(n: u32, source: u32) -> Col {
    col(n, 9, source, Cast::Int)
}
pub fn sfixed32(n: u32, source: u32) -> Col {
    col(n, 10, source, Cast::Int)
}
pub fn boolean(n: u32, source: u32) -> Col {
    col(n, 11, source, Cast::None)
}
/// 记录键。
pub fn key(source: u32) -> Col {
    Col { field: Field::key(source), cast: Cast::None }
}
/// 嵌套路径字段。
pub fn nested(path: &[u32], ty: u32, source: u32) -> Col {
    Col { field: Field { number: 0, ty, source, path: path.to_vec(), repeated: false }, cast: Cast::None }
}

fn agg(field: Field, aggregate: Aggregate, distinct: bool) -> Agg {
    Agg { column: Column { field, aggregate, name: String::new(), distinct } }
}
pub fn count() -> Agg {
    agg(Field::key(0), Aggregate::CountAll, false)
}
/// `COUNT_ALL` 聚合绑定到指定列（`count()` 绑定到记录键）。
pub fn count_all_of(c: &Col) -> Agg {
    agg(c.field.clone(), Aggregate::CountAll, false)
}
pub fn count_of(c: &Col) -> Agg {
    agg(c.field.clone(), Aggregate::Count, false)
}
pub fn count_distinct(c: &Col) -> Agg {
    agg(c.field.clone(), Aggregate::Count, true)
}
pub fn sum(c: &Col) -> Agg {
    agg(c.field.clone(), Aggregate::Sum, false)
}
pub fn avg(c: &Col) -> Agg {
    agg(c.field.clone(), Aggregate::Avg, false)
}
pub fn min(c: &Col) -> Agg {
    agg(c.field.clone(), Aggregate::Min, false)
}
pub fn max(c: &Col) -> Agg {
    agg(c.field.clone(), Aggregate::Max, false)
}

/// 查询构建器。
#[derive(Clone, Debug)]
pub struct Builder {
    query: Query,
    where_: Option<Cond>,
    having: Option<Cond>,
    order: Vec<Sort>,
}

/// 以前缀开始构建查询。
pub fn from_(prefix: impl Into<Vec<u8>>) -> Builder {
    Builder { query: Query::new(prefix), where_: None, having: None, order: Vec::new() }
}

impl Builder {
    pub fn where_(mut self, cond: Cond) -> Builder {
        self.where_ = Some(cond);
        self
    }
    pub fn select(mut self, items: &[&dyn Selectable]) -> Builder {
        self.query.columns = items.iter().map(|i| i.column()).collect();
        self
    }
    pub fn group_by(mut self, cols: &[&Col]) -> Builder {
        self.query.group = cols.iter().map(|c| c.field.clone()).collect();
        self
    }
    pub fn having(mut self, cond: Cond) -> Builder {
        self.having = Some(cond);
        self
    }
    pub fn order_by(mut self, items: impl IntoIterator<Item = Sort>) -> Builder {
        self.order = items.into_iter().collect();
        self
    }
    pub fn join(mut self, prefix: impl Into<Vec<u8>>, left: &Col, right: &Col, kind: JoinKind) -> Builder {
        self.query.joins.push(Join::new(prefix.into(), vec![left.field.clone()], vec![right.field.clone()], kind));
        self
    }
    pub fn limit(mut self, n: u64) -> Builder {
        self.query.limit = n;
        self
    }
    pub fn offset(mut self, n: u64) -> Builder {
        self.query.offset = n;
        self
    }
    pub fn after(mut self, cursor: impl Into<Vec<u8>>) -> Builder {
        self.query.after = cursor.into();
        self
    }
    pub fn distinct(mut self, on: bool) -> Builder {
        self.query.distinct = on;
        self
    }
    pub fn count_total(mut self, on: bool) -> Builder {
        self.query.count_total = on;
        self
    }
    pub fn allow_scan(mut self, on: bool) -> Builder {
        self.query.allow_scan = on;
        self
    }
    pub fn index(mut self, name: impl Into<Vec<u8>>) -> Builder {
        self.query.index_hint = name.into();
        self
    }
    pub fn memory(mut self, bytes: u64) -> Builder {
        self.query.memory_bytes = bytes;
        self
    }
    pub fn timeout_ms(mut self, ms: u64) -> Builder {
        self.query.timeout_ms = ms;
        self
    }

    /// 等价的底层 `Query`。聚合条件放在 `where_` 中会报错。
    pub fn build(&self) -> Result<Query> {
        let mut q = self.query.clone();
        if let Some(w) = &self.where_ {
            if w.is_aggregate() {
                return Err(Error::Invalid("aggregates belong in having()"));
            }
            q.where_ = w.node.build(&q.columns)?;
        }
        if let Some(h) = &self.having {
            q.having = h.node.build(&q.columns)?;
        }
        q.order = self.order.iter().map(|s| s.build(&q.columns)).collect::<Result<_>>()?;
        Ok(q)
    }

    /// 编码一次，供重复执行。
    pub fn prepare(&self) -> Result<TypedPrepared> {
        let q = self.build()?;
        Ok(TypedPrepared { prepared: q.prepare()?, columns: q.columns })
    }

    /// 执行并返回可按列取值的结果。
    pub fn run(&self, db: &impl SelectTarget) -> Result<TypedResult> {
        self.prepare()?.run(db)
    }

    /// 返回访问计划。
    pub fn explain(&self, db: &impl SelectTarget) -> Result<QueryResult> {
        self.build()?.explain(db)
    }
}

/// 已编码的类型化查询。
#[derive(Clone, Debug)]
pub struct TypedPrepared {
    prepared: PreparedQuery,
    columns: Vec<Column>,
}

impl TypedPrepared {
    pub fn run(&self, db: &impl SelectTarget) -> Result<TypedResult> {
        Ok(TypedResult { columns: self.columns.clone(), result: self.prepared.execute(db)? })
    }
}

/// 带列信息的结果；值仍在原生缓冲上惰性解码。
#[derive(Debug)]
pub struct TypedResult {
    columns: Vec<Column>,
    result: QueryResult,
}

impl TypedResult {
    /// 底层响应（游标、统计、访问计划）。
    pub fn result(&self) -> &QueryResult {
        &self.result
    }

    pub fn cursor(&self) -> Result<&[u8]> {
        self.result.cursor()
    }

    pub fn rows(&self) -> impl Iterator<Item = Result<TypedRow<'_>>> {
        let columns = &self.columns[..];
        self.result.rows().map(move |r| r.map(|row| TypedRow { columns, row }))
    }
}

/// 一行，按 `select` 时用的 `Col`/`Agg` 取值。
pub struct TypedRow<'a> {
    columns: &'a [Column],
    row: crate::result::ResultRow<'a>,
}

impl<'a> TypedRow<'a> {
    pub fn key(&self) -> &'a [u8] {
        self.row.key()
    }

    /// 取已选列的值；缺失字段为 `Value::Null`。
    pub fn get(&self, item: &dyn Selectable) -> Result<Value<'a>> {
        let target = item.column();
        let index = output_index(self.columns, &target).map_err(|_| Error::Invalid("column was not selected"))?;
        self.row.value(index as usize)?.ok_or(Error::Decode("row has fewer values than columns"))
    }
}
