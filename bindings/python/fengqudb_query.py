"""Typed, concise Python query builder over fengqudb_select.

Declare each protobuf field once, then write conditions with operators::

    from fengqudb_query import u64, s64, text, from_, count, sum_
    uid, cls, level, gold = u64(1), text(2), u64(3), s64(4)
    rows = (from_(b"p/").where((cls == "mage") & level.between(20, 60) & (gold >= 1000))
            .select(uid, gold).order_by(gold.desc(), uid).limit(20).run(db))
    for row in rows:
        print(row[uid], row[gold])        # None for a missing field

Combine conditions with ``&``, ``|`` and ``~`` (Python's ``and``/``or`` cannot be
overloaded) and parenthesize comparisons. The builder only produces the existing
request; semantics and budgets are documented in docs/STORE_SELECT.md. A Store runs
on a read-only snapshot, a transaction sees its private writes.
"""
from fengqudb_select import Query, Field, Expr, Op, Column, Order, Join, JoinKind, Aggregate, PreparedQuery

__all__ = ["u64", "i64", "f64", "text", "s64", "f32", "fixed64", "sfixed64", "fixed32", "sfixed32",
           "boolean", "key", "nested", "count", "count_distinct", "sum_", "avg", "min_", "max_",
           "from_", "Builder", "Rows", "JoinKind"]


class Cond:
    """A condition resolved against the selected columns when the query is built.

    Aggregate references (HAVING) become output-column references at that point."""
    __slots__ = ("_resolve", "aggregate")

    def __init__(self, resolve, aggregate=False):
        self._resolve, self.aggregate = resolve, aggregate

    def build(self, columns):
        return self._resolve(columns)

    def __and__(self, other):
        return Cond(lambda c: Expr(Op.AND, children=(self.build(c), other.build(c))),
                    self.aggregate or other.aggregate)

    def __or__(self, other):
        return Cond(lambda c: Expr(Op.OR, children=(self.build(c), other.build(c))),
                    self.aggregate or other.aggregate)

    def __invert__(self):
        return Cond(lambda c: Expr(Op.NOT, children=(self.build(c),)), self.aggregate)

    def __bool__(self):
        raise TypeError("use &, | and ~ to combine query conditions, not and/or/not")


def _literal(cast, value):
    return cast(value) if cast is not None and not isinstance(value, bool) else value


class Col:
    """A typed field of one source (0 = main prefix, 1..7 = joins in order)."""
    __hash__ = None  # == builds a condition, so columns are not dictionary keys

    def __init__(self, field, cast=None):
        self.field, self._cast = field, cast

    def any(self):
        """Same path as a protobuf repeated field: true when any element matches."""
        return Col(self.field.any(), self._cast)

    def _test(self, op, *values):
        values = tuple(_literal(self._cast, v) for v in values)
        return Cond(lambda c: Expr(op, self.field, values))

    def _cmp(self, op, other):
        if isinstance(other, Col):
            return Cond(lambda c: Expr(op, self.field, right=other.field))
        return self._test(op, other)

    def __eq__(self, v): return self._cmp(Op.EQ, v)
    def __ne__(self, v): return self._cmp(Op.NE, v)
    def __lt__(self, v): return self._cmp(Op.LT, v)
    def __le__(self, v): return self._cmp(Op.LE, v)
    def __gt__(self, v): return self._cmp(Op.GT, v)
    def __ge__(self, v): return self._cmp(Op.GE, v)
    def between(self, low, high): return (self >= low) & (self <= high)
    def in_(self, values): return self._test(Op.IN, *values)
    def is_null(self): return Cond(lambda c: Expr(Op.IS_NULL, self.field))
    def exists(self): return Cond(lambda c: Expr(Op.EXISTS, self.field))
    def contains(self, s): return self._test(Op.CONTAINS, s)
    def starts_with(self, s): return self._test(Op.STARTS_WITH, s)
    def ends_with(self, s): return self._test(Op.ENDS_WITH, s)
    def like(self, s): return self._test(Op.LIKE, s)
    def regex(self, s): return self._test(Op.REGEX, s)
    def icontains(self, s): return self._test(Op.ICONTAINS, s)
    def ilike(self, s): return self._test(Op.ILIKE, s)
    def fuzzy(self, s, distance):
        """Whole-field UTF-8 edit distance, 0..8."""
        return Cond(lambda c: Expr(Op.FUZZY, self.field, (s, int(distance))))
    def asc(self, nulls_first=True): return _Sort(lambda c: Order(self.field, False, nulls_first))
    def desc(self, nulls_first=True): return _Sort(lambda c: Order(self.field, True, nulls_first))
    def _column(self): return Column(self.field)


class _Sort:
    __slots__ = ("build",)
    def __init__(self, build): self.build = build


def _output_index(columns, column):
    for i, c in enumerate(columns):
        if c.field == column.field and c.aggregate == column.aggregate and c.distinct == column.distinct:
            return i
    raise ValueError("aggregate used in having/order_by must also be selected")


