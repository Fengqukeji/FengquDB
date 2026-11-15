// Unity Editor integration probe. Copy this file, examples/csharp/Student.cs,
// the RoslynAnalyzer-labelled generator into one Editor asmdef, referencing the
// runtime source asmdef supplied in bindings/csharp.
#if UNITY_EDITOR
using System;
using System.IO;
using System.Runtime.InteropServices;
using FengquDB;
using FengquDB.Examples;
using UnityEditor;
using UnityEngine;

namespace FengquDB.UnityTests {
[InitializeOnLoad]
public static class EntityProbe {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr LoadLibraryW(string path);
    [Serializable] public sealed class Result {
        public bool ok;
        public string unity = "", error = "";
        public int encodedBytes, score;
    }
    static EntityProbe() { EditorApplication.delayCall += Run; }
    [MenuItem("Tools/FengquDB/Run Entity Probe")]
    public static void Run() {
        // The request lives outside Assets, avoiding recursive import/reload.
        string request = Path.Combine(Directory.GetParent(Application.dataPath).FullName, "Temp/fqdb-entity-probe.txt");
        if (!File.Exists(request)) return;
        string[] config = File.ReadAllLines(request);
        File.Delete(request);
        var result = new Result { unity = Application.unityVersion };
        try {
            if (LoadLibraryW(config[0]) == IntPtr.Zero) throw new Exception("Native DLL load failed " + Marshal.GetLastWin32Error());
            var value = new Student { Id = 1, Name = "张三🙂", Score = 95 };
            byte[] encoded = new byte[value.EncodedSize()];
            result.encodedBytes = value.EncodeTo(encoded);
            if (Student.Decode(encoded).Name != value.Name || Student.ReadScore(encoded) != 95)
                throw new Exception("Generated wire roundtrip failed");
            using (var db = new Store(config[1])) {
                db.Put(value, forceReliable: true);
                db.Put("score", 95); db.Put("name", "张三");
                if (db.GetEntity<Student>(1).Name != value.Name || db.Get<int>("score") != 95 || db.Get<string>("name") != "张三")
                    throw new Exception("Database typed roundtrip failed");
                using (var reader = db.PrepareValue<int>("score")) result.score = reader.Read();
                using (var tx = db.Begin()) { tx.Put("transaction", 42); tx.Commit(forceReliable: true); }
                if (db.Get<int>("transaction") != 42) throw new Exception("Transaction failed");
                var rows = Q.From("student/").Where(Student.Cols.Score >= 90L).Select(Student.Cols.Name).Run(db);
                if (rows.Count != 1) throw new Exception("Generated query column failed");
                db.Sync();
            }
            using (var db = new Store(config[1]))
                if (db.GetEntity<Student>(1).Score != 95) throw new Exception("Reopen failed");
            result.ok = true;
        } catch (Exception error) { result.error = error.ToString(); }
        File.WriteAllText(config[2], JsonUtility.ToJson(result, true));
        if (result.ok) Debug.Log("FengquDB generated entity probe PASS " + result.unity);
        else Debug.LogError("FengquDB generated entity probe FAIL " + result.error);
    }
}
}
#endif
