package io.fengqudb;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Iterator;
import java.util.List;
import java.util.NoSuchElementException;
import java.util.function.Function;

/**
 * Typed, concise query builder over {@link Select}. Declare each protobuf field once:
 *
 * <pre>
 * Q.Col&lt;Long&gt; id = Q.u64(1), level = Q.u64(3), gold = Q.s64(4);
 * Q.Col&lt;String&gt; cls = Q.text(2);
 * Q.Rows rows = Q.from("p/").where(cls.eq("mage").and(level.between(20L, 60L)).and(gold.ge(1000L)))
 *     .select(id, gold).orderBy(gold.desc(), id).limit(20).run(db);
 * for (Q.Row row : rows) use(row.get(id), row.get(gold));   // null for a missing field
 * </pre>
 *
 * Only the existing request is produced; semantics are in docs/STORE_SELECT.md. uint64 values
 * above Long.MAX_VALUE read as negative longs (use Long.toUnsignedString). A Store runs on a
 * read-only snapshot; a transaction sees its private writes.
 */
public final class Q {
    private Q() {}

    /** A condition resolved against the selected columns when the query is built. */
    public static final class Cond {
        final Function<List<Select.Column>, Select.Expr> build;
        final boolean aggregate;
        Cond(Function<List<Select.Column>, Select.Expr> build, boolean aggregate) {
            this.build = build;
            this.aggregate = aggregate;
        }
        private Cond merge(Select.Op op, Cond other) {
            return new Cond(c -> {
                Select.Expr out = new Select.Expr();
                out.op = op;
                for (Select.Expr e : new Select.Expr[] {build.apply(c), other.build.apply(c)})
                    if (e.op == op) out.children.addAll(e.children); else out.children.add(e);
                return out;
            }, aggregate || other.aggregate);
        }
        public Cond and(Cond other) { return merge(Select.Op.AND, other); }
        public Cond or(Cond other) { return merge(Select.Op.OR, other); }
        public Cond not() { return new Cond(c -> Select.Expr.not(build.apply(c)), aggregate); }
    }

    /** A sort item; aggregates resolve to their output-column position. */
    public static final class Sort {
        final Function<List<Select.Column>, Select.Order> build;
        Sort(Function<List<Select.Column>, Select.Order> build) { this.build = build; }
    }

    interface Item { Select.Column column(); }

    static boolean same(Select.Field a, Select.Field b) {
        return a.source == b.source && a.type == b.type && Arrays.equals(a.path, b.path);
    }
    static boolean same(Select.Column a, Select.Column b) {
        return same(a.field, b.field) && a.aggregate == b.aggregate && a.distinct == b.distinct;
    }
    static int index(List<Select.Column> columns, Select.Column c) {
        for (int i = 0; i < columns.size(); i++)
            if (same(columns.get(i), c)) return i;
        throw new IllegalArgumentException("Aggregate used in having/orderBy must also be selected");
    }
    static Select.Order order(Select.Field f, boolean descending, boolean nullsFirst) {
        Select.Order o = new Select.Order(f, descending);
        o.nullsFirst = nullsFirst;
        return o;
    }

    /** A typed field of one source (0 = main prefix, 1..7 = joins in order). */
    public static final class Col<T> implements Item {
        public final Select.Field field;
        final Class<T> kind;
        Col(Select.Field field, Class<T> kind) { this.field = field; this.kind = kind; }
        public Select.Column column() { return new Select.Column(field); }
        private Cond test(Select.Op op, Object... values) {
            return new Cond(c -> field.test(op, values), false);
        }
        private Cond cmp(Select.Op op, Col<T> other) {
            return new Cond(c -> field.compare(op, other.field), false);
        }
        public Cond eq(T v) { return test(Select.Op.EQ, v); }
        public Cond ne(T v) { return test(Select.Op.NE, v); }
        public Cond lt(T v) { return test(Select.Op.LT, v); }
        public Cond le(T v) { return test(Select.Op.LE, v); }
        public Cond gt(T v) { return test(Select.Op.GT, v); }
        public Cond ge(T v) { return test(Select.Op.GE, v); }
        public Cond eq(Col<T> o) { return cmp(Select.Op.EQ, o); }
        public Cond lt(Col<T> o) { return cmp(Select.Op.LT, o); }
        public Cond gt(Col<T> o) { return cmp(Select.Op.GT, o); }
        public Cond between(T low, T high) { return ge(low).and(le(high)); }
        @SafeVarargs public final Cond in(T... values) { return test(Select.Op.IN, (Object[]) values); }
        public Cond isNull() { return test(Select.Op.IS_NULL); }
        public Cond exists() { return test(Select.Op.EXISTS); }
        public Cond contains(String s) { return test(Select.Op.CONTAINS, s); }
        public Cond startsWith(String s) { return test(Select.Op.STARTS_WITH, s); }
        public Cond endsWith(String s) { return test(Select.Op.ENDS_WITH, s); }
        public Cond like(String s) { return test(Select.Op.LIKE, s); }
        public Cond regex(String s) { return test(Select.Op.REGEX, s); }
        public Cond icontains(String s) { return test(Select.Op.ICONTAINS, s); }
        public Cond ilike(String s) { return test(Select.Op.ILIKE, s); }
        /** Whole-field UTF-8 edit distance, 0..8. */
        public Cond fuzzy(String s, int distance) { return test(Select.Op.FUZZY, s, distance); }
        public Sort asc() { return new Sort(c -> order(field, false, true)); }
        public Sort desc() { return new Sort(c -> order(field, true, true)); }
    }

