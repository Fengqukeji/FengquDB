import (
    "encoding/binary"
    "errors"
    "math"
    "strconv"
    "unicode/utf8"
)

var fqModelError = errors.New("invalid FengquDB model wire data, range, or size")

func fqAppendU(b []byte, v uint64) []byte {
    for v >= 128 { b=append(b,byte(v)|128);v>>=7 }
    return append(b,byte(v))
}
func fqAppendB(b, v []byte) []byte { b=fqAppendU(b,uint64(len(v)));return append(b,v...) }
func fqBytesSize(n int) int {size:=n+1;for n>=128 {size++;n>>=7};return size}
func fqAppendS(b []byte, v string) []byte { b=fqAppendU(b,uint64(len(v)));return append(b,v...) }
func fqAppendFixed(b []byte,v uint64,n int) []byte {for i:=0;i<n;i++ {b=append(b,byte(v>>uint(8*i)))};return b}
func fqZ(v int64) uint64 {return uint64(v<<1)^uint64(v>>63)}
func fqUnz(v uint64) int64 {return int64(v>>1)^-int64(v&1)}
func fqBool(v bool) uint64 {if v{return 1};return 0}
func fqFloat32(v float32) uint64 {return uint64(math.Float32bits(v))}
func fqFloat64(v float64) uint64 {return math.Float64bits(v)}
func fqSignedKey(v int64) string {return strconv.FormatInt(v,10)}
func fqUnsignedKey(v uint64) string {return strconv.FormatUint(v,10)}
func fqKey(prefix,id string) ([]byte,error) {if len(prefix)+len(id)>65536 || !utf8.ValidString(id){return nil,fqModelError};return []byte(prefix+id),nil}

type fqModelReader struct {data []byte;pos int;err error}
func (r *fqModelReader) u() uint64 {
    var v uint64
    for i:=0;i<10 && r.err==nil && r.pos<len(r.data);i++ {
        b:=r.data[r.pos];r.pos++;if i==9 && b>1{break};v|=uint64(b&127)<<uint(i*7);if b<128{return v}
    }
    r.err=fqModelError;return 0
}
func (r *fqModelReader) tag() uint32 {t:=r.u();if t>0xffffffff || t>>3==0 || t&7>5{r.err=fqModelError};return uint32(t)}
func (r *fqModelReader) take(n uint64) []byte {if r.err!=nil || n>uint64(len(r.data)-r.pos){r.err=fqModelError;return nil};p:=r.pos;r.pos+=int(n);return r.data[p:r.pos]}
func (r *fqModelReader) b() []byte {return r.take(r.u())}
func (r *fqModelReader) str() string {b:=r.b();if !utf8.Valid(b){r.err=fqModelError};return string(b)}
func (r *fqModelReader) fixed(n int) uint64 {b:=r.take(uint64(n));if r.err!=nil{return 0};if n==4{return uint64(binary.LittleEndian.Uint32(b))};return binary.LittleEndian.Uint64(b)}
func (r *fqModelReader) i32() int32 {v:=fqUnz(r.u());if v<math.MinInt32 || v>math.MaxInt32 {r.err=fqModelError};return int32(v)}
func (r *fqModelReader) u32() uint32 {v:=r.u();if v>math.MaxUint32{r.err=fqModelError};return uint32(v)}
func (r *fqModelReader) boolean() bool {v:=r.u();if v>1{r.err=fqModelError};return v==1}
func (r *fqModelReader) skip(t uint32,depth int) {
    switch t&7 {
    case 0:r.u();return
    case 1:r.take(8);return
    case 2:r.b();return
    case 5:r.take(4);return
    case 3:
        if depth>=64 {break}
        for r.err==nil && r.pos<len(r.data){inner:=r.tag();if inner&7==4 {if inner>>3!=t>>3{r.err=fqModelError};return};r.skip(inner,depth+1)}
    }
    r.err=fqModelError
}
