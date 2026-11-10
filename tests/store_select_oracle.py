"""Deterministic native query results checked against SQLite, not its timing."""
import sys,random,sqlite3,re
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"bindings/python"))
from fengqudb import Store
from fengqudb_select import *
from fengqudb_select import _u,_b

def distance(a,b):
    prior=list(range(len(b)+1))
    for i,c in enumerate(a,1):
        row=[i]
        for j,d in enumerate(b,1):row.append(min(prior[j]+1,row[j-1]+1,prior[j-1]+(c!=d)))
        prior=row
    return prior[-1]

def run(path,library):
    root=Path(path);assert not root.exists();root.mkdir(parents=True);rng=random.Random(48572)
    sql=sqlite3.connect(":memory:");sql.execute("PRAGMA case_sensitive_like=ON")
    sql.execute("CREATE TABLE players(id INTEGER,level INTEGER,score INTEGER,name TEXT,category TEXT,flag INTEGER,optional INTEGER)")
    sql.create_function("rx",2,lambda p,v:None if v is None else int(re.search(p,v) is not None))
    sql.create_function("fuzzy",3,lambda p,v,d:None if v is None else int(distance(p,v)<=d))
    fields=[(Field(2,1),"level"),(Field(3,5),"score"),(Field(4,4),"name"),(Field(5,4),"category"),(Field(6,11),"flag"),(Field(7,5),"optional")]
    rows=[];blobs=[]
    for i in range(128):
        level=rng.randrange(50);score=rng.randrange(-100,101);name=("Name-"+str(i)) if i%11 else "张三";category=["red","green","blue"][i%3];flag=i%2==0;optional=None if i%5==0 else i%7-3
        rows.append((i,level,score,name,category,int(flag),optional));v=_u(1,i)+_u(2,level)+_u(3,(score<<1)^(score>>63))+_b(4,name)+_b(5,category)+_u(6,int(flag))
        if optional is not None:v+=_u(7,(optional<<1)^(optional>>63))
        blobs.append((b"p/"+str(i).encode(),v))
    sql.executemany("INSERT INTO players VALUES(?,?,?,?,?,?,?)",rows)
    def leaf():
        field,name=rng.choice(fields)
        if name in ("name","category"):
            op=rng.choice([Op.EQ,Op.NE,Op.IN,Op.CONTAINS,Op.STARTS_WITH,Op.LIKE,Op.REGEX,Op.FUZZY])
            value=rng.choice(["Name-1","red","blue","张三","Name","green"])
            if op==Op.IN:
                values=[value,None] if rng.randrange(2) else [value,value];return field.test(op,*values),f"{name} IN ({','.join('?' for _ in values)})",values
            if op==Op.CONTAINS:return field.test(op,value),f"instr({name},?)>0",[value]
            if op==Op.STARTS_WITH:return field.test(op,value),f"substr({name},1,length(?))=?",[value,value]
            if op==Op.LIKE:return field.test(op,value+"%"),f"{name} LIKE ?",[value+"%"]
            if op==Op.REGEX:return field.test(op,"^"+value+".*$"),f"rx(?,{name})",["^"+value+".*$"]
            if op==Op.FUZZY:return field.test(op,value,1),f"fuzzy(?,{name},1)",[value]
            return field.test(op,value),f"{name} {'=' if op==Op.EQ else '<>'} ?",[value]
        if name=="flag":
            value=bool(rng.randrange(2));return field.test(Op.EQ,value),"flag=?",[int(value)]
        op=rng.choice([Op.EQ,Op.NE,Op.LT,Op.LE,Op.GT,Op.GE,Op.IN,Op.IS_NULL,Op.EXISTS])
        value=rng.randrange(-20,61)
        if op==Op.IS_NULL:return field.test(op),f"{name} IS NULL",[]
        if op==Op.EXISTS:return field.test(op),f"{name} IS NOT NULL",[]
        if op==Op.IN:
            values=rng.choice([[],[value,value+1],[value,None]])
            return field.test(op,*values),f"{name} IN ({','.join('?' for _ in values)})",values
        return field.test(op,value),f"{name} {dict(zip([Op.EQ,Op.NE,Op.LT,Op.LE,Op.GT,Op.GE],['=','<>','<','<=','>','>=']))[op]} ?",[value]
    def tree(depth=0):
        if depth>=3 or rng.randrange(3)==0:return leaf()
        a,sa,pa=tree(depth+1)
        if rng.randrange(4)==0:return ~a,f"NOT ({sa})",pa
        b,sb,pb=tree(depth+1);both=bool(rng.randrange(2));return (a&b if both else a|b),f"({sa}) {'AND' if both else 'OR'} ({sb})",pa+pb
    with Store(root/"main",library=library) as db:
        db.put_many(blobs,force_reliable=True)
        for name,parts in [(b"category-level",[(5,4),(2,1)]),(b"score",[(3,5)]),(b"name",[(4,4)]),(b"flag",[(6,11)])]:db.define_compound_index(name,b"p/",parts,flags=2)
        for case in range(160):
            expr,where,args=tree();limit=rng.randrange(1,25);offset=rng.randrange(8)
            q=Query(b"p/",where=expr,columns=[Column(Field(1,1)),Column(Field(3,5))],order=[Order(Field(3,5),True),Order(Field(1,1))],limit=limit,offset=offset,count_total=True)
            actual=[tuple(r["values"]) for r in q.execute(db).rows]
            expected=sql.execute("SELECT id,score FROM players WHERE "+where+" ORDER BY score DESC,id LIMIT ? OFFSET ?",args+[limit,offset]).fetchall()
            assert actual==expected,(case,where,args,actual,expected)
        q=Query(b"p/",columns=[Column(Field(5,4)),Column(Field(1,1),Aggregate.COUNT_ALL),Column(Field(3,5),Aggregate.SUM),Column(Field(7,5),Aggregate.COUNT),Column(Field(3,5),Aggregate.MIN),Column(Field(3,5),Aggregate.MAX),Column(Field(3,5),Aggregate.AVG)],group=[Field(5,4)],order=[Order(Field.column(0))])
        actual=q.execute(db).rows;expected=sql.execute("SELECT category,count(*),sum(score),count(optional),min(score),max(score),avg(score) FROM players GROUP BY category ORDER BY category").fetchall()
        for a,b in zip(actual,expected):
            values=a["values"];assert values[0].decode()==b[0] and tuple(values[1:6])==b[1:6] and abs(values[6]-b[6])<1e-9
        sql.execute("CREATE TABLE orders(uid INTEGER,amount INTEGER)");orders=[(i%30,i*7) for i in range(90)];sql.executemany("INSERT INTO orders VALUES(?,?)",orders)
        db.put_many([(b"o/"+str(i).encode(),_u(1,u)+_u(2,a)) for i,(u,a) in enumerate(orders)],force_reliable=True)
        q=Query(b"p/",columns=[Column(Field(1,1)),Column(Field(2,1,1))],joins=[Join(b"o/",(Field(1,1),),(Field(1,1,1),),JoinKind.LEFT)],order=[Order(Field(1,1)),Order(Field(2,1,1))],limit=4096)
        expected=sql.execute("SELECT p.id,o.amount FROM players p LEFT JOIN orders o ON p.id=o.uid ORDER BY p.id,o.amount").fetchall()
        for indexed in (False,True):
            if indexed:db.define_compound_index(b"owner",b"o/",[(1,1)],flags=2)
            actual=[tuple(r["values"]) for r in q.execute(db).rows];assert actual==expected,(indexed,actual[:10],expected[:10])
        db.verify()
    sql.close();print("PASS SQLite oracle: 160 nested predicates, typed ranges/patterns/fuzzy, aggregates and hash/index joins")

if __name__=="__main__":run(sys.argv[1],sys.argv[2])
