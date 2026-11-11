"""Typed native queries. Fields use protobuf numbers/types, not Python callbacks.

Matching is bytewise; i-operators fold ASCII. Missing fields use SQL three-valued
logic. See docs/STORE_SELECT.md for budgets, joins and pagination contracts.
"""
from dataclasses import dataclass, field as factory
from enum import IntEnum
import math
import struct
from fengqudb import _feature_varint, _feature_fields

def _u(n, v):
    if not isinstance(v, int) or not 0 <= v < 1 << 64:
        raise ValueError("query integer outside uint64")
    return _feature_varint(n * 8) + _feature_varint(v)

def _b(n, v):
    if isinstance(v, str): v = v.encode("utf-8")
    v = bytes(v)
    return _feature_varint(n * 8 + 2) + _feature_varint(len(v)) + v

def _value(v):
    if v is None: return _u(1, 0)
    if isinstance(v, bool): return _u(1, 5) + _u(2, int(v))
    if isinstance(v, int):
        if v >= 0: return _u(1, 1) + _u(2, v)
        if v < -(1 << 63): raise ValueError("query integer outside int64")
        return _u(1, 2) + _u(3, (v << 1) ^ (v >> 63))
    if isinstance(v, float):
        if not math.isfinite(v): raise ValueError("non-finite query number")
        return _u(1, 3) + b"\x21" + struct.pack("<d", v)
    return _u(1, 4) + _b(5, v)

def _decode_value(data):
    f = dict(_feature_fields(data)); k = f.get(1, 0)
    if k == 0: return None
    if k == 1: return f.get(2, 0)
    if k == 2:
        n = f.get(3, 0); return (n >> 1) ^ -(n & 1)
    if k == 3: return struct.unpack("<d", struct.pack("<Q", f.get(4, 0)))[0]
    if k == 4: return f.get(5, b"")
    if k == 5: return bool(f.get(2, 0))
    raise ValueError("invalid query value kind")

class Op(IntEnum):
    TRUE=0; FALSE=1; AND=2; OR=3; NOT=4; EQ=5; NE=6; LT=7; LE=8; GT=9; GE=10
    IN=11; EXISTS=12; IS_NULL=13; CONTAINS=14; STARTS_WITH=15; ENDS_WITH=16
    LIKE=17; REGEX=18; ICONTAINS=19; ILIKE=20; IREGEX=21; FUZZY=22

class Aggregate(IntEnum):
    NONE=0; COUNT_ALL=1; COUNT=2; SUM=3; MIN=4; MAX=5; AVG=6

class JoinKind(IntEnum):
    INNER=0; LEFT=1; SEMI=2; ANTI=3

@dataclass(frozen=True)
class Field:
    number: int = 0
    type: int = 4
    source: int = 0
    path: tuple = ()
    def encode(self):
        path = self.path or ((self.number,) if self.number else ())
        return _u(1, self.source) + _u(2, self.type) + b"".join(_u(3, n) for n in path)
    @staticmethod
    def key(source=0): return Field(source=source)
    @staticmethod
    def record(source=0): return Field(type=0, source=source)
    @staticmethod
    def column(index): return Field(index+1, 0, 255)
    def test(self, op, *values): return Expr(op, self, tuple(values))
    def compare_field(self, op, other): return Expr(op, self, right=other)

@dataclass(frozen=True)
class Expr:
    op: Op = Op.TRUE
    field: Field = Field()
    values: tuple = ()
    children: tuple = ()
    right: object = None
    def encode(self, depth=0):
        if depth>64: raise ValueError("query expression too deep")
        return (_u(1, int(self.op)) + (_b(2, self.field.encode()) if self.op >= 5 else b"") +
                b"".join(_b(3, _value(v)) for v in self.values) +
                b"".join(_b(4, c.encode(depth+1)) for c in self.children) +
                (_b(5, self.right.encode()) if self.right is not None else b""))
    def __and__(self, other): return Expr(Op.AND, children=(self, other))
    def __or__(self, other): return Expr(Op.OR, children=(self, other))
    def __invert__(self): return Expr(Op.NOT, children=(self,))

