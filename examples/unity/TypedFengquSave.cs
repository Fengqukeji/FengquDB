#nullable enable
using System;
using FengquDB.Examples;
using FengquDB.Examples.V2;
using UnityEngine;

// Requires generated player_v2 DTOs, PlayerSaves.cs and Google.Protobuf in the
// business assembly, plus the FengquDB managed/native plugin for this platform.
public sealed class TypedFengquSave : MonoBehaviour {
    private PlayerSaves? saves;
    private PlayerSaves Live =>
        saves ?? throw new InvalidOperationException("Save database is closed");
    private void Awake() {
        saves = new PlayerSaves(Application.persistentDataPath);
    }
    public PlayerSave? LoadPlayer() => Live.Load(42); // null means no save.
    public void SavePlayer(PlayerSave value) => Live.Save(value);
    private void OnApplicationPause(bool paused) {
        if (paused && saves != null)
            saves.Sync();
    }
    private void OnDestroy() {
        if (saves != null)
            saves.Dispose();
        saves = null;
    }
}
