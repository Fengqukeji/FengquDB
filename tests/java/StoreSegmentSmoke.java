import io.fengqudb.Store;
import java.nio.*;
import java.nio.file.*;
import java.util.*;
public final class StoreSegmentSmoke {
    static byte[] key="blob/large".getBytes(java.nio.charset.StandardCharsets.UTF_8);
    static void check(boolean ok){if(!ok)throw new AssertionError("segmented Java model");}
    static void revision(byte[] value,int revision){ByteBuffer.wrap(value).order(ByteOrder.LITTLE_ENDIAN).putLong(0,revision);}
    public static void main(String[] args) throws Exception {
        Path path=Paths.get(args[0]);String base=args[0]+"-base",archive=args[0]+"-increment",restored=args[0]+"-restored";
        Store.Options options=new Store.Options();options.segmented=true;options.buffered=true;options.cacheBytes=4L<<20;
        byte[] value=new byte[8<<20];Arrays.fill(value,(byte)'p');long before;
        try(Store store=new Store(path.toString(),options)) {
            for(int i=0;i<5;++i){revision(value,i);store.putIfChanged(key,value);}
            try(Store.Snapshot old=store.snapshot()) {
                byte[] expectedOld=value.clone();
                for(int i=5;i<10;++i){revision(value,i);store.putIfChanged(key,value);}
                StoreSmoke.operations(store);store.checkpoint();
                check(Arrays.equals(store.get(key),value) && Arrays.equals(old.get(key),expectedOld));
                try(var stream=Files.list(path)){before=stream.filter(p->p.toString().endsWith(".fdb")).count();}
                check(before>=2);
                long from=store.backup(base).snapshotVersion;store.holdHistory("chain".getBytes(),from);
                revision(value,10);store.putIfChanged(key,value);store.compact();
                check(Arrays.equals(old.get(key),expectedOld) && store.historyInfo().retainedGenerations>=1);
                store.backupIncremental(archive,from);
            }
            store.releaseHistory("chain".getBytes());store.compact();store.verify();
            check(store.historyInfo().retainedGenerations==0 && Arrays.equals(store.get(key),value));
        }
        Store.Options existing=new Store.Options();existing.create=false;
        try(Store store=new Store(path.toString(),existing)){check(Arrays.equals(store.get(key),value));}
        Store.recoverChain(base,new String[]{archive},restored,0,0);
        try(Store copy=new Store(restored)){check(Arrays.equals(copy.get(key),value));copy.verify();}
        for(String output:new String[]{path.toString(),base,archive,restored})StoreSmoke.clean(Paths.get(output));
        System.out.println("PASS Java 8 MiB values, "+before+" segments, old snapshot, indexes, history, backup chain and reopen");
    }
}
