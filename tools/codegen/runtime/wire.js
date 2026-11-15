const _fqDecoder = new TextDecoder('utf-8', {fatal: true, ignoreBOM: true});
const _fqLimit = 16 * 1024 * 1024;
function _fqText(v) {
  if (typeof v !== 'string') throw new TypeError('Expected string');
  for (let i=0; i<v.length; ++i) {
    const c=v.charCodeAt(i);
    if (c>=0xd800 && c<=0xdbff) {
      const n=v.charCodeAt(++i);
      if (!(n>=0xdc00 && n<=0xdfff)) throw new Error('Invalid UTF-16');
    } else if (c>=0xdc00 && c<=0xdfff) throw new Error('Invalid UTF-16');
  }
  return Buffer.from(v, 'utf8');
}
function _fqInt(v, bits, signed) {
  if (bits===64) {
    if (typeof v!=='bigint') throw new TypeError('64-bit fields require bigint');
  } else {
    if (typeof v!=='number' || !Number.isInteger(v)) throw new TypeError('Expected integer');
    v=BigInt(v);
  }
  const b=BigInt(bits), min=signed ? -(1n<<(b-1n)) : 0n, max=signed ? 1n<<(b-1n) : 1n<<b;
  if (v<min || v>=max) throw new RangeError('Integer outside schema range');
  return v;
}
class _FqWriter {
  constructor(destination) { this.data=destination || Buffer.allocUnsafe(256); this.pos=0; this.fixed=!!destination; }
  room(n) {
    if (n>_fqLimit-this.pos) throw new RangeError('Entity exceeds 16 MiB');
    if (n<=this.data.length-this.pos) return;
    if (this.fixed) throw new RangeError('Output buffer too small');
    const b=Buffer.allocUnsafe(Math.min(_fqLimit, Math.max(this.pos+n, this.data.length*2)));
    b.set(this.data.subarray(0,this.pos)); this.data=b;
  }
  u(v) { v=BigInt(v); let n=1,x=v;while(x>=128n){++n;x>>=7n;}this.room(n);while(v>=128n){this.data[this.pos++]=Number(v&127n)|128;v>>=7n;}this.data[this.pos++]=Number(v); }
  raw(b) { if (!(b instanceof Uint8Array)) throw new TypeError('Expected Uint8Array'); this.room(b.length); this.data.set(b,this.pos);this.pos+=b.length; }
  b(v) { this.u(v.length); this.raw(v); }
  f(v,n) { this.room(n);const d=new DataView(this.data.buffer,this.data.byteOffset+this.pos,n);if(n===4)d.setFloat32(0,v,true);else d.setFloat64(0,v,true);this.pos+=n; }
  finish() { return this.data.subarray(0,this.pos); }
}
class _FqReader {
  constructor(data) { if(!(data instanceof Uint8Array)||data.length>_fqLimit)throw new Error('Invalid entity bytes');this.data=data;this.pos=0; }
  u() {
    let v=0n;
    for(let i=0;i<10 && this.pos<this.data.length;i++) {
      const b=this.data[this.pos++];if(i===9 && b>1)break;
      v|=BigInt(b&127)<<BigInt(i*7);if(b<128)return v;
    }
    throw new Error('Invalid or truncated varint');
  }
  tag() {const t=this.u();if(t>0xffffffffn || t>>3n===0n || (t&7n)>5n)throw new Error('Invalid tag');return Number(t);}
  take(n) {if(n>BigInt(this.data.length-this.pos))throw new Error('Truncated value');const p=this.pos;this.pos+=Number(n);return this.data.subarray(p,this.pos);}
  b() {return this.take(this.u());}
  f(n) {const b=this.take(BigInt(n)),d=new DataView(b.buffer,b.byteOffset,n);return n===4?d.getFloat32(0,true):d.getFloat64(0,true);}
  skip(t,depth=0) {
    switch(t&7) {
      case 0:this.u();return;case 1:this.take(8n);return;case 2:this.b();return;case 5:this.take(4n);return;
      case 3:
        if(depth>=64)break;
        while(this.pos<this.data.length){const inner=this.tag();if((inner&7)===4){if(Math.floor(inner/8)!==Math.floor(t/8))throw new Error('Mismatched group');return;}this.skip(inner,depth+1);}
    }
    throw new Error('Invalid group');
  }
}
function _fqSigned(v,bits) {const s=(v>>1n)^-(v&1n);return bits===64?s:Number(_fqInt(Number(s),32,true));}
function _fqUnsigned(v,bits) {if(bits===32 && v>0xffffffffn)throw new RangeError('uint32 overflow');return bits===64?v:Number(v);}
function _fqBool(v) {if(v>1n)throw new Error('Invalid bool');return v===1n;}
function _fqKey(prefix,id) {const b=Buffer.concat([prefix,typeof id==='string'?_fqText(id):Buffer.from(id.toString(),'ascii')]);if(b.length>65536)throw new Error('Key exceeds 64 KiB');return b;}
