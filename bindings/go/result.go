package fengqudb

/*
#include "fqdb_go.h"
*/
import "C"

import (
	"math"
	"runtime"
	"unsafe"
)

// Stats 是查询执行统计（响应字段 3）。
type Stats struct {
	Candidates, Matched, ResultTotal, Work uint64
	PeakMemory, PayloadReads, JoinProbes   uint64
	Complete                               bool
}

// Access 是一项访问计划（响应字段 2，每个来源一项）。
type Access struct {
	Source            uint64
	Strategy, Index   string
	Reason            string
	Covering, Ordered bool
	NeedsScan         bool
	SampledCandidates uint64
}

// Result 持有 fqdb_store_select 返回的原生响应缓冲，并直接在其上惰性解析：
// 不整体复制到 Go 堆，行与值按需解码。零值可用，且可通过 ExecuteInto 反复复用。
//
// 生命周期规则：Key、Value.Bytes、Cursor 等返回的切片都指向原生缓冲，在 Close
// （或 Result 被再次用于执行）之前有效；需要逃逸时自行复制。Close 立即归还缓冲。
// 一个 Result 不可被多个 goroutine 同时使用。
type Result struct {
	ptr     unsafe.Pointer
	size    int    // 原生缓冲的完整长度（释放时需要）
	buf     []byte // 响应字段 1 内的查询结果（原生缓冲的子切片）
	pos     int    // 下一个待扫描位置
	rs, re  int    // 当前行的消息范围
	ks, ke  int    // 当前行键的范围
	err     error
	managed bool // 由 Execute 分配、带 finalizer
}

func newResult() *Result {
	r := new(Result)
	r.managed = true
	runtime.SetFinalizer(r, (*Result).Close)
	return r
}

// adopt 接管 C 返回的缓冲；调用前 Result 必须已 Close。
func (r *Result) adopt(b C.fqg_buf) error {
	if b.st != C.FQDB_OK {
		return statusErr(b.st)
	}
	r.ptr, r.size, r.buf = b.data, int(b.size), nil
	r.pos, r.rs, r.re, r.ks, r.ke, r.err = 0, 0, 0, 0, 0, nil
	if b.size == 0 {
		return nil
	}
	// 外层响应：字段 1 是查询结果消息（bytes），其内部才是行/计划/统计/游标。
	outer := unsafe.Slice((*byte)(b.data), int(b.size))
	for p := 0; p < len(outer); {
		f, np, ok := nextField(outer, p)
		if !ok {
			r.Close()
			return ErrMalformed
		}
		p = np
		if f.num == 1 && f.wire == 2 {
			r.buf = outer[f.start:f.end:f.end]
		}
	}
	return nil
}

// Close 释放原生缓冲；可重复调用，之后可再次用于执行。
func (r *Result) Close() {
	if r.ptr != nil {
		C.fqg_buffer_free(r.ptr, C.uint64_t(r.size))
	}
	r.ptr, r.buf, r.size = nil, nil, 0
	r.pos, r.rs, r.re, r.ks, r.ke = 0, 0, 0, 0, 0
	if r.managed {
		runtime.SetFinalizer(r, nil)
		r.managed = false
	}
}

// Raw 返回原生响应字节（只读视图）。
func (r *Result) Raw() []byte { return r.buf }

// Err 返回解析过程中遇到的错误（Next 返回 false 后检查）。
func (r *Result) Err() error { return r.err }

type pbField struct {
	num        uint32
	wire       uint8
	v          uint64
	start, end int
}

func readVarint(b []byte, p int) (uint64, int, bool) {
	var x uint64
	for shift := uint(0); shift < 70; shift += 7 {
		if p >= len(b) {
			return 0, p, false
		}
		c := b[p]
		p++
		x |= uint64(c&127) << shift
		if c < 128 {
			return x, p, true
		}
	}
	return 0, p, false
}

