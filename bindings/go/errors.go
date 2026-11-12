package fengqudb

/*
#include "fqdb_go.h"
*/
import "C"

import (
	"errors"
	"fmt"
)

// Status 是核心库返回的 fqdb_status 数值码。
type Status int32

// 状态码，与 include/fengqudb.h 一致。
const (
	StatusOK               Status = 0
	StatusNotFound         Status = 1
	StatusEnd              Status = 2
	StatusInvalidArgument  Status = 3
	StatusIOError          Status = 4
	StatusCorrupt          Status = 5
	StatusLocked           Status = 6
	StatusConflict         Status = 7
	StatusNoMemory         Status = 8
	StatusLimit            Status = 9
	StatusCommitUnknown    Status = 10
	StatusRecoveryRequired Status = 11
	StatusUnsupported      Status = 12
	StatusInternalError    Status = 13
	StatusHistoryLost      Status = 14
	StatusAlreadyExists    Status = 15
	StatusBufferTooSmall   Status = 16
	StatusTimeout          Status = 17
	StatusNotModified      Status = 18
	StatusCancelled        Status = 19
	StatusConstraint       Status = 20
	StatusAsyncRequired    Status = 21
)

// Error 是非 OK 状态码对应的错误，保留数值码与核心库给出的文字说明。
type Error struct {
	Code    Status
	Message string
}

func (e *Error) Error() string {
	return fmt.Sprintf("fengqudb: %s (status %d)", e.Message, int32(e.Code))
}

// Is 让 errors.Is(err, ErrNotFound) 等按状态码比较。
func (e *Error) Is(target error) bool {
	t, ok := target.(*Error)
	return ok && t.Code == e.Code
}

// 常用哨兵错误；可用 errors.Is 判断。
var (
	ErrNotFound = statusTable[StatusNotFound]
	ErrConflict = statusTable[StatusConflict]
	ErrLimit    = statusTable[StatusLimit]
	// ErrClosed 表示对已关闭的 Store、事务、读取器等对象继续调用。
	ErrClosed = errors.New("fengqudb: use of closed object")
	// ErrMalformed 表示查询响应的 protobuf 内容被截断或无效。
	ErrMalformed = errors.New("fengqudb: malformed query response")
)

// statusTable 预先分配每个状态码对应的错误对象：热路径上的未命中不产生堆分配。
var statusTable = func() [22]*Error {
	var t [22]*Error
	for i := range t {
		t[i] = &Error{Code: Status(i), Message: C.GoString(C.fqdb_status_message(C.fqdb_status(i)))}
	}
	return t
}()

// ABIVersion 返回已加载核心库的 ABI 版本（本绑定面向 2）。
func ABIVersion() uint32 { return uint32(C.fqdb_abi_version()) }

// statusErr 把 C 状态码转换为 error；OK 返回 nil。
func statusErr(st C.fqdb_status) error {
	if st == C.FQDB_OK {
		return nil
	}
	if st > 0 && int(st) < len(statusTable) {
		return statusTable[st]
	}
	return &Error{Code: Status(st), Message: C.GoString(C.fqdb_status_message(st))}
}