@dataclass(frozen=True)
class Column:
    field: Field = Field()
    aggregate: Aggregate = Aggregate.NONE
    name: str = ""
    distinct: bool = False
    def encode(self):
        return _b(1, self.field.encode()) + _u(2, int(self.aggregate)) + _b(3, self.name) + _u(4, int(self.distinct))

@dataclass(frozen=True)
class Order:
    field: Field
    descending: bool = False
    nulls_first: bool = True
    def encode(self): return _b(1, self.field.encode()) + _u(2, int(self.descending)) + _u(3, int(self.nulls_first))

@dataclass(frozen=True)
class Join:
    prefix: bytes
    left: tuple
    right: tuple
    kind: JoinKind = JoinKind.INNER
    on: Expr = Expr()
    def encode(self):
        return (_b(1, self.prefix) + b"".join(_b(2, f.encode()) for f in self.left) +
                b"".join(_b(3, f.encode()) for f in self.right) + _u(4, int(self.kind)) + _b(5, self.on.encode()))

@dataclass
class Query:
    prefix: bytes
    where: Expr = Expr()
    columns: list = factory(default_factory=list)
    order: list = factory(default_factory=list)
    group: list = factory(default_factory=list)
    having: Expr = Expr()
    joins: list = factory(default_factory=list)
    limit: int = 100
    offset: int = 0
    memory_bytes: int = 16 << 20
    max_candidates: int = 1000000
    max_work: int = 100000000
    timeout_ms: int = 1000
    allow_scan: bool = True
    count_total: bool = False
    distinct: bool = False
    index_hint: bytes = b""
    after: bytes = b""
    def encode(self):
        out = _b(1, self.prefix) + _b(2, self.where.encode())
        for n, values in ((3,self.columns),(4,self.order),(5,self.group)):
            out += b"".join(_b(n, v.encode()) for v in values)
        out += _b(6,self.having.encode()) + b"".join(_b(7,v.encode()) for v in self.joins)
        for n,v in enumerate((self.limit,self.offset,self.memory_bytes,self.max_candidates,self.max_work,
                              self.timeout_ms,int(self.allow_scan),int(self.count_total),int(self.distinct)),8):
            out += _u(n,v)
        return out + _b(17,self.index_hint) + _b(18,self.after)
    def prepare(self): return PreparedQuery(self.encode())
    def execute(self, database): return self.prepare().execute(database)
    def explain(self, database): return self.prepare().explain(database)

@dataclass(frozen=True)
class PreparedQuery:
    encoded: bytes
    def __post_init__(self): object.__setattr__(self,"encoded",bytes(self.encoded))
    def _run(self, database, explain):
        operation=54 if explain else 53
        # Store uses the read-only snapshot entry; transactions keep private-write visibility.
        response=(database.select_raw(operation,self.encoded) if hasattr(database,"select_raw")
                  else database.execute(operation,b"",value=self.encoded))
        if response.value is None: raise ValueError("missing query response")
        return Result(response.value)
    def execute(self, database): return self._run(database,False)
    def explain(self, database): return self._run(database,True)

class Result:
    def __init__(self,data):
        self.rows=[];self.access=[];self.stats={};self.cursor=b""
        for n,v in _feature_fields(data):
            if n==1:
                row={"key":b"","values":[]}
                for k,x in _feature_fields(v):
                    if k==1: row["key"]=x
                    elif k==2: row["values"].append(_decode_value(x))
                self.rows.append(row)
            elif n==2:
                names={1:"source",2:"strategy",3:"index",4:"reason",5:"covering",6:"ordered",7:"needs_scan",8:"sampled_candidates"}
                self.access.append({names[k]:x.decode("utf8") if k in (2,3,4) else bool(x) if k in (5,6,7) else x for k,x in _feature_fields(v) if k in names})
            elif n==3:
                names={1:"candidates",2:"matched",3:"result_total",4:"work",5:"peak_memory",6:"payload_reads",7:"join_probes",8:"complete"}
                self.stats={names[k]:bool(x) if k==8 else x for k,x in _feature_fields(v) if k in names}
            elif n==4: self.cursor=v
