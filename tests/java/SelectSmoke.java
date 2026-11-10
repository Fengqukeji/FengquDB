import io.fengqudb.Store;
import io.fengqudb.Select;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
public final class SelectSmoke {
    static byte[] b(String s){return s.getBytes(StandardCharsets.UTF_8);}
    static void check(boolean v){if(!v)throw new AssertionError("Java query assertion");}
    public static void main(String[]args){try(Store db=new Store(args[0])){
        Select.Field uid=new Select.Field(1,1),level=new Select.Field(3,1),kind=new Select.Field(2,4);
        Select q=new Select();q.prefix=b("p/");q.where=Select.Expr.all(kind.test(Select.Op.EQ,"mage"),level.test(Select.Op.GE,5),level.test(Select.Op.LE,15));q.columns.add(new Select.Column(uid));q.columns.add(new Select.Column(level));q.order.add(new Select.Order(level,true));q.limit=7;
        Select.Prepared prepared=q.prepare();Select.Result first=prepared.execute(db,false);check(first.rows.size()==7&&first.rows.get(0).values.get(0).bits==55);check(q.explain(db).rows.isEmpty());q.after=first.cursor;check(q.execute(db).rows.size()==7);
        q=new Select();q.prefix=b("p/");q.columns.add(new Select.Column(kind));q.columns.add(new Select.Column(uid,Select.Aggregate.COUNT_ALL));q.columns.add(new Select.Column(new Select.Field(4,5),Select.Aggregate.SUM));q.group.add(kind);q.having=Select.Field.column(1).test(Select.Op.GT,20);q.order.add(new Select.Order(Select.Field.column(0)));check(q.execute(db).rows.size()==2);
        q=new Select();q.prefix=b("p/");q.where=new Select.Field(5,4).test(Select.Op.REGEX,"^Name-1[0-9]$");q.columns.add(new Select.Column(uid));check(q.execute(db).rows.size()==10);
        Select.Join join=new Select.Join();join.prefix=b("o/");join.left.add(uid);join.right.add(new Select.Field(2,1,1));q.joins.add(join);q.where=new Select.Expr();q.columns.add(new Select.Column(new Select.Field(3,1,1)));check(q.execute(db).rows.size()==32);
        try(Store.Transaction tx=db.begin()){check(q.execute(tx).rows.size()==32);}
    }System.out.println("PASS Java query builders, prepared requests, cursor, patterns, aggregates and joins");}
}