// nextField 解析 b[p:] 开头的一个 protobuf 字段，返回字段及下一个位置。
func nextField(b []byte, p int) (f pbField, next int, ok bool) {
	tag, p, ok := readVarint(b, p)
	if !ok {
		return f, p, false
	}
	f.num, f.wire = uint32(tag>>3), uint8(tag&7)
	switch f.wire {
	case 0:
		f.v, p, ok = readVarint(b, p)
		return f, p, ok
	case 1, 5:
		n := 8
		if f.wire == 5 {
			n = 4
		}
		if p+n > len(b) {
			return f, p, false
		}
		for i := n - 1; i >= 0; i-- {
			f.v = f.v<<8 | uint64(b[p+i])
		}
		return f, p + n, true
	case 2:
		var l uint64
		l, p, ok = readVarint(b, p)
		if !ok || l > uint64(len(b)-p) {
			return f, p, false
		}
		f.start, f.end = p, p+int(l)
		return f, f.end, true
	}
	return f, p, false
}

// Next 前进到下一行；没有更多行或响应格式错误时返回 false（后者见 Err）。
func (r *Result) Next() bool {
	for r.pos < len(r.buf) {
		f, np, ok := nextField(r.buf, r.pos)
		if !ok {
			r.err, r.pos = ErrMalformed, len(r.buf)
			return false
		}
		r.pos = np
		if f.num != 1 || f.wire != 2 {
			continue
		}
		r.rs, r.re = f.start, f.end
		r.ks, r.ke = 0, 0
		for p := f.start; p < f.end; {
			g, np, ok := nextField(r.buf[:f.end], p)
			if !ok {
				r.err, r.pos = ErrMalformed, len(r.buf)
				return false
			}
			p = np
			if g.num == 1 && g.wire == 2 {
				r.ks, r.ke = g.start, g.end
			}
		}
		return true
	}
	return false
}

// Key 返回当前行的键（原生视图）。对聚合/分组行，键是分组键的编码。
func (r *Result) Key() []byte { return r.buf[r.ks:r.ke:r.ke] }

// NumValues 返回当前行的值个数。
func (r *Result) NumValues() int {
	n := 0
	for p := r.rs; p < r.re; {
		g, np, ok := nextField(r.buf[:r.re], p)
		if !ok {
			return n
		}
		p = np
		if g.num == 2 && g.wire == 2 {
			n++
		}
	}
	return n
}

// Value 解码当前行的第 i 个值；i 越界或格式错误时返回 ErrMalformed。
func (r *Result) Value(i int) (Value, error) {
	n := 0
	for p := r.rs; p < r.re; {
		g, np, ok := nextField(r.buf[:r.re], p)
		if !ok {
			return Value{}, ErrMalformed
		}
		p = np
		if g.num == 2 && g.wire == 2 {
			if n == i {
				return decodeValue(r.buf[:g.end], g.start)
			}
			n++
		}
	}
	return Value{}, ErrMalformed
}

// AppendValues 把当前行的全部值解码并追加到 dst（dst 容量足够时不分配）。
func (r *Result) AppendValues(dst []Value) ([]Value, error) {
	for p := r.rs; p < r.re; {
		g, np, ok := nextField(r.buf[:r.re], p)
		if !ok {
			return dst, ErrMalformed
		}
		p = np
		if g.num == 2 && g.wire == 2 {
			v, err := decodeValue(r.buf[:g.end], g.start)
			if err != nil {
				return dst, err
			}
			dst = append(dst, v)
		}
	}
	return dst, nil
}

