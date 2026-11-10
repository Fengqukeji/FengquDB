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
        }
        Console.WriteLine("PASS C# query builders, prepared requests, cursor, patterns, aggregates and joins");
    }
}
