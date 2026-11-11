package io.fengqudb;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.Consumer;

/** Typed queries executed by the native core. Byte strings and unsigned longs
 * keep their wire representation; use Long.toUnsignedString for uint64 results. */
public final class Select {
    public enum Op { TRUE,FALSE,AND,OR,NOT,EQ,NE,LT,LE,GT,GE,IN,EXISTS,IS_NULL,
        CONTAINS,STARTS_WITH,ENDS_WITH,LIKE,REGEX,ICONTAINS,ILIKE,IREGEX,FUZZY }
    public enum Aggregate { NONE,COUNT_ALL,COUNT,SUM,MIN,MAX,AVG }
    public enum JoinKind { INNER,LEFT,SEMI,ANTI }
    public static final class Value {
        public int kind;public long bits;public double real;public byte[] bytes=new byte[0];
        public static Value uintBits(long bits){Value v=new Value();v.kind=1;v.bits=bits;return v;}
        public static Value of(Object x){Value v=new Value();if(x==null)return v;if(x instanceof Value)return (Value)x;
            if(x instanceof Boolean){v.kind=5;v.bits=(Boolean)x?1:0;}
            else if(x instanceof byte[]){v.kind=4;v.bytes=((byte[])x).clone();}
            else if(x instanceof String){v.kind=4;v.bytes=((String)x).getBytes(StandardCharsets.UTF_8);}
            else if(x instanceof Float||x instanceof Double){v.kind=3;v.real=((Number)x).doubleValue();if(!Double.isFinite(v.real))throw new IllegalArgumentException("Non-finite query number");}
            else if(x instanceof Byte||x instanceof Short||x instanceof Integer||x instanceof Long){v.kind=2;v.bits=((Number)x).longValue();}
            else throw new IllegalArgumentException("Unsupported query literal");return v;}
        byte[] encode(){return wire(w->{w.u(1,kind);if(kind==1||kind==5)w.u(2,bits);else if(kind==2)w.u(3,(bits<<1)^(bits>>63));else if(kind==3)w.fixed(4,Double.doubleToRawLongBits(real));else if(kind==4)w.b(5,bytes);});}
        public String text(){return new String(bytes,StandardCharsets.UTF_8);}
        public String unsignedText(){return Long.toUnsignedString(bits);}
    }
    public static final class Field {
        public int source,type=4;public int[] path=new int[0];
        // true：path 指向 protobuf repeated 字段，条件在任一元素满足时为真（仅主源 WHERE 叶子条件）。
        public boolean repeated;
        public Field(){}
        public Field(int number,int type){this(number,type,0);}
        public Field(int number,int type,int source){this.source=source;this.type=type;path=new int[]{number};}
        public static Field key(int source){Field f=new Field();f.source=source;return f;}
        public static Field record(int source){Field f=key(source);f.type=0;return f;}
        public static Field column(int index){return new Field(index+1,0,255);}
        public Field any(){Field f=new Field();f.source=source;f.type=type;f.path=path;f.repeated=true;return f;}
        byte[] encode(){return wire(w->{w.u(1,source);w.u(2,type);for(int n:path)w.u(3,n);if(repeated)w.u(4,1);});}
        public Expr test(Op op,Object... values){Expr e=new Expr();e.op=op;e.field=this;for(Object v:values)e.values.add(Value.of(v));return e;}
        public Expr compare(Op op,Field right){Expr e=new Expr();e.op=op;e.field=this;e.right=right;return e;}
    }
    public static final class Expr {
        public Op op=Op.TRUE;public Field field=new Field(),right;
        public List<Value> values=new ArrayList<>();public List<Expr> children=new ArrayList<>();
        public static Expr all(Expr... children){Expr e=new Expr();e.op=Op.AND;e.children.addAll(Arrays.asList(children));return e;}
        public static Expr any(Expr... children){Expr e=new Expr();e.op=Op.OR;e.children.addAll(Arrays.asList(children));return e;}
        public static Expr not(Expr child){Expr e=new Expr();e.op=Op.NOT;e.children.add(child);return e;}
        byte[] encode(){return encode(0);} byte[] encode(int depth){if(depth>64)throw new IllegalArgumentException("query expression too deep");return wire(w->{w.u(1,op.ordinal());if(op.ordinal()>=5)w.b(2,field.encode());for(Value v:values)w.b(3,v.encode());for(Expr c:children)w.b(4,c.encode(depth+1));if(right!=null)w.b(5,right.encode());});}
    }
    public static final class Column {
        public Field field;public Aggregate aggregate=Aggregate.NONE;public String name="";public boolean distinct;
        public Column(Field f){field=f;}public Column(Field f,Aggregate a){field=f;aggregate=a;}
        byte[] encode(){return wire(w->{w.b(1,field.encode());w.u(2,aggregate.ordinal());w.b(3,name.getBytes(StandardCharsets.UTF_8));w.u(4,distinct?1:0);});}
    }
    public static final class Order {
        public Field field;public boolean descending,nullsFirst=true;
        public Order(Field f){field=f;}public Order(Field f,boolean desc){field=f;descending=desc;}
        byte[] encode(){return wire(w->{w.b(1,field.encode());w.u(2,descending?1:0);w.u(3,nullsFirst?1:0);});}
    }
    public static final class Join {
        public byte[] prefix=new byte[0];public List<Field> left=new ArrayList<>(),right=new ArrayList<>();
        public JoinKind kind=JoinKind.INNER;public Expr on=new Expr();
        byte[] encode(){return wire(w->{w.b(1,prefix);for(Field f:left)w.b(2,f.encode());for(Field f:right)w.b(3,f.encode());w.u(4,kind.ordinal());w.b(5,on.encode());});}
    }
    public byte[] prefix=new byte[0],after=new byte[0],indexHint=new byte[0];
    public Expr where=new Expr(),having=new Expr();public List<Column> columns=new ArrayList<>();
    public List<Order> order=new ArrayList<>();public List<Field> group=new ArrayList<>();public List<Join> joins=new ArrayList<>();
    public long limit=100,offset=0,memoryBytes=16L<<20,maxCandidates=1000000,maxWork=100000000,timeoutMs=1000;
    public boolean allowScan=true,countTotal,distinct;
    public byte[] encode(){return wire(w->{w.b(1,prefix);w.b(2,where.encode());for(Column c:columns)w.b(3,c.encode());for(Order o:order)w.b(4,o.encode());for(Field f:group)w.b(5,f.encode());w.b(6,having.encode());for(Join j:joins)w.b(7,j.encode());long[] values={limit,offset,memoryBytes,maxCandidates,maxWork,timeoutMs,allowScan?1:0,countTotal?1:0,distinct?1:0};for(int i=0;i<values.length;i++){if(values[i]<0)throw new IllegalArgumentException("Negative query limit");w.u(i+8,values[i]);}w.b(17,indexHint);w.b(18,after);});}
    public Prepared prepare(){return new Prepared(encode());}
    public Result execute(Store db){return prepare().execute(db,false);}
    public Result execute(Store.Transaction tx){return prepare().execute(tx,false);}
    public Result explain(Store db){return prepare().execute(db,true);}
    public Result explain(Store.Transaction tx){return prepare().execute(tx,true);}
    public static final class Prepared {
        private final byte[] bytes;private Prepared(byte[] b){bytes=b;}
        // Store runs on a read-only snapshot; a transaction keeps private-write visibility.
public Result execute(Store db,boolean explain){return new Result(db.selectRaw(explain?54:53,bytes).value);}
        public Result execute(Store.Transaction tx,boolean explain){return new Result(tx.execute(explain?54:53,new byte[0],null,bytes).value);}
    }
    public static final class Row {public byte[] key=new byte[0];public List<Value> values=new ArrayList<>();}
    public static final class Access {public long source,sampledCandidates;public String strategy="",index="",reason="";public boolean covering,ordered,needsScan;}
    public static final class Stats {public long candidates,matched,resultTotal,work,peakMemory,payloadReads,joinProbes;public boolean complete;}
    public static final class Result {
        public List<Row> rows=new ArrayList<>();public List<Access> access=new ArrayList<>();public Stats stats=new Stats();public byte[] cursor=new byte[0];
        private Result(byte[] bytes){Reader r=new Reader(bytes);while(r.next()){
            if(r.field==1){Row row=new Row();Reader a=new Reader(r.bytes);while(a.next()){if(a.field==1)row.key=a.bytes;else if(a.field==2){Value v=new Value();Reader p=new Reader(a.bytes);while(p.next()){if(p.field==1)v.kind=(int)p.number;else if(p.field==2)v.bits=p.number;else if(p.field==3)v.bits=(p.number>>>1)^-(p.number&1);else if(p.field==4)v.real=Double.longBitsToDouble(p.number);else if(p.field==5)v.bytes=p.bytes;}row.values.add(v);}}rows.add(row);}
            else if(r.field==2){Access x=new Access();Reader a=new Reader(r.bytes);while(a.next()){switch(a.field){case 1:x.source=a.number;break;case 2:x.strategy=new String(a.bytes,StandardCharsets.UTF_8);break;case 3:x.index=new String(a.bytes,StandardCharsets.UTF_8);break;case 4:x.reason=new String(a.bytes,StandardCharsets.UTF_8);break;case 5:x.covering=a.number!=0;break;case 6:x.ordered=a.number!=0;break;case 7:x.needsScan=a.number!=0;break;case 8:x.sampledCandidates=a.number;break;default:break;}}access.add(x);}
            else if(r.field==3){Reader a=new Reader(r.bytes);while(a.next())switch(a.field){case 1:stats.candidates=a.number;break;case 2:stats.matched=a.number;break;case 3:stats.resultTotal=a.number;break;case 4:stats.work=a.number;break;case 5:stats.peakMemory=a.number;break;case 6:stats.payloadReads=a.number;break;case 7:stats.joinProbes=a.number;break;case 8:stats.complete=a.number!=0;break;default:break;}}
            else if(r.field==4)cursor=r.bytes;
        }}
    }
    private static byte[] wire(Consumer<Wire> action){Wire w=new Wire();action.accept(w);return w.out.toByteArray();}
    private static final class Wire {
        final ByteArrayOutputStream out=new ByteArrayOutputStream();
        void var(long v){while((v&~127L)!=0){out.write((int)(v&127)|128);v>>>=7;}out.write((int)v);}
        void u(int n,long v){var(n*8L);var(v);}void b(int n,byte[] data){var(n*8L+2);var(data.length);out.write(data,0,data.length);}
        void fixed(int n,long v){var(n*8L+1);for(int i=0;i<8;i++){out.write((int)v&255);v>>>=8;}}
    }
    private static final class Reader {
        final byte[] data;int at,field;long number;byte[] bytes;
        Reader(byte[] b){if(b==null)throw new IllegalStateException("Missing query response");data=b;}
        long var(){long n=0;for(int i=0;i<10;i++){if(at>=data.length)throw new IllegalStateException("Truncated query response");int b=data[at++]&255;if(i==9&&b>1)throw new IllegalStateException("Invalid varint");n|=(long)(b&127)<<(i*7);if((b&128)==0)return n;}throw new IllegalStateException("Invalid varint");}
        boolean next(){if(at==data.length)return false;long tag=var();field=(int)(tag>>>3);int kind=(int)(tag&7);bytes=null;number=0;if(field==0)throw new IllegalStateException("Invalid field");if(kind==0)number=var();else if(kind==1||kind==5){int n=kind==1?8:4;if(n>data.length-at)throw new IllegalStateException("Truncated fixed value");for(int i=0;i<n;i++)number|=(long)(data[at++]&255)<<(i*8);}else if(kind==2){long n=var();if(n<0||n>data.length-at)throw new IllegalStateException("Truncated query bytes");bytes=Arrays.copyOfRange(data,at,at+(int)n);at+=(int)n;}else throw new IllegalStateException("Invalid query wire type");return true;}
    }
}
