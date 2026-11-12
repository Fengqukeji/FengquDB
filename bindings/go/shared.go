package fengqudb

/*
#include "fqdb_go.h"
*/
import "C"

import (
	"runtime"
	"unsafe"
)

var emptyBytes = []byte{}

// Shared 是共享读返回的值视图，指向核心库的不可变原生内存，不复制值字节。
//
// 生命周期规则：
//   - 持有 Shared 期间占用一份固定内存预算（pinned_bytes），必须调用 Close 归还。
//   - Bytes/Text 返回的切片/字符串在 Close 之前有效，Close 之后不得再使用（会读到已释放内存）。
//   - 视图在之后的写入、删除、整理，乃至 Store.Close 之后仍然有效且内容不变。
//   - 切片是只读语义：不要写入。切片容量等于长度，append 会复制到 Go 堆。
//   - Shared 是值类型（热路径无堆分配，也因此没有 finalizer）。需要跨函数长期持有、
//     并希望有 GC 兜底时，用 Escape 转为 *SharedValue。
//   - Shared 的拷贝不共享"已关闭"状态：只对其中一个副本调用 Close。
type Shared struct {
	owner   *C.fqdb_store_value
	data    unsafe.Pointer
	size    int
	version uint64
	expires uint64
}

// newShared 把 C 返回值转换为 Shared，非 OK 状态转成 error。
func newShared(r C.fqg_shared) (Shared, error) {
	if r.st != C.FQDB_OK {
		return Shared{}, statusErr(r.st)
	}
	return Shared{owner: r.owner, data: r.data, size: int(r.size), version: uint64(r.version), expires: uint64(r.expires)}, nil
}

// Bytes 返回指向原生内存的只读切片（空值返回非 nil 的空切片）。
func (s *Shared) Bytes() []byte {
	if s.owner == nil {
		return nil
	}
	if s.size == 0 {
		return emptyBytes
	}
	return unsafe.Slice((*byte)(s.data), s.size)
}

// Text 把值当作字符串零拷贝返回；与 Bytes 同样在 Close 前有效。
func (s *Shared) Text() string {
	if s.owner == nil || s.size == 0 {
		return ""
	}
	return unsafe.String((*byte)(s.data), s.size)
}

// Len 返回值的字节数。
func (s *Shared) Len() int { return s.size }

// Version 返回该值的版本号。
func (s *Shared) Version() uint64 { return s.version }

// ExpiresAtMs 返回过期的绝对 Unix 毫秒，0 表示不过期。
func (s *Shared) ExpiresAtMs() uint64 { return s.expires }

// Close 归还租约；对同一个 Shared 重复调用无副作用。
func (s *Shared) Close() {
	if s.owner == nil {
		return
	}
	C.fqdb_store_value_close(s.owner)
	s.owner, s.data, s.size = nil, nil, 0
}

// SharedValue 是堆上的共享视图，带 finalizer 兜底：忘记 Close 时由 GC 归还租约。
// finalizer 的触发时点不确定，固定内存预算可能迟迟不释放，因此仍应显式 Close。
type SharedValue struct {
	Shared
}

// Escape 把租约的所有权转移到一个带 finalizer 的堆对象（会产生一次分配）。
// 调用后原 Shared 不再有效，不要再对它调用 Close。
func (s *Shared) Escape() *SharedValue {
	v := &SharedValue{Shared: *s}
	*s = Shared{}
	if v.owner != nil {
		runtime.SetFinalizer(v, (*SharedValue).finalize)
	}
	return v
}

func (v *SharedValue) finalize() { v.Shared.Close() }

// Close 归还租约并取消 finalizer。
func (v *SharedValue) Close() {
	runtime.SetFinalizer(v, nil)
	v.Shared.Close()
}

// PreparedKey 是已封送的键：键只复制一次，之后每次读取不再封送、不再分配。
// 多个 goroutine 可并发调用 GetShared。Close 不得与读取并发。
// Store 关闭后 PreparedKey 仍可读取（核心库保持底层库存活）。
type PreparedKey struct {
	h *C.fqdb_store_key
}

// PrepareKey 为 key 创建准备键。
func (s *Store) PrepareKey(key []byte) (*PreparedKey, error) {
	if s.h == nil {
		return nil, ErrClosed
	}
	r := C.fqg_key_prepare(s.h, kptr(key), C.uint64_t(len(key)))
	runtime.KeepAlive(key)
	if err := statusErr(r.st); err != nil {
		return nil, err
	}
	p := &PreparedKey{h: (*C.fqdb_store_key)(r.ptr)}
	runtime.SetFinalizer(p, (*PreparedKey).Close)
	return p, nil
}

// GetShared 返回当前值的共享视图；未命中返回 ErrNotFound。使用完 Close。
func (p *PreparedKey) GetShared() (Shared, error) {
	if p.h == nil {
		return Shared{}, ErrClosed
	}
	return newShared(C.fqg_key_get_shared(p.h))
}

// Close 释放准备键；重复调用无副作用。已取得的 Shared 不受影响。
func (p *PreparedKey) Close() {
	if p.h == nil {
		return
	}
	runtime.SetFinalizer(p, nil)
	C.fqdb_store_key_close(p.h)
	p.h = nil
}

// Row 是批量读取器的一行，直接映射核心库的 fqdb_store_result_row（40 字节），
// 切片和行描述都位于原生内存，不复制。
type Row struct {
	data    unsafe.Pointer
	size    uint64
	version uint64
	expires uint64
	found   uint32
	_       uint32
}

