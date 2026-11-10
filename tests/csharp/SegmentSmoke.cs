using System;
using System.Buffers.Binary;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using FengquDB;
static class SegmentSmoke {
    static readonly byte[] Key=Encoding.UTF8.GetBytes("blob/large");
    static void Check(bool ok){if(!ok)throw new Exception("segmented C# model");}
    static void Revision(byte[] value,int revision)=>BinaryPrimitives.WriteUInt64LittleEndian(value,(ulong)revision);
    static void Main(string[] args) {
        NativeLibrary.SetDllImportResolver(typeof(Store).Assembly,(name,assembly,flags)=>
            name=="fengqudb"?NativeLibrary.Load(args[1]):IntPtr.Zero);
        string path=args[0],backup=path+"-base",archive=path+"-increment",restored=path+"-restored";
        var options=new StoreOptions{Segmented=true,Buffered=true,CacheBytes=4UL<<20};
        byte[] value=new byte[8<<20];Array.Fill(value,(byte)'p');int before;
        using(var store=new Store(path,options)) {
            for(int i=0;i<5;++i){Revision(value,i);store.PutIfChanged(Key,value);}
            using(var old=store.Snapshot()) {
                byte[] expectedOld=(byte[])value.Clone();
                for(int i=5;i<10;++i){Revision(value,i);store.PutIfChanged(Key,value);}
                StoreSmoke.Operations(store);store.Checkpoint();
                Check(store.Get(Key)!.SequenceEqual(value) && old.Get(Key)!.SequenceEqual(expectedOld));
                before=Directory.GetFiles(path,"*.fdb").Length;Check(before>=2);
                ulong from=store.Backup(backup).SnapshotVersion;store.HoldHistory(Encoding.UTF8.GetBytes("chain"),from);
                Revision(value,10);store.PutIfChanged(Key,value);store.Compact();
                Check(old.Get(Key)!.SequenceEqual(expectedOld) && store.HistoryInfo().RetainedGenerations>=1);
                store.BackupIncremental(archive,from);
            }
            store.ReleaseHistory(Encoding.UTF8.GetBytes("chain"));store.Compact();store.Verify();
            Check(store.HistoryInfo().RetainedGenerations==0 && store.Get(Key)!.SequenceEqual(value));
        }
        using(var store=new Store(path,new StoreOptions{Create=false}))Check(store.Get(Key)!.SequenceEqual(value));
        Store.RecoverChain(backup,new[]{archive},restored);
        using(var copy=new Store(restored)){Check(copy.Get(Key)!.SequenceEqual(value));copy.Verify();}
        foreach(var output in new[]{path,backup,archive,restored})StoreSmoke.Clean(output);
        Console.WriteLine($"PASS C# 8 MiB values, {before} segments, old snapshot, indexes, history, backup chain and reopen");
    }
}
