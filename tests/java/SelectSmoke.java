import io.fengqudb.Store;
import io.fengqudb.Select;
import io.fengqudb.Q;
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
        // Typed builder: same request bytes as the hand-written query, typed rows and aggregates.
        Q.Col<Long> id=Q.u64(1),lvl=Q.u64(3),gold=Q.s64(4),opt=Q.u64(10);Q.Col<String> cls=Q.text(2);
        Q.Builder typed=Q.from("p/").where(cls.eq("mage").and(lvl.ge(5L)).and(lvl.le(15L))).select(id,lvl).orderBy(lvl.desc()).limit(7);
        Select manual=new Select();manual.prefix=b("p/");manual.where=Select.Expr.all(kind.test(Select.Op.EQ,"mage"),level.test(Select.Op.GE,5L),level.test(Select.Op.LE,15L));manual.columns.add(new Select.Column(uid));manual.columns.add(new Select.Column(level));manual.order.add(new Select.Order(level,true));manual.limit=7;
        check(Arrays.equals(typed.build().encode(),manual.encode()));
        Q.Rows rows=typed.run(db);check(rows.size()==7&&rows.get(0).get(id)==55L);
        for(Q.Row row:rows)check(row.get(lvl)>=5&&row.get(lvl)<=15&&row.get(id)%2==1);
        check(typed.prepare().run(db).size()==7);
        Q.Rows groups=Q.from("p/").groupBy(cls).select(cls,Q.count(),Q.sum(gold)).having(Q.sum(gold).gt(8000L)).orderBy(cls).run(db);
        check(groups.size()==1&&groups.get(0).get(cls).equals("mage")&&groups.get(0).get(Q.count())==32L&&groups.get(0).get(Q.sum(gold))==8064L);
        Q.Rows byAvg=Q.from("p/").groupBy(cls).select(cls,Q.avg(gold)).orderBy(Q.avg(gold).desc()).run(db);
        check(byAvg.size()>=2);for(int i=1;i<byAvg.size();i++)check(byAvg.get(i-1).get(Q.avg(gold))>=byAvg.get(i).get(Q.avg(gold)));
        Q.Rows missing=Q.from("p/").where(opt.isNull()).select(id,opt).limit(100).run(db);
        check(missing.size()==22&&missing.get(0).get(opt)==null);
        check(Q.from("p/").where(id.in(3L,8L,3L)).select(id).run(db).size()==2);
        check(Q.from("p/").where(cls.eq("mage").not()).select(id).run(db).size()==32);
        Q.Builder page=Q.from("p/").select(id).orderBy(gold.desc()).limit(9);java.util.List<Long> seen=new java.util.ArrayList<>();
        for(;;){Q.Rows r=page.run(db);for(Q.Row row:r)seen.add(row.get(id));if(r.cursor().length==0)break;page.after(r.cursor());}
        check(seen.size()==64&&seen.get(0)==63L&&seen.get(63)==0L);
        try{Q.from("p/").where(Q.count().gt(1L));check(false);}catch(IllegalArgumentException e){}
    }System.out.println("PASS Java query builders, prepared requests, cursor, patterns, aggregates and joins");}
}
