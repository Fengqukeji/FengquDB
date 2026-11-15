mod common;

use common::TempDir;
use fengqudb::{Error, Options, Store, WriteMode, WriteOptions};

#[test]
fn write_policy() -> Result<(), Box<dyn std::error::Error>> {
    let root = TempDir::new("policy");
    std::fs::create_dir_all(root.path())?;
    let dir = root.path().join("players");
    let db = Store::open(&dir, &Options::default())?;
    let world = Store::open(root.path().join("world"), &Options::default())?;
    let defaults = WriteOptions::default();
    assert_eq!(db.write_options()?, defaults);
    let mut policy = WriteOptions { mode: WriteMode::Performance, period_ms: 75, threshold_bytes: 2 << 20 };
    db.configure_write(&policy)?;
    db.put(b"name", b"Alice")?;
    assert_eq!(db.get(b"name")?.as_deref(), Some(&b"Alice"[..]));
    policy.mode = WriteMode::Reliable;
    db.configure_write(&policy)?;
    db.put(b"paid", b"yes")?;
    let info = db.info()?;
    assert_eq!(info.version, info.durable_version);
    assert_eq!(world.write_options()?, defaults);
    for invalid in [WriteOptions { period_ms: 0, ..policy }, WriteOptions { threshold_bytes: 65 << 20, ..policy }] {
        assert!(db.configure_write(&invalid).is_err());
        assert_eq!(db.write_options()?, policy);
    }
    db.close();
    assert_eq!(db.write_options(), Err(Error::Closed));
    assert_eq!(db.configure_write(&defaults), Err(Error::Closed));
    assert_eq!(db.reset_write_options(), Err(Error::Closed));
    drop(db);
    let db = Store::open(&dir, &Options::default())?;
    assert_eq!(db.write_options()?, policy);
    assert_eq!(db.get(b"paid")?.as_deref(), Some(&b"yes"[..]));
    db.reset_write_options()?;
    assert_eq!(db.write_options()?, defaults);
    drop(db);
    let db = Store::open(&dir, &Options::default())?;
    assert_eq!(db.write_options()?, defaults);
    Ok(())
}
