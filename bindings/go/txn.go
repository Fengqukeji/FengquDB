package fengqudb

/*
#include "fqdb_go.h"
*/
import "C"

import (
	"runtime"
	"unsafe"
)

// PutItem 是 Txn.PutMany 的一项。
type PutItem struct {
	Key, Value  []byte
	ExpiresAtMs uint64
}

// Txn 是事务句柄：单 goroutine 使用，读得到自己的未提交写入。
// 无论是否提交成功，都必须 Close。Store 关闭后事务仍可使用到 Close 为止。
type Txn struct {
	h *C.fqdb_store_txn
}

// Begin 开始事务；timeoutMs 为 0 表示没有期限。
func (s *Store) Begin(timeoutMs uint64) (*Txn, error) {
	if s.h == nil {
		return nil, ErrClosed
	}
	r := C.fqg_txn_begin(s.h, C.uint64_t(timeoutMs))
	if err := statusErr(r.st); err != nil {
		return nil, err
	}
	t := &Txn{h: (*C.fqdb_store_txn)(r.ptr)}
	runtime.SetFinalizer(t, (*Txn).Close)
	return t, nil
}

// Get 读取事务内可见的值（含本事务的私有写入），返回副本；未命中返回 ErrNotFound。
func (t *Txn) Get(key []byte) ([]byte, error) {
	if t.h == nil {
		return nil, ErrClosed
	}
	r := C.fqg_txn_get(t.h, kptr(key), C.uint64_t(len(key)))
	runtime.KeepAlive(key)
	if err := statusErr(r.st); err != nil {
		return nil, err
	}
	out := make([]byte, int(r.size))
	if r.size > 0 {
		copy(out, unsafe.Slice((*byte)(r.data), int(r.size)))
	}
	C.fqg_buffer_free(r.data, r.size)
	return out, nil
}

// Put 在事务中写入键值。
func (t *Txn) Put(key, value []byte) error { return t.PutExpiring(key, value, 0) }

// PutExpiring 在事务中写入键值并设置绝对 Unix 毫秒过期时间。
func (t *Txn) PutExpiring(key, value []byte, expiresAtMs uint64) error {
	if t.h == nil {
		return ErrClosed
	}
	st := C.fqg_txn_put(t.h, kptr(key), C.uint64_t(len(key)), kptr(value), C.uint64_t(len(value)), C.uint64_t(expiresAtMs))
	runtime.KeepAlive(key)
	runtime.KeepAlive(value)
	return statusErr(st)
}

// Delete 在事务中删除键。
func (t *Txn) Delete(key []byte) error {
	if t.h == nil {
		return ErrClosed
	}
	st := C.fqg_txn_delete(t.h, kptr(key), C.uint64_t(len(key)))
	runtime.KeepAlive(key)
	return statusErr(st)
}

// PutMany 一次提交多个写入；键与值先复制进 C 内存，核心库整体校验。
func (t *Txn) PutMany(items []PutItem) error {
	if t.h == nil {
		return ErrClosed
	}
	if len(items) == 0 {
		return nil
	}
	blobs := make([][]byte, 0, 2*len(items))
	for _, it := range items {
		blobs = append(blobs, it.Key, it.Value)
	}
	mem, sl := packSlices(blobs)
	defer C.free(mem)
	arr := C.malloc(C.size_t(len(items)) * C.size_t(unsafe.Sizeof(C.fqdb_store_put_item{})))
	defer C.free(arr)
	out := unsafe.Slice((*C.fqdb_store_put_item)(arr), len(items))
	for i, it := range items {
		out[i] = C.fqdb_store_put_item{key: sl[2*i], value: sl[2*i+1], expires_at_ms: C.uint64_t(it.ExpiresAtMs)}
	}
	return statusErr(C.fqdb_store_txn_put_many(t.h, (*C.fqdb_store_put_item)(arr), C.uint32_t(len(items))))
}

// Commit 提交事务并返回提交版本；提交后仍需 Close。
func (t *Txn) Commit() (uint64, error) {
	if t.h == nil {
		return 0, ErrClosed
	}
	r := C.fqg_txn_commit(t.h)
	if err := statusErr(r.st); err != nil {
		return 0, err
	}
	return uint64(r.value), nil
}

// SelectRequest 在事务内执行 SELECT/EXPLAIN（可见私有写入并登记冲突依赖）。
func (t *Txn) SelectRequest(request []byte, out *Result) error {
	if t.h == nil {
		return ErrClosed
	}
	out.Close()
	r := C.fqg_txn_execute(t.h, kptr(request), C.uint64_t(len(request)))
	runtime.KeepAlive(request)
	return out.adopt(r)
}

// Close 关闭事务（未提交则放弃）；重复调用无副作用。
func (t *Txn) Close() {
	if t.h == nil {
		return
	}
	runtime.SetFinalizer(t, nil)
	C.fqdb_store_txn_close(t.h)
	t.h = nil
}
