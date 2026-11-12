/** 核心库状态码（include/fengqudb.h）。 */
export const Status = {
  OK: 0,
  NOT_FOUND: 1,
  END: 2,
  INVALID_ARGUMENT: 3,
  IO_ERROR: 4,
  CORRUPT: 5,
  LOCKED: 6,
  CONFLICT: 7,
  NO_MEMORY: 8,
  LIMIT: 9,
  COMMIT_UNKNOWN: 10,
  RECOVERY_REQUIRED: 11,
  UNSUPPORTED: 12,
  INTERNAL_ERROR: 13,
  HISTORY_LOST: 14,
  ALREADY_EXISTS: 15,
  BUFFER_TOO_SMALL: 16,
  TIMEOUT: 17,
  NOT_MODIFIED: 18,
  CANCELLED: 19,
  CONSTRAINT: 20,
  ASYNC_REQUIRED: 21,
  /** 仅绑定使用：句柄已关闭或共享视图已释放。 */
  CLOSED: -1,
} as const;

const NAMES = new Map<number, string>(Object.entries(Status).map(([k, v]) => [v as number, k]));

/** 所有非 OK 的核心库状态都映射为该错误，保留数值码。 */
export class FengquDBError extends Error {
  /** 数值状态码；-1 表示句柄已关闭/已释放。 */
  readonly status: number;
  constructor(status: number, message?: string) {
    super(message ?? `FengquDB status ${status}`);
    this.name = "FengquDBError";
    this.status = status;
  }
  /** 状态码的符号名，例如 "CONFLICT"。 */
  get statusName(): string {
    return NAMES.get(this.status) ?? "UNKNOWN";
  }
}
