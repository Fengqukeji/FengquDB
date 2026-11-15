// Package fengqudb 是 FengquDB C ABI（include/fengqudb_store.h）的 Go 绑定。
//
// 构建：需要 cgo，头文件目录与库目录通过 CGO_CFLAGS / CGO_LDFLAGS 提供，
// 详见 README.md。
//
// 指针与生命周期规则（重要）：
//   - 传给本包的 []byte 键/值只在调用期间被读取，核心库会自行复制；调用返回后可修改。
//   - 共享读（GetShared、PreparedKey.GetShared、Reader.Read、Snapshot.GetShared）返回
//     指向原生内存的视图，不复制值字节。Shared 必须 Close，Reader 的行只到下一次
//     Read/Release/Close 为止，详见各类型文档。
//   - 参数里的 Go 切片会被 cgo 视为逃逸；热路径请复用键缓冲区，不要每次调用都新建。
package fengqudb

/*
#cgo CFLAGS: -I${SRCDIR}/include -I${SRCDIR}/../../include
#cgo LDFLAGS: -L${SRCDIR}/Native -lfengqudb
#cgo linux LDFLAGS: -Wl,-rpath,$ORIGIN
#cgo darwin LDFLAGS: -Wl,-rpath,@loader_path
#include "fqdb_go.h"
*/
import "C"

import (
	"runtime"
	"unsafe"
)

// 耐久模式与打开标志。
const (
	DurabilitySync     = 0
	DurabilityBuffered = 1
	FlagCreate         = 1
	FlagSegmented      = 2
)

// Options 对应 fqdb_store_options。传 nil 给 Open 使用核心库默认值
// （SYNC、缺失时创建、缓存 64 MiB、固定内存 128 MiB 等）。全 0 的 Options 会被核心库拒绝。
type Options struct {
	Durability          uint32
	Flags               uint32
	CacheBytes          uint64
	DeltaBytes          uint64
	WriterBytes         uint64
	TransactionBytes    uint64
	AllTransactionBytes uint64
	PinnedBytes         uint64
	MaxSnapshots        uint64
}

// WriteMode 控制写入成功的确认时机，与打开选项的 Durability 分开。
type WriteMode uint32

const (
	WriteReliable    WriteMode = 0 // 等待可靠落盘后返回。
	WritePerformance WriteMode = 1 // 内存生效后返回，按周期或字节阈值刷盘。
)

// WriteOptions 是保存在数据库目录中的写入配置。
type WriteOptions struct {
	Mode           WriteMode
	PeriodMs       uint32
	ThresholdBytes uint64
}

// DefaultWriteOptions 返回新数据库的默认值：性能模式、50 ms、1 MiB。
func DefaultWriteOptions() WriteOptions {
	return WriteOptions{Mode: WritePerformance, PeriodMs: 50, ThresholdBytes: 1 << 20}
}

// Info 对应 fqdb_store_info。
type Info struct {
	Version, DurableVersion, FileBytes           uint64
	IndexedKeys, StoredKeys, StoredValueBytes    uint64
	DeltaBytes, CacheBytes, PinnedBytes          uint64
	TransactionBytes, RecoveredTx, RepairedBytes uint64
}

// Store 是进程内数据库句柄，可被多个 goroutine 并发使用读/写方法。
// Close 不得与同一 Store 上其它调用并发。关闭后其方法返回 ErrClosed；
// 已创建的 PreparedKey、Reader 以及共享视图仍然有效（核心库按引用计数保持底层库）。
type Store struct {
	h *C.fqdb_store
}

// Open 打开（或按 Flags 创建）目录 dir 下的数据库。
func Open(dir string, opts *Options) (*Store, error) {
	cdir := C.CString(dir)
	defer C.free(unsafe.Pointer(cdir))
	var copt *C.fqdb_store_options
	var o C.fqdb_store_options
	if opts != nil {
		o.struct_size = C.uint32_t(unsafe.Sizeof(o))
		o.abi_version = 1
		o.durability = C.uint32_t(opts.Durability)
		o.flags = C.uint32_t(opts.Flags)
		o.cache_bytes = C.uint64_t(opts.CacheBytes)
		o.delta_bytes = C.uint64_t(opts.DeltaBytes)
		o.writer_bytes = C.uint64_t(opts.WriterBytes)
		o.transaction_bytes = C.uint64_t(opts.TransactionBytes)
		o.all_transaction_bytes = C.uint64_t(opts.AllTransactionBytes)
		o.pinned_bytes = C.uint64_t(opts.PinnedBytes)
		o.max_snapshots = C.uint64_t(opts.MaxSnapshots)
		copt = &o
	}
	r := C.fqg_open(cdir, copt)
	if err := statusErr(r.st); err != nil {
		return nil, err
	}
	s := &Store{h: (*C.fqdb_store)(r.ptr)}
	runtime.SetFinalizer(s, (*Store).finalize)
	return s, nil
}

// finalize 是兜底：用户忘记 Close 时释放句柄。正确性不应依赖它。
func (s *Store) finalize() { _ = s.Close() }