func init() {
	if unsafe.Sizeof(Row{}) != C.sizeof_fqdb_store_result_row {
		panic("fengqudb: Row layout does not match fqdb_store_result_row")
	}
}

// Found 区分命中与未命中（空值也算命中）。
func (r *Row) Found() bool { return r.found != 0 }

// Value 返回值的只读视图；未命中返回 nil，空值返回非 nil 的空切片。
func (r *Row) Value() []byte {
	if r.found == 0 {
		return nil
	}
	if r.size == 0 {
		return emptyBytes
	}
	return unsafe.Slice((*byte)(r.data), int(r.size))
}

// Version 返回值的版本号。
func (r *Row) Version() uint64 { return r.version }

// ExpiresAtMs 返回过期的绝对 Unix 毫秒，0 表示不过期。
func (r *Row) ExpiresAtMs() uint64 { return r.expires }

// Reader 是批量准备读取器：一组键复制一次，之后每次 Read 一次 cgo 往返即得到整批结果，
// 未变化的热数据不分配、不复制载荷。
//
// 生命周期规则：
//   - Read 返回的 []Row 及其中 Value() 字节仅在下一次 Read/Acquire/Release/Close
//     之前有效（即使这些调用失败）。需要保留请复制，或用 Acquire。
//   - 一个 Reader 由调用方串行使用；多个 goroutine 请各自创建 Reader。
//   - 父 Store 关闭后 Reader 仍可使用。
type Reader struct {
	h *C.fqdb_store_reader
}

// packSlices 把键复制进一块 C 内存（切片数组 + 数据区），避免 C 侧持有 Go 指针。
// 调用方负责 C.free(mem)。
func packSlices(keys [][]byte) (mem unsafe.Pointer, arr []C.fqdb_slice) {
	total := 0
	for _, k := range keys {
		total += len(k)
	}
	head := len(keys) * int(unsafe.Sizeof(C.fqdb_slice{}))
	mem = C.malloc(C.size_t(head + total + 1))
	arr = unsafe.Slice((*C.fqdb_slice)(mem), len(keys))
	data := unsafe.Slice((*byte)(unsafe.Add(mem, head)), total+1)
	off := 0
	for i, k := range keys {
		if len(k) == 0 {
			arr[i] = C.fqdb_slice{data: nil, size: 0}
			continue
		}
		copy(data[off:], k)
		arr[i] = C.fqdb_slice{data: unsafe.Pointer(&data[off]), size: C.uint64_t(len(k))}
		off += len(k)
	}
	return mem, arr
}

// PrepareReader 为 1..4096 个键创建批量读取器，保留顺序、重复键与空值。
func (s *Store) PrepareReader(keys [][]byte) (*Reader, error) {
	if s.h == nil {
		return nil, ErrClosed
	}
	if len(keys) == 0 || len(keys) > 4096 {
		return nil, &Error{Code: StatusInvalidArgument, Message: "reader needs 1..4096 keys"}
	}
	mem, arr := packSlices(keys)
	defer C.free(mem)
	r := C.fqg_reader_create(s.h, &arr[0], C.uint32_t(len(keys)))
	if err := statusErr(r.st); err != nil {
		return nil, err
	}
	rd := &Reader{h: (*C.fqdb_store_reader)(r.ptr)}
	runtime.SetFinalizer(rd, (*Reader).Close)
	return rd, nil
}

func rowsOf(r C.fqg_rows) []Row {
	if r.count == 0 {
		return nil
	}
	return unsafe.Slice((*Row)(unsafe.Pointer(r.rows)), int(r.count))
}

// Read 读取整批键，返回与键顺序一致的行。
func (rd *Reader) Read() ([]Row, error) {
	if rd.h == nil {
		return nil, ErrClosed
	}
	r := C.fqg_reader_read(rd.h)
	if r.st != C.FQDB_OK {
		return nil, statusErr(r.st)
	}
	return rowsOf(r), nil
}

// Release 释放当前保留的结果，保留准备好的键。
func (rd *Reader) Release() {
	if rd.h != nil {
		C.fqdb_store_reader_release(rd.h)
	}
}

// Results 是 Reader.Acquire 取得的独立结果集：直到 Close 前一直有效，不受后续 Read 影响。
type Results struct {
	h    *C.fqdb_store_results
	rows []Row
}

// Acquire 读取整批并取得独立所有权的结果集。
func (rd *Reader) Acquire() (*Results, error) {
	if rd.h == nil {
		return nil, ErrClosed
	}
	r := C.fqg_reader_acquire(rd.h)
	if err := statusErr(r.st); err != nil {
		return nil, err
	}
	res := &Results{h: (*C.fqdb_store_results)(r.ptr)}
	res.rows = rowsOf(C.fqg_results_rows(res.h))
	runtime.SetFinalizer(res, (*Results).Close)
	return res, nil
}

// Rows 返回结果行，Close 之前有效。
func (r *Results) Rows() []Row { return r.rows }

// Close 释放结果集。
func (r *Results) Close() {
	if r.h == nil {
		return
	}
	runtime.SetFinalizer(r, nil)
	C.fqdb_store_results_close(r.h)
	r.h, r.rows = nil, nil
}

// Close 关闭读取器；重复调用无副作用。
func (rd *Reader) Close() {
	if rd.h == nil {
		return
	}
	runtime.SetFinalizer(rd, nil)
	C.fqdb_store_reader_close(rd.h)
	rd.h = nil
}