// decodeValue 解码 b[p:] 到末尾的一个 QueryValue 消息，规则同 Python _value_at。
func decodeValue(b []byte, p int) (Value, error) {
	var kind, u, raw uint64
	var real uint64
	var data []byte
	for p < len(b) {
		f, np, ok := nextField(b, p)
		if !ok {
			return Value{}, ErrMalformed
		}
		p = np
		switch {
		case f.wire == 0 && f.num == 1:
			kind = f.v
		case f.wire == 0 && f.num == 2:
			u = f.v
		case f.wire == 0 && f.num == 3:
			raw = f.v
		case f.wire == 1 && f.num == 4:
			real = f.v
		case f.wire == 2 && f.num == 5:
			data = b[f.start:f.end:f.end]
		}
	}
	switch ValueKind(kind) {
	case KindNull:
		return Value{}, nil
	case KindUint:
		return Value{Kind: KindUint, U: u}, nil
	case KindSint:
		return Value{Kind: KindSint, I: int64(raw>>1) ^ -int64(raw&1)}, nil
	case KindDouble:
		return Value{Kind: KindDouble, F: math.Float64frombits(real)}, nil
	case KindBytes:
		if data == nil {
			data = emptyBytes
		}
		return Value{Kind: KindBytes, B: data}, nil
	case KindBool:
		return Value{Kind: KindBool, U: u & 1}, nil
	}
	return Value{}, ErrMalformed
}

// section 查找最后一个出现的顶层字段 n（wire 2）。
func (r *Result) section(n uint32) (start, end int, found bool, err error) {
	for p := 0; p < len(r.buf); {
		f, np, ok := nextField(r.buf, p)
		if !ok {
			return 0, 0, false, ErrMalformed
		}
		p = np
		if f.num == n && f.wire == 2 {
			start, end, found = f.start, f.end, true
		}
	}
	return
}

// Cursor 返回续页游标（原生视图）；空表示已无更多页。续页时把它复制到 Query.After。
func (r *Result) Cursor() ([]byte, error) {
	s, e, ok, err := r.section(4)
	if err != nil || !ok {
		return nil, err
	}
	return r.buf[s:e:e], nil
}

// Stats 解码统计信息。
func (r *Result) Stats() (Stats, error) {
	var st Stats
	for p := 0; p < len(r.buf); {
		f, np, ok := nextField(r.buf, p)
		if !ok {
			return st, ErrMalformed
		}
		p = np
		if f.num != 3 || f.wire != 2 {
			continue
		}
		for q := f.start; q < f.end; {
			g, nq, ok := nextField(r.buf[:f.end], q)
			if !ok {
				return st, ErrMalformed
			}
			q = nq
			if g.wire != 0 {
				continue
			}
			switch g.num {
			case 1:
				st.Candidates = g.v
			case 2:
				st.Matched = g.v
			case 3:
				st.ResultTotal = g.v
			case 4:
				st.Work = g.v
			case 5:
				st.PeakMemory = g.v
			case 6:
				st.PayloadReads = g.v
			case 7:
				st.JoinProbes = g.v
			case 8:
				st.Complete = g.v != 0
			}
		}
	}
	return st, nil
}

// Access 解码访问计划（字符串会复制）。
func (r *Result) Access() ([]Access, error) {
	var out []Access
	for p := 0; p < len(r.buf); {
		f, np, ok := nextField(r.buf, p)
		if !ok {
			return out, ErrMalformed
		}
		p = np
		if f.num != 2 || f.wire != 2 {
			continue
		}
		var a Access
		for q := f.start; q < f.end; {
			g, nq, ok := nextField(r.buf[:f.end], q)
			if !ok {
				return out, ErrMalformed
			}
			q = nq
			switch {
			case g.wire == 0 && g.num == 1:
				a.Source = g.v
			case g.wire == 2 && g.num == 2:
				a.Strategy = string(r.buf[g.start:g.end])
			case g.wire == 2 && g.num == 3:
				a.Index = string(r.buf[g.start:g.end])
			case g.wire == 2 && g.num == 4:
				a.Reason = string(r.buf[g.start:g.end])
			case g.wire == 0 && g.num == 5:
				a.Covering = g.v != 0
			case g.wire == 0 && g.num == 6:
				a.Ordered = g.v != 0
			case g.wire == 0 && g.num == 7:
				a.NeedsScan = g.v != 0
			case g.wire == 0 && g.num == 8:
				a.SampledCandidates = g.v
			}
		}
		out = append(out, a)
	}
	return out, nil
}
