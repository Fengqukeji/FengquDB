using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace FengquDB {
// Typed, concise C# query builder over StoreQuery. Declare each protobuf field once:
//
//   var id = Q.U64(1); var cls = Q.Text(2); var level = Q.U64(3); var gold = Q.S64(4);
//   var rows = Q.From("p/").Where(cls == "mage" && level.Between(20, 60) && gold >= 1000)
//                .Select(id, gold).OrderBy(gold.Desc(), id).Limit(20).Run(db);
//   foreach (var row in rows) Use(row.Get(id), row.Get(gold));   // ulong, long
//
// Only the existing request is produced; semantics are in docs/STORE_SELECT.md.
// A Store runs on a read-only snapshot; a StoreTransaction sees its private writes.
public interface IQColumn { QueryColumn Column { get; } }
public interface IQOrder { QueryOrder Resolve(IReadOnlyList<QueryColumn> columns); }

// A condition resolved against the selected columns when the query is built, so
// HAVING aggregates become output-column references. && and || are supported.
public sealed class QCond {
    readonly Func<IReadOnlyList<QueryColumn>, QueryExpr> build;
    internal readonly bool Aggregate;
    internal QCond(Func<IReadOnlyList<QueryColumn>, QueryExpr> build, bool aggregate = false) {
        this.build = build;
        Aggregate = aggregate;
    }
    internal QueryExpr Build(IReadOnlyList<QueryColumn> c) => build(c);
    static QCond Merge(QueryOp op, QCond a, QCond b) => new QCond(c => {
        var children = new List<QueryExpr>();
        foreach (var e in new[] { a.Build(c), b.Build(c) })
            if (e.Op == op) children.AddRange(e.Children); else children.Add(e);
        return new QueryExpr { Op = op, Children = children.ToArray() };
    }, a.Aggregate || b.Aggregate);
    public static QCond operator &(QCond a, QCond b) => Merge(QueryOp.And, a, b);
    public static QCond operator |(QCond a, QCond b) => Merge(QueryOp.Or, a, b);
    public static QCond operator !(QCond a) => new QCond(c => QueryExpr.Not(a.Build(c)), a.Aggregate);
    // Never short-circuit: a && b always builds a & b.
    public static bool operator true(QCond _) => false;
    public static bool operator false(QCond _) => false;
}

public sealed class QSort : IQOrder {
    readonly Func<IReadOnlyList<QueryColumn>, QueryOrder> build;
    internal QSort(Func<IReadOnlyList<QueryColumn>, QueryOrder> build) { this.build = build; }
    public QueryOrder Resolve(IReadOnlyList<QueryColumn> columns) => build(columns);
}

internal static class QHelp {
    internal static bool Same(QueryField a, QueryField b) =>
        a.Source == b.Source && a.Type == b.Type && a.Path.SequenceEqual(b.Path);
    internal static bool Same(QueryColumn a, QueryColumn b) =>
        Same(a.Field, b.Field) && a.Aggregate == b.Aggregate && a.Distinct == b.Distinct;
    internal static uint Index(IReadOnlyList<QueryColumn> columns, QueryColumn c) {
        for (int i = 0; i < columns.Count; i++)
            if (Same(columns[i], c)) return (uint)i;
        throw new ArgumentException("Aggregate used in Having/OrderBy must also be selected");
    }
    internal static QCond Test(QueryField f, QueryOp op, object? v) =>
        new QCond(_ => new QueryExpr { Op = op, Field = f, Values = new object?[] { v } });
}

// A typed field of one source (0 = main prefix, 1..7 = joins in order).
public sealed class QCol<T> : IQColumn, IQOrder {
    public readonly QueryField Field;
    public QCol(QueryField field) { Field = field; }
    public QueryColumn Column => new QueryColumn(Field);
    QueryOrder IQOrder.Resolve(IReadOnlyList<QueryColumn> _) => new QueryOrder(Field);
    QCond Cmp(QueryOp op, QCol<T> o) => new QCond(_ => Field.Compare(op, o.Field));
    public static QCond operator ==(QCol<T> c, T v) => QHelp.Test(c.Field, QueryOp.Eq, v);
    public static QCond operator !=(QCol<T> c, T v) => QHelp.Test(c.Field, QueryOp.Ne, v);
    public static QCond operator <(QCol<T> c, T v) => QHelp.Test(c.Field, QueryOp.Lt, v);
    public static QCond operator <=(QCol<T> c, T v) => QHelp.Test(c.Field, QueryOp.Le, v);
    public static QCond operator >(QCol<T> c, T v) => QHelp.Test(c.Field, QueryOp.Gt, v);
    public static QCond operator >=(QCol<T> c, T v) => QHelp.Test(c.Field, QueryOp.Ge, v);
    public static QCond operator ==(QCol<T> a, QCol<T> b) => a.Cmp(QueryOp.Eq, b);
    public static QCond operator !=(QCol<T> a, QCol<T> b) => a.Cmp(QueryOp.Ne, b);
    public static QCond operator <(QCol<T> a, QCol<T> b) => a.Cmp(QueryOp.Lt, b);
    public static QCond operator <=(QCol<T> a, QCol<T> b) => a.Cmp(QueryOp.Le, b);
    public static QCond operator >(QCol<T> a, QCol<T> b) => a.Cmp(QueryOp.Gt, b);
    public static QCond operator >=(QCol<T> a, QCol<T> b) => a.Cmp(QueryOp.Ge, b);
    public override bool Equals(object? o) => ReferenceEquals(this, o);
    public override int GetHashCode() => System.Runtime.CompilerServices.RuntimeHelpers.GetHashCode(this);
    public QCond Between(T low, T high) => (this >= low) & (this <= high);
    public QCond In(params T[] values) =>
        new QCond(_ => new QueryExpr { Op = QueryOp.In, Field = Field, Values = values.Cast<object?>().ToArray() });
    public QCond IsNull() => new QCond(_ => new QueryExpr { Op = QueryOp.IsNull, Field = Field });
    public QCond Exists() => new QCond(_ => new QueryExpr { Op = QueryOp.Exists, Field = Field });
    public QCond Contains(string s) => QHelp.Test(Field, QueryOp.Contains, s);
    public QCond StartsWith(string s) => QHelp.Test(Field, QueryOp.StartsWith, s);
    public QCond EndsWith(string s) => QHelp.Test(Field, QueryOp.EndsWith, s);
    public QCond Like(string s) => QHelp.Test(Field, QueryOp.Like, s);
    public QCond Regex(string s) => QHelp.Test(Field, QueryOp.Regex, s);
    public QCond IContains(string s) => QHelp.Test(Field, QueryOp.IContains, s);
    public QCond ILike(string s) => QHelp.Test(Field, QueryOp.ILike, s);
    // Whole-field UTF-8 edit distance, 0..8.
    public QCond Fuzzy(string s, ulong distance) =>
        new QCond(_ => new QueryExpr { Op = QueryOp.Fuzzy, Field = Field, Values = new object?[] { s, distance } });
    public QSort Asc(bool nullsFirst = true) => new QSort(_ => new QueryOrder(Field) { NullsFirst = nullsFirst });
    public QSort Desc(bool nullsFirst = true) => new QSort(_ => new QueryOrder(Field, true) { NullsFirst = nullsFirst });
}

// An aggregate output column; comparisons are valid only in Having().
public sealed class QAgg<T> : IQColumn, IQOrder {
    public QueryColumn Column { get; }
    internal QAgg(QueryColumn column) { Column = column; }
    QueryOrder IQOrder.Resolve(IReadOnlyList<QueryColumn> c) => Asc().Resolve(c);
    QCond Cmp(QueryOp op, T v) =>
        new QCond(c => QueryField.Column(QHelp.Index(c, Column)).Test(op, new object?[] { v }), true);
    public static QCond operator ==(QAgg<T> a, T v) => a.Cmp(QueryOp.Eq, v);
    public static QCond operator !=(QAgg<T> a, T v) => a.Cmp(QueryOp.Ne, v);
    public static QCond operator <(QAgg<T> a, T v) => a.Cmp(QueryOp.Lt, v);
    public static QCond operator <=(QAgg<T> a, T v) => a.Cmp(QueryOp.Le, v);
    public static QCond operator >(QAgg<T> a, T v) => a.Cmp(QueryOp.Gt, v);
    public static QCond operator >=(QAgg<T> a, T v) => a.Cmp(QueryOp.Ge, v);
    public override bool Equals(object? o) => ReferenceEquals(this, o);
    public override int GetHashCode() => System.Runtime.CompilerServices.RuntimeHelpers.GetHashCode(this);
    public QSort Asc(bool nullsFirst = true) =>
        new QSort(c => new QueryOrder(QueryField.Column(QHelp.Index(c, Column))) { NullsFirst = nullsFirst });
    public QSort Desc(bool nullsFirst = true) =>
        new QSort(c => new QueryOrder(QueryField.Column(QHelp.Index(c, Column)), true) { NullsFirst = nullsFirst });
}

public static class Q {
    // Field factories: the name is the protobuf wire type, the value is the C# type.
    public static QCol<ulong> U64(uint n, uint source = 0) => new QCol<ulong>(new QueryField(n, 1, source));
    public static QCol<long> I64(uint n, uint source = 0) => new QCol<long>(new QueryField(n, 2, source));
    public static QCol<double> F64(uint n, uint source = 0) => new QCol<double>(new QueryField(n, 3, source));
    public static QCol<string> Text(uint n, uint source = 0) => new QCol<string>(new QueryField(n, 4, source));
    public static QCol<long> S64(uint n, uint source = 0) => new QCol<long>(new QueryField(n, 5, source));
    public static QCol<double> F32(uint n, uint source = 0) => new QCol<double>(new QueryField(n, 6, source));
    public static QCol<ulong> Fixed64(uint n, uint source = 0) => new QCol<ulong>(new QueryField(n, 7, source));
    public static QCol<long> SFixed64(uint n, uint source = 0) => new QCol<long>(new QueryField(n, 8, source));
    public static QCol<ulong> Fixed32(uint n, uint source = 0) => new QCol<ulong>(new QueryField(n, 9, source));
    public static QCol<long> SFixed32(uint n, uint source = 0) => new QCol<long>(new QueryField(n, 10, source));
    public static QCol<bool> Bool(uint n, uint source = 0) => new QCol<bool>(new QueryField(n, 11, source));
    public static QCol<string> Key(uint source = 0) => new QCol<string>(QueryField.Key(source));
    public static QCol<T> Nested<T>(uint type, uint source, params uint[] path) =>
        new QCol<T>(new QueryField { Source = source, Type = type, Path = path });

    public static QAgg<ulong> Count() => new QAgg<ulong>(new QueryColumn(QueryField.Key(), QueryAggregate.CountAll));
    public static QAgg<ulong> Count<T>(QCol<T> c) => new QAgg<ulong>(new QueryColumn(c.Field, QueryAggregate.Count));
    public static QAgg<ulong> CountDistinct<T>(QCol<T> c) =>
        new QAgg<ulong>(new QueryColumn(c.Field, QueryAggregate.Count) { Distinct = true });
    public static QAgg<T> Sum<T>(QCol<T> c) => new QAgg<T>(new QueryColumn(c.Field, QueryAggregate.Sum));
    public static QAgg<double> Avg<T>(QCol<T> c) => new QAgg<double>(new QueryColumn(c.Field, QueryAggregate.Avg));
    public static QAgg<T> Min<T>(QCol<T> c) => new QAgg<T>(new QueryColumn(c.Field, QueryAggregate.Min));
    public static QAgg<T> Max<T>(QCol<T> c) => new QAgg<T>(new QueryColumn(c.Field, QueryAggregate.Max));
    public static QueryBuilder From(string prefix) => new QueryBuilder(Encoding.UTF8.GetBytes(prefix));
    public static QueryBuilder From(byte[] prefix) => new QueryBuilder(prefix);
}

public sealed class QueryBuilder {
    readonly byte[] prefix;
    QCond? where, having;
    QueryColumn[] columns = Array.Empty<QueryColumn>();
    IQOrder[] order = Array.Empty<IQOrder>();
    QueryField[] group = Array.Empty<QueryField>();
    readonly List<QueryJoin> joins = new List<QueryJoin>();
    ulong limit = 100, offset, memory = 16UL << 20, timeout = 1000, maxCandidates = 1000000;
    bool allowScan = true, distinct, countTotal;
    byte[] after = Array.Empty<byte>(), index = Array.Empty<byte>();
    internal QueryBuilder(byte[] prefix) { this.prefix = prefix; }
    public QueryBuilder Where(QCond c) {
        if (c.Aggregate) throw new ArgumentException("Aggregates belong in Having()");
        where = c; return this;
    }
    public QueryBuilder Select(params IQColumn[] items) { columns = items.Select(i => i.Column).ToArray(); return this; }
    public QueryBuilder GroupBy(params IQColumn[] items) { group = items.Select(i => i.Column.Field).ToArray(); return this; }
    public QueryBuilder Having(QCond c) { having = c; return this; }
    public QueryBuilder OrderBy(params IQOrder[] items) { order = items; return this; }
    public QueryBuilder Join<T>(string prefix, QCol<T> left, QCol<T> right, QueryJoinKind kind = QueryJoinKind.Inner) {
        joins.Add(new QueryJoin { Prefix = Encoding.UTF8.GetBytes(prefix), Left = new[] { left.Field },
                                  Right = new[] { right.Field }, Kind = kind });
        return this;
    }
    public QueryBuilder Limit(ulong n) { limit = n; return this; }
    public QueryBuilder Offset(ulong n) { offset = n; return this; }
    public QueryBuilder After(byte[] cursor) { after = cursor; return this; }
    public QueryBuilder Distinct(bool on = true) { distinct = on; return this; }
    public QueryBuilder CountTotal(bool on = true) { countTotal = on; return this; }
    public QueryBuilder AllowScan(bool on) { allowScan = on; return this; }
    public QueryBuilder Index(string name) { index = Encoding.UTF8.GetBytes(name); return this; }
    public QueryBuilder Memory(ulong bytes) { memory = bytes; return this; }
    public QueryBuilder TimeoutMs(ulong ms) { timeout = ms; return this; }
    public QueryBuilder MaxCandidates(ulong n) { maxCandidates = n; return this; }
    // The equivalent low-level request.
    public StoreQuery Build() {
        var q = new StoreQuery { Prefix = prefix, Columns = columns, Group = group, Joins = joins.ToArray(),
                                 Limit = limit, Offset = offset, MemoryBytes = memory, TimeoutMs = timeout,
                                 MaxCandidates = maxCandidates, AllowScan = allowScan, Distinct = distinct,
                                 CountTotal = countTotal, After = after, IndexHint = index };
        if (where != null) q.Where = where.Build(columns);
        if (having != null) q.Having = having.Build(columns);
        q.Order = order.Select(o => o.Resolve(columns)).ToArray();
        return q;
    }
    public QPrepared Prepare() => new QPrepared(Build().Prepare(), columns);
    public QRows Run(Store db) => new QRows(columns, db.Select(Build()));
    public QRows Run(StoreTransaction tx) => new QRows(columns, tx.Select(Build()));
}

// Encodes the request once for repeated execution.
public sealed class QPrepared {
    readonly PreparedQuery prepared; readonly QueryColumn[] columns;
    internal QPrepared(PreparedQuery p, QueryColumn[] c) { prepared = p; columns = c; }
    public QRows Run(Store db) => new QRows(columns, prepared.Execute(db));
    public QRows Run(StoreTransaction tx) => new QRows(columns, prepared.Execute(tx));
}

public sealed class QRows : IReadOnlyList<QRow> {
    readonly QueryColumn[] columns;
    public StoreQueryResult Raw { get; }
    internal QRows(QueryColumn[] columns, StoreQueryResult raw) { this.columns = columns; Raw = raw; }
    internal int IndexOf(QueryColumn c) {
        for (int i = 0; i < columns.Length; i++)
            if (QHelp.Same(columns[i], c)) return i;
        throw new KeyNotFoundException("Column was not selected");
    }
    public int Count => Raw.Rows.Count;
    public QRow this[int i] => new QRow(this, Raw.Rows[i]);
    // Non-empty when another page exists; pass it to QueryBuilder.After().
    public byte[] Cursor => Raw.Cursor;
    public QueryStats Stats => Raw.Stats;
    public IEnumerator<QRow> GetEnumerator() { foreach (var r in Raw.Rows) yield return new QRow(this, r); }
    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
}

public readonly struct QRow {
    readonly QRows owner; readonly QueryRow row;
    internal QRow(QRows owner, QueryRow row) { this.owner = owner; this.row = row; }
    public byte[] Key => row.Key;
    // Throws InvalidOperationException for a missing field (NULL); use TryGet for optional fields.
    public T Get<T>(QCol<T> c) => Require<T>(Raw(c.Column));
    public T Get<T>(QAgg<T> a) => Require<T>(Raw(a.Column));
    public bool TryGet<T>(QCol<T> c, out T value) {
        var v = Raw(c.Column);
        value = v == null ? default! : Cast<T>(v);
        return v != null;
    }
    object? Raw(QueryColumn c) => row.Values[owner.IndexOf(c)];
    static T Require<T>(object? v) => v == null ? throw new InvalidOperationException("Query value is NULL") : Cast<T>(v);
    static T Cast<T>(object v) {
        if (typeof(T) == typeof(string)) return (T)(object)Encoding.UTF8.GetString((byte[])v);
        if (v is T t) return t;
        return (T)Convert.ChangeType(v, typeof(T));
    }
}
}
