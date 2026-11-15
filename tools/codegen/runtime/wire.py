from dataclasses import dataclass, field
import struct as _struct

_FQ_LIMIT = 16 * 1024 * 1024

def _fq_int(value, bits, signed=False):
    if type(value) is not int or not (-(1 << (bits-1)) if signed else 0) <= value < (1 << (bits-1) if signed else 1 << bits):
        raise ValueError('Integer outside schema range')
    return value

def _fq_u(out, value):
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)

def _fq_room(out, start, length):
    if length > _FQ_LIMIT - (len(out) - start):
        raise ValueError('Entity exceeds 16 MiB')

def _fq_b(out, value, start):
    n = len(value)
    header = 1
    while n >= 128:
        header += 1
        n >>= 7
    _fq_room(out, start, header + len(value))
    _fq_u(out, len(value))
    out.extend(value)

def _fq_bool(value):
    if type(value) is not bool:
        raise ValueError('Expected bool')
    return int(value)

class _FqReader:
    __slots__ = ('data', 'pos')
    def __init__(self, data):
        self.data = memoryview(data).cast('B')
        self.pos = 0
        if len(self.data) > _FQ_LIMIT:
            raise ValueError('Entity exceeds 16 MiB')
    def u(self):
        value = 0
        for i in range(10):
            if self.pos >= len(self.data): break
            b = self.data[self.pos]
            self.pos += 1
            if i == 9 and b > 1: break
            value |= (b & 127) << (i*7)
            if b < 128: return value
        raise ValueError('Invalid or truncated protobuf varint')
    def tag(self):
        t = self.u()
        if t > 0xffffffff or t >> 3 == 0 or t & 7 > 5:
            raise ValueError('Invalid protobuf tag')
        return t
    def take(self, n):
        if n > len(self.data) - self.pos:
            raise ValueError('Truncated protobuf value')
        start = self.pos
        self.pos += n
        return self.data[start:self.pos]
    def b(self): return self.take(self.u())
    def floating(self, fmt, n): return _struct.unpack(fmt, self.take(n))[0]
    def skip(self, t, depth=0):
        kind = t & 7
        if kind == 0: self.u()
        elif kind == 1: self.take(8)
        elif kind == 2: self.b()
        elif kind == 5: self.take(4)
        elif kind == 3 and depth < 64:
            while self.pos < len(self.data):
                inner = self.tag()
                if inner & 7 == 4:
                    if inner >> 3 != t >> 3: raise ValueError('Mismatched group')
                    return
                self.skip(inner, depth+1)
            raise ValueError('Unterminated group')
        else: raise ValueError('Invalid group or nesting limit')

def _fq_signed(value, bits):
    return _fq_int((value >> 1) ^ -(value & 1), bits, True)

def _fq_read_bool(value):
    if value > 1: raise ValueError('Invalid bool')
    return bool(value)

def _fq_key(prefix, value):
    result = prefix + (value.encode('utf-8') if isinstance(value, str) else str(value).encode('ascii'))
    if len(result) > 65536: raise ValueError('Key exceeds 64 KiB')
    return result
