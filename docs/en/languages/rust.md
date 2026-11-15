# Rust getting started

[简体中文](../../languages/rust.md)

[API and examples](../api/rust.md)

Open a database handle at application startup, share it across application modules, and close it at shutdown.
## 1. Reference the included crate

Download this language's SDK ZIP and extract its `FengquDB` folder into your project root. Add this under dependencies in Cargo.toml:

```toml
fengqudb = { path = "FengquDB/Crates/fengqudb" }
```

The engine is precompiled. The build entry point below deploys its runtime beside the Cargo executable.

## 2. Enable automatic generation

In build.rs at the application project root, write `include!("FengquDB/generate.rs");` to use the included generation entry point. If you already have a build.rs, merge its generation logic:

```text
project/
  Cargo.toml
  build.rs                          <- Includes the SDK's generate.rs
  src/main.rs
  src/student.rs
  FengquDB/Tools/fqdb-codegen.exe          <- Windows tool
```

`src/student.rs`:

```rust
// fqdb:entity prefix=student/ key=id
#[derive(Default)]
pub struct Student { pub id:i64, pub name:String, pub score:i32 }
```

## 3. Call and run

`src/main.rs`:

```rust
use fengqudb::{Store, Options};
mod student;
#[allow(dead_code)]
mod codecs { include!(concat!(env!("FQDB_GENERATED_DIR"), "/rust/fq_codecs.rs")); }
use codecs::student::StudentFqCodec as Codec;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    std::fs::create_dir_all("save")?;
    let db = Store::open("save/main", &Options::default())?;
    let student = student::Student { id:7, name:"Alice".into(), score:95 };
    db.put(&Codec::key(&student)?, &Codec::encode(&student)?)?;
    let data = db.get(&Codec::key(&student)?)?.unwrap();
    let saved = Codec::decode(&data)?;
    println!("{} {}", saved.value.name, saved.value.score);
    db.sync()?;
    Ok(())
}
```

Run `cargo run`; `Alice 95` confirms success. Generation runs during the build, with output under Cargo's OUT_DIR; the include above refers to that directory. Rebuild after editing pub fields. To store strings or your own bytes, call `db.put(b"name", b"Alice")` directly.

## Manual generation

The tool is inside the `FengquDB` folder. On Windows, double-click `FengquDB/fqdb-generate.bat`; on macOS, double-click `FengquDB/fqdb-generate.command`; on Linux, run `sh FengquDB/fqdb-generate.command` from the project directory.

The script locates the application project and uses the generator in the SDK's Tools directory. Success prints `Entity codecs are up to date.` The initial output is under `Generated`; existing automatic-generation settings are reused. After editing entities, build again or double-click the script. Keep `FengquDB.schema.lock.json`, which preserves field numbers, with your application sources.

## Optional: data path, multiple databases, and write mode

The examples above use the default settings. Refer to this section when you want to change them.

Set a writable data directory in your application. Different subdirectories are independent databases:

```rust
use std::path::Path;
use fengqudb::{WriteMode, WriteOptions};

let root = Path::new("save"); // Replace with the host's writable data directory
std::fs::create_dir_all(root)?;
let players = Store::open(root.join("players"), &Options::default())?;
let world = Store::open(root.join("world"), &Options::default())?;
let mut options = WriteOptions::default();
options.mode = WriteMode::Performance;
options.period_ms = 50;
options.threshold_bytes = 1024 * 1024;
players.configure_write(&options)?;

options.mode = WriteMode::Reliable;
players.configure_write(&options)?; // Require durable storage for every write to this database
// players.reset_write_options()?; // Restore default performance mode, 50 ms / 1 MiB
```

New databases default to performance mode: writes return once applied in memory, and new values are immediately readable. Flushing occurs at the period or byte threshold, with no empty flushes while idle. A crash or power loss can lose unflushed data. Reliable mode waits for durable storage before returning. Each database stores its data and settings separately; settings survive reopening. Switching modes can wait for a flush, so configure at initialization when possible. Reuse the open database handle.

Publish with cargo build --release and copy the executable and adjacent database runtime from target/release. The manual script reuses the output location recorded by Cargo when available; otherwise its first output goes to Generated, while normal builds continue using OUT_DIR.
