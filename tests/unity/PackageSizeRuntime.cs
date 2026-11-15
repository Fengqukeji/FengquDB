// Copied to the minimal Player project only after its baseline build.
using System;
using System.IO;
using UnityEngine;
using FengquDB;
using FengquDB.Entities;

[FqEntity("student/")]
public partial class SizeStudent {
    [FqKey, FqField(1)] public int Id { get; set; }
    [FqField(2)] public string Name { get; set; } = "";
    [FqField(3)] public int Score { get; set; }
}

public sealed class PackageSizeRuntime : MonoBehaviour {
    private void Start() {
        try {
            using (var db = new Store(Path.Combine(Application.persistentDataPath, "size-probe"))) {
                db.Put(new SizeStudent { Id=7, Name="Alice", Score=95 });
                if (db.GetEntity<SizeStudent>(7).Score != 95) throw new Exception("Entity roundtrip failed");
                db.Sync();
            }
            Debug.Log("PASS Player SDK native roundtrip");
            if (Application.isBatchMode) Application.Quit(0);
        } catch (Exception e) {
            Debug.LogException(e);
            if (Application.isBatchMode) Application.Quit(1);
        }
    }
}