    /** An aggregate output column; comparisons are valid only in having(). */
    public static final class Agg<T> implements Item {
        final Select.Column column;
        final Class<T> kind;
        Agg(Select.Column column, Class<T> kind) { this.column = column; this.kind = kind; }
        public Select.Column column() { return column; }
        private Cond cmp(Select.Op op, T v) {
            return new Cond(c -> Select.Field.column(index(c, column)).test(op, v), true);
        }
        public Cond eq(T v) { return cmp(Select.Op.EQ, v); }
        public Cond ne(T v) { return cmp(Select.Op.NE, v); }
        public Cond lt(T v) { return cmp(Select.Op.LT, v); }
        public Cond le(T v) { return cmp(Select.Op.LE, v); }
        public Cond gt(T v) { return cmp(Select.Op.GT, v); }
        public Cond ge(T v) { return cmp(Select.Op.GE, v); }
        public Sort asc() { return new Sort(c -> order(Select.Field.column(index(c, column)), false, true)); }
        public Sort desc() { return new Sort(c -> order(Select.Field.column(index(c, column)), true, true)); }
    }

    // Field factories: the name is the protobuf wire type.
    public static Col<Long> u64(int n) { return u64(n, 0); }
    public static Col<Long> u64(int n, int source) { return new Col<>(new Select.Field(n, 1, source), Long.class); }
    public static Col<Long> i64(int n) { return new Col<>(new Select.Field(n, 2, 0), Long.class); }
    public static Col<Double> f64(int n) { return new Col<>(new Select.Field(n, 3, 0), Double.class); }
    public static Col<String> text(int n) { return text(n, 0); }
    public static Col<String> text(int n, int source) { return new Col<>(new Select.Field(n, 4, source), String.class); }
    public static Col<Long> s64(int n) { return s64(n, 0); }
    public static Col<Long> s64(int n, int source) { return new Col<>(new Select.Field(n, 5, source), Long.class); }
    public static Col<Double> f32(int n) { return new Col<>(new Select.Field(n, 6, 0), Double.class); }
    public static Col<Long> fixed64(int n) { return new Col<>(new Select.Field(n, 7, 0), Long.class); }
    public static Col<Long> sfixed64(int n) { return new Col<>(new Select.Field(n, 8, 0), Long.class); }
    public static Col<Long> fixed32(int n) { return new Col<>(new Select.Field(n, 9, 0), Long.class); }
    public static Col<Long> sfixed32(int n) { return new Col<>(new Select.Field(n, 10, 0), Long.class); }
    public static Col<Boolean> bool(int n) { return new Col<>(new Select.Field(n, 11, 0), Boolean.class); }
    public static Col<String> key(int source) { return new Col<>(Select.Field.key(source), String.class); }

    public static Agg<Long> count() {
        return new Agg<>(new Select.Column(Select.Field.key(0), Select.Aggregate.COUNT_ALL), Long.class);
    }
    public static Agg<Long> count(Col<?> c) {
        return new Agg<>(new Select.Column(c.field, Select.Aggregate.COUNT), Long.class);
    }
    public static <T> Agg<T> sum(Col<T> c) { return new Agg<>(new Select.Column(c.field, Select.Aggregate.SUM), c.kind); }
    public static Agg<Double> avg(Col<?> c) {
        return new Agg<>(new Select.Column(c.field, Select.Aggregate.AVG), Double.class);
    }
    public static <T> Agg<T> min(Col<T> c) { return new Agg<>(new Select.Column(c.field, Select.Aggregate.MIN), c.kind); }
    public static <T> Agg<T> max(Col<T> c) { return new Agg<>(new Select.Column(c.field, Select.Aggregate.MAX), c.kind); }

    public static Builder from(String prefix) { return new Builder(prefix.getBytes(java.nio.charset.StandardCharsets.UTF_8)); }
    public static Builder from(byte[] prefix) { return new Builder(prefix.clone()); }

