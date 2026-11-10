using System;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using FengquDB;
using UnityEngine;

public sealed class FengquSave : MonoBehaviour {
    private Store database;
    private static readonly byte[] PlayerKey = Encoding.UTF8.GetBytes("player/42");

    private void Awake() {
        database = Store.OpenAt(Path.Combine(Application.persistentDataPath, "db"), "main");
    }

    public async Task SavePlayerAsync(byte[] protobufBytes, bool forceReliable = false) {
        if (database == null)
            throw new InvalidOperationException("Save database is closed");
        // Submit copies the input before returning; awaiting does not block the
        // Unity main thread. Keep the receipt alive until the selected guarantee completes.
        using (var receipt =
                   database.SubmitPut(PlayerKey, protobufBytes, forceReliable: forceReliable)) {
            await receipt.WaitAsync();
        }
    }

    public byte[] LoadPlayer() {
        if (database == null)
            throw new InvalidOperationException("Save database is closed");
        return database.Get(PlayerKey); // null means no save; empty bytes are a value.
    }

    private void OnApplicationPause(bool paused) {
        if (paused && database != null)
            database.Sync();
    }

    private void OnDestroy() {
        if (database != null)
            database.Dispose();
        database = null;
    }
}
