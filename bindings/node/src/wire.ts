/** protobuf 线格式的最小写入器（与 Python 的 _u/_b/_feature_varint 字节一致）。 */

const MASK64 = (1n << 64n) - 1n;
const encoder = new TextEncoder();

export class Writer {
  private buf: Uint8Array;
  private len = 0;
  constructor(capacity = 256) {
    this.buf = new Uint8Array(capacity);
  }
  private grow(extra: number): void {
    if (this.len + extra <= this.buf.length) return;
    let n = this.buf.length * 2;
    while (n < this.len + extra) n *= 2;
    const next = new Uint8Array(n);
    next.set(this.buf.subarray(0, this.len));
    this.buf = next;
  }
  byte(b: number): void {
    this.grow(1);
    this.buf[this.len++] = b;
  }
  /** 无符号 varint；负数按 64 位补码处理，超出 64 位的 bigint 被截断（与 Python 一致）。 */
  varint(value: number | bigint): void {
    if (typeof value === "number" && value >= 0 && value <= Number.MAX_SAFE_INTEGER) {
      let v = value;
      this.grow(10);
      while (v >= 128) {
        this.buf[this.len++] = (v % 128) | 128;
        v = Math.floor(v / 128);
      }
      this.buf[this.len++] = v;
      return;
    }
    let b = BigInt(value) & MASK64;
    this.grow(10);
    while (b >= 128n) {
      this.buf[this.len++] = Number(b & 127n) | 128;
      b >>= 7n;
    }
    this.buf[this.len++] = Number(b);
  }
  raw(bytes: Uint8Array): void {
    this.grow(bytes.length);
    this.buf.set(bytes, this.len);
    this.len += bytes.length;
  }
  /** 字段 n 的 varint：tag = n*8。 */
  u(n: number, v: number | bigint): void {
    this.varint(n * 8);
    this.varint(v);
  }
  /** 字段 n 的 length-delimited 字节。 */
  b(n: number, v: Uint8Array): void {
    this.varint(n * 8 + 2);
    this.varint(v.length);
    this.raw(v);
  }
  fixed64(n: number, lo: bigint): void {
    this.varint(n * 8 + 1);
    this.grow(8);
    new DataView(this.buf.buffer, this.buf.byteOffset + this.len, 8).setBigUint64(0, lo & MASK64, true);
    this.len += 8;
  }
  double(n: number, v: number): void {
    this.varint(n * 8 + 1);
    this.grow(8);
    new DataView(this.buf.buffer, this.buf.byteOffset + this.len, 8).setFloat64(0, v, true);
    this.len += 8;
  }
  finish(): Uint8Array {
    return this.buf.slice(0, this.len);
  }
}

export function toBytes(v: string | Uint8Array | ArrayBuffer | undefined): Uint8Array {
  if (v === undefined) return new Uint8Array(0);
  if (typeof v === "string") return encoder.encode(v);
  if (v instanceof ArrayBuffer) return new Uint8Array(v);
  return v;
}

export function hexToBytes(hex: string): Uint8Array {
  if (hex.length % 2) throw new Error("odd hex length");
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
  return out;
}

export function bytesToHex(b: Uint8Array): string {
  let s = "";
  for (let i = 0; i < b.length; i++) s += b[i].toString(16).padStart(2, "0");
  return s;
}