// Close 关闭 Store。重复调用无副作用。
func (s *Store) Close() error {
	if s.h == nil {
		return nil
	}
	runtime.SetFinalizer(s, nil)
	C.fqdb_store_close(s.h)
	s.h = nil
	return nil
}

// kptr 返回字节切片首地址，空切片返回 nil。
func kptr(b []byte) unsafe.Pointer {
	if len(b) == 0 {
		return nil
	}
	return unsafe.Pointer(&b[0])
}

// Put 写入键值（无过期）。
func (s *Store) Put(key, value []byte) error { return s.PutExpiring(key, value, 0) }

// PutExpiring 写入键值，expiresAtMs 为绝对 Unix 毫秒，0 表示不过期。
func (s *Store) PutExpiring(key, value []byte, expiresAtMs uint64) error {
	if s.h == nil {
		return ErrClosed
	}
	r := C.fqg_put(s.h, kptr(key), C.uint64_t(len(key)), kptr(value), C.uint64_t(len(value)), C.uint64_t(expiresAtMs))
	runtime.KeepAlive(key)
	runtime.KeepAlive(value)
	return statusErr(r.st)
}

// Delete 删除键。
func (s *Store) Delete(key []byte) error {
	if s.h == nil {
		return ErrClosed
	}
	r := C.fqg_delete(s.h, kptr(key), C.uint64_t(len(key)))
	runtime.KeepAlive(key)
	return statusErr(r.st)
}

// Get 返回值的独立副本；未命中返回 ErrNotFound。空值返回非 nil 的空切片。
func (s *Store) Get(key []byte) ([]byte, error) {
	sh, err := s.GetShared(key)
	if err != nil {
		return nil, err
	}
	out := make([]byte, sh.Len())
	copy(out, sh.Bytes())
	sh.Close()
	return out, nil
}

// GetAppend 把值追加到 dst 并返回；dst 容量足够时不分配，且只需一次 cgo 往返。
// 未命中返回 (dst, ErrNotFound)。
func (s *Store) GetAppend(dst, key []byte) ([]byte, error) {
	if s.h == nil {
		return dst, ErrClosed
	}
	for {
		n := len(dst)
		free := cap(dst) - n
		r := C.fqg_get_into(s.h, kptr(key), C.uint64_t(len(key)), unsafe.Pointer(unsafe.SliceData(dst[n:cap(dst)])), C.uint64_t(free))
		if err := statusErr(r.st); err != nil {
			return dst, err
		}
		size := int(r.value)
		if size <= free {
			runtime.KeepAlive(key)
			return dst[:n+size], nil
		}
		grown := make([]byte, n, n+size)
		copy(grown, dst)
		dst = grown
	}
}

// GetShared 返回指向原生内存的只读视图，未命中返回 ErrNotFound。使用完必须 Shared.Close。
func (s *Store) GetShared(key []byte) (Shared, error) {
	if s.h == nil {
		return Shared{}, ErrClosed
	}
	r := C.fqg_store_get_shared(s.h, kptr(key), C.uint64_t(len(key)))
	runtime.KeepAlive(key)
	return newShared(r)
}

// Sync 把已提交数据刷到持久存储。
func (s *Store) Sync() error {
	if s.h == nil {
		return ErrClosed
	}
	return statusErr(C.fqdb_store_sync(s.h))
}

// WriteOptions 读取当前数据库的写入配置。
func (s *Store) WriteOptions() (WriteOptions, error) {
	if s.h == nil {
		return WriteOptions{}, ErrClosed
	}
	var o C.fqdb_store_write_options
	err := statusErr(C.fqdb_store_write_options_get(s.h, &o))
	runtime.KeepAlive(s)
	if err != nil {
		return WriteOptions{}, err
	}
	return WriteOptions{Mode: WriteMode(o.mode), PeriodMs: uint32(o.period_ms), ThresholdBytes: uint64(o.threshold_bytes)}, nil
}

// ConfigureWrite 切换并保存写入模式；可能等待待写数据落盘。
// 保留未修改参数时，先用 WriteOptions 读取，再修改所需字段。
func (s *Store) ConfigureWrite(options WriteOptions) error {
	if s.h == nil {
		return ErrClosed
	}
	o := C.fqdb_store_write_options{
		struct_size: C.uint32_t(unsafe.Sizeof(C.fqdb_store_write_options{})), abi_version: 1,
		mode: C.uint32_t(options.Mode), period_ms: C.uint32_t(options.PeriodMs), threshold_bytes: C.uint64_t(options.ThresholdBytes),
	}
	err := statusErr(C.fqdb_store_write_configure(s.h, &o))
	runtime.KeepAlive(s)
	return err
}

// ResetWriteOptions 恢复并保存默认性能模式、50 ms、1 MiB。
func (s *Store) ResetWriteOptions() error {
	if s.h == nil {
		return ErrClosed
	}
	err := statusErr(C.fqdb_store_write_configure(s.h, nil))
	runtime.KeepAlive(s)
	return err
}

