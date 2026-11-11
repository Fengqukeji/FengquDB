using System;
using System.Text;
using System.Runtime.InteropServices;
using FengquDB;
static class SelectSmoke {
    static byte[] B(string s)=>Encoding.UTF8.GetBytes(s);
    static void Check(bool v){if(!v)throw new Exception("C# query assertion");}
    static void Main(string[]args){
        NativeLibrary.SetDllImportResolver(typeof(Store).Assembly,(name,assembly,flags)=>name=="fengqudb"?NativeLibrary.Load(args[1]):IntPtr.Zero);
        using(var db=new Store(args[0])){
            var uid=new QueryField(1,1);var level=new QueryField(3,1);var kind=new QueryField(2,4);
            var q=new StoreQuery{Prefix=B("p/"),Where=QueryExpr.All(kind.Test(QueryOp.Eq,"mage"),level.Test(QueryOp.Ge,5),level.Test(QueryOp.Le,15)),Columns=new[]{new QueryColumn(uid),new QueryColumn(level)},Order=new[]{new QueryOrder(level,true)},Limit=7};
            var prepared=q.Prepare();var first=prepared.Execute(db);Check(first.Rows.Count==7&&(ulong)first.Rows[0].Values[0]! == 55UL);Check(db.Explain(q).Rows.Count==0);
            q.After=first.Cursor;Check(db.Select(q).Rows.Count==7);
            q=new StoreQuery{Prefix=B("p/"),Columns=new[]{new QueryColumn(kind),new QueryColumn(uid,QueryAggregate.CountAll),new QueryColumn(new QueryField(4,5),QueryAggregate.Sum)},Group=new[]{kind},Having=QueryField.Column(1).Test(QueryOp.Gt,20),Order=new[]{new QueryOrder(QueryField.Column(0))}};
            Check(db.Select(q).Rows.Count==2);
            q=new StoreQuery{Prefix=B("p/"),Where=new QueryField(5,4).Test(QueryOp.Regex,"^Name-1[0-9]$"),Columns=new[]{new QueryColumn(uid)}};Check(db.Select(q).Rows.Count==10);
            q.Joins=new[]{new QueryJoin{Prefix=B("o/"),Left=new[]{uid},Right=new[]{new QueryField(2,1,1)}}};q.Where=new QueryExpr();q.Columns=new[]{new QueryColumn(uid),new QueryColumn(new QueryField(3,1,1))};Check(db.Select(q).Rows.Count==32);
            using(var tx=db.Begin()){Check(tx.Select(q).Rows.Count==32);}
            // Typed builder: same request bytes as the hand-written query, typed rows and aggregates.
            var id=Q.U64(1);var lvl=Q.U64(3);var cls=Q.Text(2);var gold=Q.S64(4);var opt=Q.U64(10);
            var typed=Q.From("p/").Where(cls=="mage"&&lvl>=5&&lvl<=15).Select(id,lvl).OrderBy(lvl.Desc()).Limit(7);
            var manual=new StoreQuery{Prefix=B("p/"),Where=QueryExpr.All(kind.Test(QueryOp.Eq,"mage"),level.Test(QueryOp.Ge,5UL),level.Test(QueryOp.Le,15UL)),Columns=new[]{new QueryColumn(uid),new QueryColumn(level)},Order=new[]{new QueryOrder(level,true)},Limit=7};
            Check(typed.Build().Encode().AsSpan().SequenceEqual(manual.Encode()));
            var rows=typed.Run(db);Check(rows.Count==7&&rows[0].Get(id)==55UL);
            foreach(var row in rows)Check(row.Get(lvl)>=5&&row.Get(lvl)<=15&&row.Get(id)%2==1);
            Check(typed.Prepare().Run(db).Count==7);
            var groups=Q.From("p/").GroupBy(cls).Select(cls,Q.Count(),Q.Sum(gold)).Having(Q.Sum(gold)>8000L).OrderBy(cls).Run(db);
            Check(groups.Count==1&&groups[0].Get(cls)=="mage"&&groups[0].Get(Q.Count())==32UL&&groups[0].Get(Q.Sum(gold))==8064L);
            var byAvg=Q.From("p/").GroupBy(cls).Select(cls,Q.Avg(gold)).OrderBy(Q.Avg(gold).Desc()).Run(db);
            // The shared fixture also holds a "new" class from the Python run; check ordering, not names.
            Check(byAvg.Count>=2);for(int i=1;i<byAvg.Count;i++)Check(byAvg[i-1].Get(Q.Avg(gold))>=byAvg[i].Get(Q.Avg(gold)));
            var missing=Q.From("p/").Where(opt.IsNull()).Select(id,opt).Limit(100).Run(db);
            Check(missing.Count==22&&!missing[0].TryGet(opt,out _));
            Check(Q.From("p/").Where(id.In(3,8,3)).Select(id).Run(db).Count==2);
            Check(Q.From("p/").Where(!(cls=="mage")).Select(id).Run(db).Count==32);
            var page=Q.From("p/").Select(id).OrderBy(gold.Desc()).Limit(9);var seen=new System.Collections.Generic.List<ulong>();
            for(;;){var r=page.Run(db);foreach(var row in r)seen.Add(row.Get(id));if(r.Cursor.Length==0)break;page.After(r.Cursor);}
            Check(seen.Count==64&&seen[0]==63&&seen[63]==0);
            try{Q.From("p/").Where(Q.Count()>1UL);Check(false);}catch(ArgumentException){}
        }
        Console.WriteLine("PASS C# query builders, prepared requests, cursor, patterns, aggregates and joins");
    }
}
