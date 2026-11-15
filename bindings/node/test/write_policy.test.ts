import test from "node:test";
import assert from "node:assert/strict";
import * as path from "node:path";
import { Store } from "../src";
import { tmpDir, rmDir } from "./helpers";

test("write policy: independent paths, persistence, invalid settings, close", () => {
  const root = tmpDir();
  const dir = path.join(root, "players");
  let db = Store.open(dir);
  let world: Store | undefined;
  try {
    world = Store.open(path.join(root, "world"));
    const defaults = { mode: "performance", periodMs: 50, thresholdBytes: 1048576n };
    assert.deepEqual(db.writeOptions(), defaults);
    db.configureWrite({ mode: "performance", periodMs: 75, thresholdBytes: 2 * 1024 * 1024 });
    db.put("name", "Alice");
    assert.equal(Buffer.from(db.get("name")!).toString(), "Alice");
    db.configureWrite({ mode: "reliable" });
    const policy = { mode: "reliable", periodMs: 75, thresholdBytes: 2097152n };
    assert.deepEqual(db.writeOptions(), policy);
    db.put("paid", "yes");
    const info = db.info();
    assert.equal(info.version, info.durableVersion);
    assert.deepEqual(world.writeOptions(), defaults);
    for (const bad of [{mode:"bad"}, {periodMs:0}, {periodMs:2 ** 32 + 50},
      {thresholdBytes:65 * 1024 * 1024}, {thresholdBytes:-1}, {periodMs:1.5}, null]) {
      assert.throws(() => db.configureWrite(bad as any));
      assert.deepEqual(db.writeOptions(), policy);
    }
    db.close();
    assert.throws(() => db.writeOptions());
    assert.throws(() => db.configureWrite({mode:"reliable"}));
    assert.throws(() => db.resetWriteOptions());
    db = Store.open(dir);
    assert.deepEqual(db.writeOptions(), policy);
    assert.equal(Buffer.from(db.get("paid")!).toString(), "yes");
    db.resetWriteOptions();
    assert.deepEqual(db.writeOptions(), defaults);
    db.close();
    db = Store.open(dir);
    assert.deepEqual(db.writeOptions(), defaults);
  } finally {
    db.close();
    world?.close();
    rmDir(root);
  }
});