// Info 返回 Store 统计信息。
func (s *Store) Info() (Info, error) {
	if s.h == nil {
		return Info{}, ErrClosed
	}
	var in C.fqdb_store_info
	if err := statusErr(C.fqdb_store_get_info(s.h, &in)); err != nil {
		return Info{}, err
	}
	return Info{
		Version: uint64(in.version), DurableVersion: uint64(in.durable_version), FileBytes: uint64(in.file_bytes),
		IndexedKeys: uint64(in.indexed_keys), StoredKeys: uint64(in.stored_keys), StoredValueBytes: uint64(in.stored_value_bytes),
		DeltaBytes: uint64(in.delta_bytes), CacheBytes: uint64(in.cache_bytes), PinnedBytes: uint64(in.pinned_bytes),
		TransactionBytes: uint64(in.transaction_bytes), RecoveredTx: uint64(in.recovered_transactions), RepairedBytes: uint64(in.repaired_bytes),
	}, nil
}

// 索引字段类型，对应 fqdb_store_index_field.type。
const (
	IndexUint64   = 1
	IndexInt64    = 2
	IndexDouble   = 3
	IndexBytes    = 4
	IndexSint64   = 5
	IndexFloat    = 6
	IndexFixed64  = 7
	IndexSfixed64 = 8
	IndexFixed32  = 9
	IndexSfixed32 = 10
	IndexBool     = 11
)

// 索引标志。
const (
	IndexUnique   = 1
	IndexRequired = 2
)

// IndexField 是复合索引的一个顶层 protobuf 字段（1..8 个，有序）。
type IndexField struct {
	Field uint32
	Type  uint32
}

// DefineCompoundIndex 为以 prefix 开头的键定义复合索引。
func (s *Store) DefineCompoundIndex(name, prefix []byte, fields []IndexField, flags uint32) error {
	if s.h == nil {
		return ErrClosed
	}
	if len(fields) == 0 || len(fields) > 8 {
		return &Error{Code: StatusInvalidArgument, Message: "index needs 1..8 fields"}
	}
	mem := C.malloc(C.size_t(len(fields)) * C.size_t(unsafe.Sizeof(C.fqdb_store_index_field{})))
	defer C.free(mem)
	arr := unsafe.Slice((*C.fqdb_store_index_field)(mem), len(fields))
	for i, f := range fields {
		arr[i].field = C.uint32_t(f.Field)
		arr[i]._type = C.uint32_t(f.Type)
	}
	st := C.fqg_index_define_compound(s.h, kptr(name), C.uint64_t(len(name)), kptr(prefix), C.uint64_t(len(prefix)),
		(*C.fqdb_store_index_field)(mem), C.uint32_t(len(fields)), C.uint32_t(flags))
	runtime.KeepAlive(name)
	runtime.KeepAlive(prefix)
	return statusErr(st)
}

// DropIndex 删除索引。
func (s *Store) DropIndex(name []byte) error {
	if s.h == nil {
		return ErrClosed
	}
	st := C.fqg_index_drop(s.h, kptr(name), C.uint64_t(len(name)))
	runtime.KeepAlive(name)
	return statusErr(st)
}

// SelectRequest 以完整的外层请求字节（见 EncodeRequest）执行只读 SELECT/EXPLAIN，
// 结果保存在原生缓冲 out 中；out 之前的内容会先被释放。
func (s *Store) SelectRequest(request []byte, out *Result) error {
	if s.h == nil {
		return ErrClosed
	}
	out.Close()
	r := C.fqg_select(s.h, kptr(request), C.uint64_t(len(request)))
	runtime.KeepAlive(request)
	return out.adopt(r)
}

// Snapshot 是只读一致性快照。
type Snapshot struct {
	h *C.fqdb_store_snapshot
}

// Snapshot 创建快照；快照的过期时间在创建时刻评估。
func (s *Store) Snapshot() (*Snapshot, error) {
	if s.h == nil {
		return nil, ErrClosed
	}
	r := C.fqg_snapshot_create(s.h)
	if err := statusErr(r.st); err != nil {
		return nil, err
	}
	sn := &Snapshot{h: (*C.fqdb_store_snapshot)(r.ptr)}
	runtime.SetFinalizer(sn, (*Snapshot).Close)
	return sn, nil
}

// Close 关闭快照；已取得的共享视图仍然有效。
func (sn *Snapshot) Close() {
	if sn.h == nil {
		return
	}
	runtime.SetFinalizer(sn, nil)
	C.fqdb_store_snapshot_close(sn.h)
	sn.h = nil
}

// Get 返回快照中值的副本。
func (sn *Snapshot) Get(key []byte) ([]byte, error) {
	if sn.h == nil {
		return nil, ErrClosed
	}
	r := C.fqg_snapshot_get(sn.h, kptr(key), C.uint64_t(len(key)))
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

// GetShared 返回快照中值的共享视图。
func (sn *Snapshot) GetShared(key []byte) (Shared, error) {
	if sn.h == nil {
		return Shared{}, ErrClosed
	}
	r := C.fqg_snapshot_get_shared(sn.h, kptr(key), C.uint64_t(len(key)))
	runtime.KeepAlive(key)
	return newShared(r)
}