    public static final class Builder {
        private final byte[] prefix;
        private Cond where, having;
        private final List<Select.Column> columns = new ArrayList<>();
        private final List<Sort> order = new ArrayList<>();
        private final List<Select.Field> group = new ArrayList<>();
        private final List<Select.Join> joins = new ArrayList<>();
        private long limit = 100, offset;
        private byte[] after = new byte[0];
        private boolean distinct, countTotal, allowScan = true;
        Builder(byte[] prefix) { this.prefix = prefix; }
        public Builder where(Cond c) {
            if (c.aggregate) throw new IllegalArgumentException("Aggregates belong in having()");
            where = c;
            return this;
        }
        /** Accepts Col and Agg items. */
        public Builder select(Object... items) {
            columns.clear();
            for (Object i : items) columns.add(((Item) i).column());
            return this;
        }
        public Builder groupBy(Col<?>... cols) {
            group.clear();
            for (Col<?> c : cols) group.add(c.field);
            return this;
        }
        public Builder having(Cond c) { having = c; return this; }
        /** Accepts Sort items and plain Col (ascending). */
        public Builder orderBy(Object... items) {
            order.clear();
            for (Object i : items) order.add(i instanceof Sort ? (Sort) i : ((Col<?>) i).asc());
            return this;
        }
        public <T> Builder join(String prefix, Col<T> left, Col<T> right, Select.JoinKind kind) {
            Select.Join j = new Select.Join();
            j.prefix = prefix.getBytes(java.nio.charset.StandardCharsets.UTF_8);
            j.left.add(left.field);
            j.right.add(right.field);
            j.kind = kind;
            joins.add(j);
            return this;
        }
        public <T> Builder join(String prefix, Col<T> left, Col<T> right) {
            return join(prefix, left, right, Select.JoinKind.INNER);
        }
        public Builder limit(long n) { limit = n; return this; }
        public Builder offset(long n) { offset = n; return this; }
        /** Continue after the cursor returned by Rows.cursor(). */
        public Builder after(byte[] cursor) { after = cursor.clone(); return this; }
        public Builder distinct(boolean on) { distinct = on; return this; }
        public Builder countTotal(boolean on) { countTotal = on; return this; }
        public Builder allowScan(boolean on) { allowScan = on; return this; }
        /** The equivalent low-level request. */
        public Select build() {
            Select q = new Select();
            q.prefix = prefix;
            q.columns.addAll(columns);
            q.group.addAll(group);
            q.joins.addAll(joins);
            if (where != null) q.where = where.build.apply(columns);
            if (having != null) q.having = having.build.apply(columns);
            for (Sort s : order) q.order.add(s.build.apply(columns));
            q.limit = limit;
            q.offset = offset;
            q.after = after;
            q.distinct = distinct;
            q.countTotal = countTotal;
            q.allowScan = allowScan;
            return q;
        }
        public Prepared prepare() { return new Prepared(build().prepare(), new ArrayList<>(columns)); }
        public Rows run(Store db) { return new Rows(columns, build().execute(db)); }
        public Rows run(Store.Transaction tx) { return new Rows(columns, build().execute(tx)); }
    }

    /** Encodes the request once for repeated execution. */
    public static final class Prepared {
        private final Select.Prepared prepared;
        private final List<Select.Column> columns;
        Prepared(Select.Prepared p, List<Select.Column> c) { prepared = p; columns = c; }
        public Rows run(Store db) { return new Rows(columns, prepared.execute(db, false)); }
        public Rows run(Store.Transaction tx) { return new Rows(columns, prepared.execute(tx, false)); }
    }

    public static final class Rows implements Iterable<Row> {
        private final List<Select.Column> columns;
        public final Select.Result raw;
        Rows(List<Select.Column> columns, Select.Result raw) { this.columns = new ArrayList<>(columns); this.raw = raw; }
        int indexOf(Select.Column c) {
            for (int i = 0; i < columns.size(); i++)
                if (same(columns.get(i), c)) return i;
            throw new NoSuchElementException("Column was not selected");
        }
        public int size() { return raw.rows.size(); }
        public Row get(int i) { return new Row(this, raw.rows.get(i)); }
        /** Non-empty when another page exists; pass it to Builder.after(). */
        public byte[] cursor() { return raw.cursor; }
        public Iterator<Row> iterator() {
            Iterator<Select.Row> it = raw.rows.iterator();
            return new Iterator<Row>() {
                public boolean hasNext() { return it.hasNext(); }
                public Row next() { return new Row(Rows.this, it.next()); }
            };
        }
    }

    public static final class Row {
        private final Rows owner;
        private final Select.Row row;
        Row(Rows owner, Select.Row row) { this.owner = owner; this.row = row; }
        public byte[] key() { return row.key; }
        /** Returns null for a missing field (SQL NULL). */
        public <T> T get(Col<T> c) { return convert(row.values.get(owner.indexOf(c.column())), c.kind); }
        public <T> T get(Agg<T> a) { return convert(row.values.get(owner.indexOf(a.column)), a.kind); }
        private static <T> T convert(Select.Value v, Class<T> kind) {
            if (v.kind == 0) return null;
            Object out;
            if (kind == String.class) out = v.text();
            else if (kind == Boolean.class) out = v.bits != 0;
            else if (kind == Double.class) out = v.kind == 3 ? v.real : (double) v.bits;
            else if (v.kind == 3) throw new IllegalStateException("Real result read as integer column");
            else out = v.bits;
            return kind.cast(out);
        }
    }
}