class Agg:
    """An aggregate output column; comparisons are valid only in having()."""
    __hash__ = None

    def __init__(self, column):
        self.column = column

    def _cmp(self, op, value):
        return Cond(lambda c: Expr(op, Field.column(_output_index(c, self.column)), (value,)), True)

    def __eq__(self, v): return self._cmp(Op.EQ, v)
    def __ne__(self, v): return self._cmp(Op.NE, v)
    def __lt__(self, v): return self._cmp(Op.LT, v)
    def __le__(self, v): return self._cmp(Op.LE, v)
    def __gt__(self, v): return self._cmp(Op.GT, v)
    def __ge__(self, v): return self._cmp(Op.GE, v)
    def asc(self, nulls_first=True):
        return _Sort(lambda c: Order(Field.column(_output_index(c, self.column)), False, nulls_first))
    def desc(self, nulls_first=True):
        return _Sort(lambda c: Order(Field.column(_output_index(c, self.column)), True, nulls_first))
    def _column(self): return self.column


# Field factories: the name is the protobuf wire type.
def u64(n, source=0): return Col(Field(n, 1, source), int)
def i64(n, source=0): return Col(Field(n, 2, source), int)
def f64(n, source=0): return Col(Field(n, 3, source), float)
def text(n, source=0): return Col(Field(n, 4, source))
def s64(n, source=0): return Col(Field(n, 5, source), int)
def f32(n, source=0): return Col(Field(n, 6, source), float)
def fixed64(n, source=0): return Col(Field(n, 7, source), int)
def sfixed64(n, source=0): return Col(Field(n, 8, source), int)
def fixed32(n, source=0): return Col(Field(n, 9, source), int)
def sfixed32(n, source=0): return Col(Field(n, 10, source), int)
def boolean(n, source=0): return Col(Field(n, 11, source), bool)
def key(source=0): return Col(Field.key(source))
def nested(path, type, source=0): return Col(Field(0, type, source, tuple(path)))

def count(): return Agg(Column(Field.key(), Aggregate.COUNT_ALL))
def count_of(col): return Agg(Column(col.field, Aggregate.COUNT))
def count_distinct(col): return Agg(Column(col.field, Aggregate.COUNT, distinct=True))
def sum_(col): return Agg(Column(col.field, Aggregate.SUM))
def avg(col): return Agg(Column(col.field, Aggregate.AVG))
def min_(col): return Agg(Column(col.field, Aggregate.MIN))
def max_(col): return Agg(Column(col.field, Aggregate.MAX))


class Rows:
    """Result rows with access by the same Col/Agg objects used in select()."""

    def __init__(self, columns, result):
        self._columns, self.raw = columns, result
        self.cursor, self.stats, self.access = result.cursor, result.stats, result.access

    def _index(self, item):
        target = item._column()
        for i, c in enumerate(self._columns):
            if c.field == target.field and c.aggregate == target.aggregate and c.distinct == target.distinct:
                return i
        raise KeyError("column was not selected")

    def __len__(self): return len(self.raw.rows)
    def __iter__(self): return (_Row(self, r) for r in self.raw.rows)
    def __getitem__(self, i): return _Row(self, self.raw.rows[i])


class _Row:
    __slots__ = ("_owner", "_row")
    def __init__(self, owner, row): self._owner, self._row = owner, row
    def __getitem__(self, item): return self._row["values"][self._owner._index(item)]
    @property
    def key(self): return self._row["key"]


class Builder:
    def __init__(self, prefix):
        self._prefix = prefix.encode() if isinstance(prefix, str) else bytes(prefix)
        self._where = self._having = None
        self._columns, self._order, self._group, self._joins = [], [], [], []
        self._options = {}

    def where(self, cond):
        if cond.aggregate: raise ValueError("aggregates belong in having()")
        self._where = cond; return self
    def select(self, *items): self._columns = [i._column() for i in items]; return self
    def group_by(self, *cols): self._group = [c.field for c in cols]; return self
    def having(self, cond): self._having = cond; return self
    def order_by(self, *items):
        self._order = [i if isinstance(i, _Sort) else i.asc() for i in items]; return self
    def join(self, prefix, left, right, kind=JoinKind.INNER):
        prefix = prefix.encode() if isinstance(prefix, str) else bytes(prefix)
        self._joins.append(Join(prefix, (left.field,), (right.field,), kind)); return self
    def limit(self, n): self._options["limit"] = n; return self
    def offset(self, n): self._options["offset"] = n; return self
    def after(self, cursor): self._options["after"] = cursor; return self
    def distinct(self, on=True): self._options["distinct"] = on; return self
    def count_total(self, on=True): self._options["count_total"] = on; return self
    def allow_scan(self, on): self._options["allow_scan"] = on; return self
    def index(self, name): self._options["index_hint"] = name.encode() if isinstance(name, str) else name; return self
    def memory(self, n): self._options["memory_bytes"] = n; return self
    def timeout_ms(self, n): self._options["timeout_ms"] = n; return self

    def build(self):
        """The equivalent low-level fengqudb_select.Query."""
        cols = self._columns
        q = Query(self._prefix, columns=list(cols), group=list(self._group), joins=list(self._joins),
                  order=[s.build(cols) for s in self._order], **self._options)
        if self._where is not None: q.where = self._where.build(cols)
        if self._having is not None: q.having = self._having.build(cols)
        return q

    def prepare(self): return Prepared(self.build().prepare(), list(self._columns))
    def run(self, database): return Rows(self._columns, self.build().execute(database))
    def explain(self, database): return self.build().explain(database)


class Prepared:
    """Encodes the request once for repeated execution."""
    def __init__(self, prepared, columns): self._prepared, self._columns = prepared, columns
    def run(self, database): return Rows(self._columns, self._prepared.execute(database))


def from_(prefix): return Builder(prefix)
