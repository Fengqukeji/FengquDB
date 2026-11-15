import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.charset.CodingErrorAction;
import java.util.Arrays;

final class FqModelWire {
    static final int LIMIT = 16777216;
    static byte[] text(String v) {
        for (int i=0;i<v.length();++i) {
            char c=v.charAt(i);
            if (Character.isHighSurrogate(c)) {
                if (++i>=v.length() || !Character.isLowSurrogate(v.charAt(i))) throw new IllegalArgumentException("Invalid UTF-16");
            } else if (Character.isLowSurrogate(c)) throw new IllegalArgumentException("Invalid UTF-16");
        }
        return v.getBytes(StandardCharsets.UTF_8);
    }
    static String text(byte[] v) {
        try { return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(v)).toString(); }
        catch (java.nio.charset.CharacterCodingException e) { throw new IllegalArgumentException("Invalid UTF-8",e); }
    }
    static final class Writer {
        byte[] data; int pos; final int start; final boolean fixed;
        Writer(byte[] destination,int offset) {
            if(offset<0 || (destination!=null && offset>destination.length))throw new IllegalArgumentException("Invalid offset");
            data=destination==null?new byte[256]:destination;pos=offset;start=offset;fixed=destination!=null;
        }
        void room(int n) {
            if(n<0 || n>LIMIT-(pos-start))throw new IllegalArgumentException("Entity exceeds 16 MiB");
            if(n<=data.length-pos)return;
            if(fixed)throw new IllegalArgumentException("Buffer too small");
            data=Arrays.copyOf(data,Math.min(LIMIT,Math.max(pos+n,data.length*2)));
        }
        void u(long v) {
            // At most ten bytes; reserve exact demand for caller-owned buffers.
            do {room(1);data[pos++]=(byte)((v&127)|((v&~127L)!=0?128:0));v>>>=7;}while(v!=0);
        }
        void raw(byte[] v){room(v.length);System.arraycopy(v,0,data,pos,v.length);pos+=v.length;}
        void b(byte[] v){u(v.length);raw(v);}
        void fixed(long v,int n){room(n);for(int i=0;i<n;++i)data[pos++]=(byte)(v>>>(8*i));}
        byte[] finish(){return Arrays.copyOfRange(data,start,pos);}
    }
    static final class Reader {
        final byte[] data; int pos;
        Reader(byte[] source){if(source.length>LIMIT)throw new IllegalArgumentException("Entity exceeds 16 MiB");data=source;}
        long u(){long v=0;for(int i=0;i<10 && pos<data.length;i++){int b=data[pos++]&255;if(i==9 && b>1)break;v|=(long)(b&127)<<(i*7);if(b<128)return v;}throw new IllegalArgumentException("Invalid varint");}
        int tag(){long t=u();if(t<=0 || t>0xffffffffL || (t>>>3)==0 || (t&7)>5)throw new IllegalArgumentException("Invalid tag");return (int)t;}
        int take(long n){if(n<0 || n>data.length-pos)throw new IllegalArgumentException("Truncated value");int begin=pos;pos+=(int)n;return begin;}
        byte[] b(){long n=u();int begin=take(n);return Arrays.copyOfRange(data,begin,pos);}
        long fixed(int n){int begin=take(n);long v=0;for(int i=0;i<n;++i)v|=(long)(data[begin+i]&255)<<(8*i);return v;}
        void skip(int t,int depth){switch(t&7){case 0:u();return;case 1:take(8);return;case 2:take(u());return;case 5:take(4);return;case 3:if(depth>=64)break;while(pos<data.length){int inner=tag();if((inner&7)==4){if((inner>>>3)!=(t>>>3))throw new IllegalArgumentException("Mismatched group");return;}skip(inner,depth+1);}}throw new IllegalArgumentException("Invalid group");}
    }
    static int i32(long v){if(v<Integer.MIN_VALUE || v>Integer.MAX_VALUE)throw new IllegalArgumentException("sint32 overflow");return (int)v;}
    static long u32(long v){if(v<0 || v>0xffffffffL)throw new IllegalArgumentException("uint32 overflow");return v;}
    static long z(long v){return (v<<1)^(v>>63);}
    static long unz(long v){return (v>>>1)^-(v&1);}
    static boolean bool(long v){if(v!=0 && v!=1)throw new IllegalArgumentException("Invalid bool");return v==1;}
    static byte[] key(byte[] prefix,byte[] id){if(id.length>65536-prefix.length)throw new IllegalArgumentException("Key exceeds 64 KiB");byte[] b=Arrays.copyOf(prefix,prefix.length+id.length);System.arraycopy(id,0,b,prefix.length,id.length);return b;}
}
