"""Public Python query regressions; caller supplies a fresh, owned fixture path."""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"bindings/python"))
from fengqudb import Store, Error
from fengqudb_select import Query, Field, Expr, Op, Column, Order, Join, JoinKind, Aggregate, _u, _b

def document(i):
    return (_u(1,i)+_b(2,b"mage" if i%2 else b"warrior")+_u(3,i%20)+
            _u(4,((i*11-100)<<1)^((i*11-100)>>63))+_b(5,("Name-"+str(i)).encode())+
            _b(6,b"x"*4096)+(_u(10,i) if i%3 else b"")+_b(8,_u(1,i+1)))

def run(root,library):
    root=Path(root);assert not root.exists();root.mkdir(parents=True)
    with Store(root/"main",library=library) as db:
        db.put_many([(b"p/"+str(i).encode(),document(i)) for i in range(64)],force_reliable=True)
        db.define_compound_index(b"class-level",b"p/",[(2,4),(3,1)],flags=2)
        db.define_compound_index(b"uid",b"p/",[(1,1)],flags=3)
        level=Field(3,1);gold=Field(4,5);uid=Field(1,1);name=Field(5,4);kind=Field(2,4)
        q=Query(b"p/",where=kind.test(Op.EQ,b"mage")&level.test(Op.GE,5)&level.test(Op.LE,15),
                columns=[Column(uid),Column(level)],order=[Order(level,True)],limit=7,count_total=True)
        result=q.execute(db);expected=sorted([i for i in range(64) if i%2 and 5<=i%20<=15],key=lambda i:(i%20,b"p/"+str(i).encode()),reverse=True)
        assert [r["values"][0] for r in result.rows]==expected[:7],(result.rows,expected)
        assert any(a.get("index")=="class-level" for a in result.access)
        assert q.explain(db).rows==[]
        values=[]
        while True:
            page=q.execute(db);values += [r["values"][0] for r in page.rows]
            if not page.cursor: break
            q.after=page.cursor
        assert values==expected,(values,expected)
        q=Query(b"p/",where=(kind.test(Op.EQ,b"mage")|kind.test(Op.EQ,b"warrior"))&~gold.test(Op.LT,0)&uid.test(Op.IN,10,20,30),columns=[Column(uid)],order=[Order(uid)])
        assert [r["values"][0] for r in q.execute(db).rows]==[10,20,30]
        for op,pattern,expected_ids in [(Op.REGEX,b"^Name-(?:1[0-9]|2[0-3])$",list(range(10,24))),
                                       (Op.LIKE,b"Name-1_",list(range(10,20))),
                                       (Op.ICONTAINS,b"NAME-63",[63])]:
            q=Query(b"p/",where=name.test(op,pattern),columns=[Column(uid)],order=[Order(uid)])
            assert [r["values"][0] for r in q.execute(db).rows]==expected_ids,(op,q.execute(db).rows)
        q=Query(b"p/",where=~Field(10,1).test(Op.EQ,3),columns=[Column(uid)])
        assert all(r["values"][0]%3 for r in q.execute(db).rows)
        q=Query(b"p/",columns=[Column(Field(0,1,path=(8,1)))],limit=1)
        assert q.execute(db).rows[0]["values"]==[1]
        q=Query(b"p/",columns=[Column(kind),Column(uid,Aggregate.COUNT_ALL),Column(gold,Aggregate.SUM),Column(gold,Aggregate.MIN),Column(gold,Aggregate.MAX),Column(gold,Aggregate.AVG)],group=[kind],order=[Order(Field.column(0))],having=Field.column(1).test(Op.GT,20))
        result=q.execute(db)
        assert len(result.rows)==2
        for row in result.rows:
            c,count,total,minimum,maximum,avg=row["values"];items=[i*11-100 for i in range(64) if (b"mage" if i%2 else b"warrior")==c]
            assert (count,total,minimum,maximum)==(len(items),sum(items),min(items),max(items))
            assert abs(avg-sum(items)/len(items))<1e-9
        db.put_many([(b"o/"+str(i).encode(),_u(1,i)+_u(2,i%16)+_u(3,i*10)) for i in range(32)],force_reliable=True)
        db.define_compound_index(b"order-owner",b"o/",[(2,1)],flags=2)
        join=Join(b"o/",(uid,),(Field(2,1,1),))
        q=Query(b"p/",columns=[Column(uid),Column(Field(3,1,1))],joins=[join],order=[Order(uid),Order(Field(3,1,1))])
        result=q.execute(db);assert len(result.rows)==32
        assert any(a.get("strategy")=="index_join" for a in result.access)
        q.joins=[Join(b"o/",(uid,),(Field(2,1,1),),JoinKind.LEFT)]
        assert len(q.execute(db).rows)==80
        q.columns=[Column(uid)];q.order=[];q.joins=[Join(b"o/",(uid,),(Field(2,1,1),),JoinKind.SEMI)]
        assert len(q.execute(db).rows)==16
        q.joins=[Join(b"o/",(uid,),(Field(2,1,1),),JoinKind.ANTI)]
        assert len(q.execute(db).rows)==48
        with db.transaction() as tx:
            q=Query(b"p/",where=kind.test(Op.EQ,b"new"),columns=[Column(uid)])
            tx.put(b"p/0",document(0).replace(_b(2,b"warrior"),_b(2,b"new")))
            assert q.execute(tx).rows[0]["values"]==[0]
        q=Query(b"p/",max_candidates=1,order=[Order(gold)])
        try:q.execute(db);raise AssertionError("query candidate budget ignored")
        except Error as e: assert e.code==9
        db.sync();db.verify()
    with Store(root/"main",library=library) as db:
        assert len(Query(b"p/",columns=[Column(uid)]).execute(db).rows)==64
    print("PASS Python native conditions, ordering/cursor, patterns, groups, joins, private writes and budgets")

if __name__=="__main__":run(sys.argv[1],sys.argv[2])
