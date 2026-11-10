using System;
using System.Globalization;
using System.IO;
using System.Text;
using Google.Protobuf;
using PlayerSave = FengquDB.Examples.V2.PlayerSave;

namespace FengquDB.Examples {
// Include this source and generated DTOs in the business/Unity assembly.
// The core FengquDB.Managed assembly has no Google.Protobuf dependency.
public sealed class PlayerSaves : IDisposable {
    readonly Store database;
    static byte[] Key(ulong id) =>
        Encoding.UTF8.GetBytes("player/" + id.ToString(CultureInfo.InvariantCulture));

    public PlayerSaves(string persistentRoot, string name = "main", bool create = true) {
        database = Store.OpenAt(Path.Combine(persistentRoot, "db"), name,
                                new StoreOptions { Create = create });
        // This adapter exposes synchronous Save. Use SubmitPut/WaitAsync for a
        // periodic-mode Unity adapter that must not block its main thread.
        database.ConfigureWrite(new StoreWriteOptions { Mode = StoreWriteMode.Reliable });
    }
    public PlayerSave? Load(ulong id) {
        return database.ReadShared < PlayerSave
            ? > (Key(id), row => row.Found ? PlayerSave.Parser.ParseFrom(row.Value) : null);
    }
    public bool Save(PlayerSave value) {
        if (value == null)
            throw new ArgumentNullException(nameof(value));
        if (!value.HasPlayerId)
            throw new ArgumentException("Player ID must be explicitly present", nameof(value));
        return database.PutIfChanged(Key(value.PlayerId), value.ToByteArray()).Changed;
    }
    public void Sync() => database.Sync();
    public void Dispose() => database.Dispose();
}
}
