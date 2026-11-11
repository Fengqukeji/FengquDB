using System;
using System.IO;
using System.Text;
using System.Collections.Generic;

namespace FengquDB {
public enum QueryOp : uint { True, False, And, Or, Not, Eq, Ne, Lt, Le, Gt, Ge, In,
    Exists, IsNull, Contains, StartsWith, EndsWith, Like, Regex, IContains, ILike, IRegex, Fuzzy }
public enum QueryAggregate : uint { None, CountAll, Count, Sum, Min, Max, Avg }
public enum QueryJoinKind : uint { Inner, Left, Semi, Anti }
public sealed class QueryField {
    public uint Source, Type=4;
    public uint[] Path=Array.Empty<uint>();
    public QueryField() {}
    public QueryField(uint number,uint type,uint source=0) { Source=source;Type=type;Path=new[]{number}; }
    public static QueryField Key(uint source=0)=>new QueryField{Source=source};
    public static QueryField Record(uint source=0)=>new QueryField{Source=source,Type=0};
    public static QueryField Column(uint index)=>new QueryField(index+1,0,255);
    internal byte[] Encode()=>QueryWire.Encode(w=>{QueryWire.UInt(w,1,Source);QueryWire.UInt(w,2,Type);foreach(var n in Path)QueryWire.UInt(w,3,n);});
    public QueryExpr Test(QueryOp op,params object?[] values)=>new QueryExpr{Op=op,Field=this,Values=values};
    public QueryExpr Compare(QueryOp op,QueryField right)=>new QueryExpr{Op=op,Field=this,Right=right};
}
public sealed class QueryExpr {
    public QueryOp Op;
    public QueryField Field=new QueryField();
    public object?[] Values=Array.Empty<object?>();
    public QueryExpr[] Children=Array.Empty<QueryExpr>();
    public QueryField? Right;
    public static QueryExpr All(params QueryExpr[] children)=>new QueryExpr{Op=QueryOp.And,Children=children};
    public static QueryExpr Any(params QueryExpr[] children)=>new QueryExpr{Op=QueryOp.Or,Children=children};
    public static QueryExpr Not(QueryExpr child)=>new QueryExpr{Op=QueryOp.Not,Children=new[]{child}};
    internal byte[] Encode(int depth=0)=>QueryWire.Encode(w=>{
        if(depth>64)throw new ArgumentException("query expression too deep");
        QueryWire.UInt(w,1,(uint)Op);if((uint)Op>=5)QueryWire.Bytes(w,2,Field.Encode());
        foreach(var v in Values)QueryWire.Bytes(w,3,QueryWire.Value(v));
        foreach(var c in Children)QueryWire.Bytes(w,4,c.Encode(depth+1));if(Right!=null)QueryWire.Bytes(w,5,Right.Encode());
    });
}
public sealed class QueryColumn {
    public QueryField Field;public QueryAggregate Aggregate;public string Name="";public bool Distinct;
    public QueryColumn(QueryField field,QueryAggregate aggregate=QueryAggregate.None){Field=field;Aggregate=aggregate;}
    internal byte[] Encode()=>QueryWire.Encode(w=>{QueryWire.Bytes(w,1,Field.Encode());QueryWire.UInt(w,2,(uint)Aggregate);QueryWire.Bytes(w,3,Encoding.UTF8.GetBytes(Name));QueryWire.UInt(w,4,Distinct?1UL:0);});
}
public sealed class QueryOrder {
    public QueryField Field;public bool Descending,NullsFirst=true;
    public QueryOrder(QueryField field,bool descending=false){Field=field;Descending=descending;}
    internal byte[] Encode()=>QueryWire.Encode(w=>{QueryWire.Bytes(w,1,Field.Encode());QueryWire.UInt(w,2,Descending?1UL:0);QueryWire.UInt(w,3,NullsFirst?1UL:0);});
}
public sealed class QueryJoin {
    public byte[] Prefix=Array.Empty<byte>();public QueryField[] Left=Array.Empty<QueryField>(),Right=Array.Empty<QueryField>();
    public QueryJoinKind Kind;public QueryExpr On=new QueryExpr();
    internal byte[] Encode()=>QueryWire.Encode(w=>{QueryWire.Bytes(w,1,Prefix);foreach(var f in Left)QueryWire.Bytes(w,2,f.Encode());foreach(var f in Right)QueryWire.Bytes(w,3,f.Encode());QueryWire.UInt(w,4,(uint)Kind);QueryWire.Bytes(w,5,On.Encode());});
}
public sealed class StoreQuery {
    public byte[] Prefix=Array.Empty<byte>(),After=Array.Empty<byte>(),IndexHint=Array.Empty<byte>();
    public QueryExpr Where=new QueryExpr(),Having=new QueryExpr();
    public QueryColumn[] Columns=Array.Empty<QueryColumn>();public QueryOrder[] Order=Array.Empty<QueryOrder>();
    public QueryField[] Group=Array.Empty<QueryField>();public QueryJoin[] Joins=Array.Empty<QueryJoin>();
    public ulong Limit=100,Offset,MemoryBytes=16UL<<20,MaxCandidates=1000000,MaxWork=100000000,TimeoutMs=1000;
    public bool AllowScan=true,CountTotal,Distinct;
    public byte[] Encode()=>QueryWire.Encode(w=>{
        QueryWire.Bytes(w,1,Prefix);QueryWire.Bytes(w,2,Where.Encode());foreach(var c in Columns)QueryWire.Bytes(w,3,c.Encode());
        foreach(var o in Order)QueryWire.Bytes(w,4,o.Encode());foreach(var g in Group)QueryWire.Bytes(w,5,g.Encode());QueryWire.Bytes(w,6,Having.Encode());foreach(var j in Joins)QueryWire.Bytes(w,7,j.Encode());
        ulong[] values={Limit,Offset,MemoryBytes,MaxCandidates,MaxWork,TimeoutMs,AllowScan?1UL:0,CountTotal?1UL:0,Distinct?1UL:0};for(int i=0;i<values.Length;i++)QueryWire.UInt(w,i+8,values[i]);QueryWire.Bytes(w,17,IndexHint);QueryWire.Bytes(w,18,After);
    });
    public PreparedQuery Prepare()=>new PreparedQuery(Encode());
}
public sealed class PreparedQuery {
    readonly byte[] encoded;
    internal PreparedQuery(byte[] bytes){encoded=bytes;}
    // Store runs on a read-only snapshot; a transaction keeps private-write visibility.
    public StoreQueryResult Execute(Store db,bool explain=false)=>StoreQueryResult.Decode(db.SelectRaw((StoreOperation)(explain?54:53),encoded).Value!);
    public StoreQueryResult Execute(StoreTransaction tx,bool explain=false)=>StoreQueryResult.Decode(tx.Execute((StoreOperation)(explain?54:53),Array.Empty<byte>(),value:encoded).Value!);
}
public sealed class QueryRow {public byte[] Key=Array.Empty<byte>();public List<object?> Values=new List<object?>();}
public sealed class QueryAccess {
    public uint Source;public string Strategy="",Index="",Reason="";public bool Covering,Ordered,NeedsScan;public ulong SampledCandidates;
}
public sealed class QueryStats {public ulong Candidates,Matched,ResultTotal,Work,PeakMemory,PayloadReads,JoinProbes;public bool Complete;}
public sealed class StoreQueryResult {
    public List<QueryRow> Rows=new List<QueryRow>();public List<QueryAccess> Access=new List<QueryAccess>();public QueryStats Stats=new QueryStats();public byte[] Cursor=Array.Empty<byte>();
    internal static StoreQueryResult Decode(byte[] bytes){
        var result=new StoreQueryResult();foreach(var f in StoreFeatureWire.Fields(bytes)){
            if(f.Number==1){var row=new QueryRow();foreach(var p in StoreFeatureWire.Fields(f.Bytes!)){if(p.Number==1)row.Key=p.Bytes!;else if(p.Number==2)row.Values.Add(QueryWire.ReadValue(p.Bytes!));}result.Rows.Add(row);}
            else if(f.Number==2){var a=new QueryAccess();foreach(var p in StoreFeatureWire.Fields(f.Bytes!)){switch(p.Number){case 1:a.Source=(uint)p.Value;break;case 2:a.Strategy=Encoding.UTF8.GetString(p.Bytes!);break;case 3:a.Index=Encoding.UTF8.GetString(p.Bytes!);break;case 4:a.Reason=Encoding.UTF8.GetString(p.Bytes!);break;case 5:a.Covering=p.Value!=0;break;case 6:a.Ordered=p.Value!=0;break;case 7:a.NeedsScan=p.Value!=0;break;case 8:a.SampledCandidates=p.Value;break;}}result.Access.Add(a);}
            else if(f.Number==3){foreach(var p in StoreFeatureWire.Fields(f.Bytes!))switch(p.Number){case 1:result.Stats.Candidates=p.Value;break;case 2:result.Stats.Matched=p.Value;break;case 3:result.Stats.ResultTotal=p.Value;break;case 4:result.Stats.Work=p.Value;break;case 5:result.Stats.PeakMemory=p.Value;break;case 6:result.Stats.PayloadReads=p.Value;break;case 7:result.Stats.JoinProbes=p.Value;break;case 8:result.Stats.Complete=p.Value!=0;break;}}
            else if(f.Number==4)result.Cursor=f.Bytes!;
        }return result;
    }
}
internal static class QueryWire {
    internal static byte[] Encode(Action<BinaryWriter> action){using(var stream=new MemoryStream()){using(var w=new BinaryWriter(stream,Encoding.UTF8,true))action(w);return stream.ToArray();}}
    internal static void UInt(BinaryWriter w,int n,ulong v){StoreFeatureWire.Var(w,(ulong)n*8);StoreFeatureWire.Var(w,v);}
    internal static void Bytes(BinaryWriter w,int n,byte[] v)=>StoreFeatureWire.Bytes(w,n,v);
    internal static byte[] Value(object? value)=>Encode(w=>{
        if(value==null){UInt(w,1,0);return;}if(value is bool b){UInt(w,1,5);UInt(w,2,b?1UL:0);return;}
        if(value is byte[] data){UInt(w,1,4);Bytes(w,5,data);return;}if(value is string s){UInt(w,1,4);Bytes(w,5,Encoding.UTF8.GetBytes(s));return;}
        if(value is double||value is float){double d=Convert.ToDouble(value);if(double.IsNaN(d)||double.IsInfinity(d))throw new ArgumentException("Non-finite query number");UInt(w,1,3);StoreFeatureWire.Var(w,33);w.Write(d);return;}
        if(value is ulong||value is uint||value is ushort||value is byte){UInt(w,1,1);UInt(w,2,Convert.ToUInt64(value));return;}
        if(value is long||value is int||value is short||value is sbyte){long n=Convert.ToInt64(value);UInt(w,1,2);UInt(w,3,unchecked(((ulong)n<<1)^(ulong)(n>>63)));return;}
        throw new ArgumentException("Query literals must be null, bool, integer, float, string or byte[]");
    });
    internal static object? ReadValue(byte[] bytes){ulong kind=0,u=0,i=0,bits=0;byte[] data=Array.Empty<byte>();foreach(var f in StoreFeatureWire.Fields(bytes)){if(f.Number==1)kind=f.Value;else if(f.Number==2)u=f.Value;else if(f.Number==3)i=f.Value;else if(f.Number==4)bits=f.Value;else if(f.Number==5)data=f.Bytes!;}switch(kind){case 0:return null;case 1:return u;case 2:return unchecked((long)(i>>1)^-((long)i&1));case 3:return BitConverter.Int64BitsToDouble(unchecked((long)bits));case 4:return data;case 5:return u!=0;default:throw new InvalidDataException("Invalid query value");}}
}
public sealed partial class Store {
    public StoreQueryResult Select(StoreQuery query)=>query.Prepare().Execute(this);
    public StoreQueryResult Explain(StoreQuery query)=>query.Prepare().Execute(this,true);
}
public sealed partial class StoreTransaction {
    public StoreQueryResult Select(StoreQuery query)=>query.Prepare().Execute(this);
    public StoreQueryResult Explain(StoreQuery query)=>query.Prepare().Execute(this,true);
}
}
