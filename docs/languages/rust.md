# Rust 接入

[API、用途和调用例子](../api/rust.md)

应用启动时打开数据库句柄，交给各业务模块复用，退出时统一关闭。
## 1. 引用随包 crate

下载本语言独立 ZIP，把 SDK 解压为业务项目根目录的 `FengquDB`，Cargo.toml 的 dependencies 中加入：

```toml
fengqudb = { path = "FengquDB/Crates/fengqudb" }
```

数据库引擎已经预编译。下面的构建入口会将运行库部署到 Cargo 可执行文件旁。

## 2. 设置自动生成

在业务项目根目录的 build.rs 中写 `include!("FengquDB/generate.rs");`，即可使用包内的构建生成入口；已有 build.rs 时合并其中的生成逻辑：

```text
项目/
  Cargo.toml
  build.rs                          ← 引用包内 generate.rs
  src/main.rs
  src/student.rs
  FengquDB/Tools/fqdb-codegen.exe          ← Windows 工具
```

`src/student.rs`：

```rust
// fqdb:entity prefix=student/ key=id
#[derive(Default)]
pub struct Student { pub id:i64, pub name:String, pub score:i32 }
```

## 3. 调用并运行

`src/main.rs`：

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

运行 `cargo run`，看到 `Alice 95` 即成功。生成器随构建运行，生成文件在 Cargo 的 OUT_DIR 下；上面的 include 指向生成目录。修改 pub 字段后再编译即可更新。只存字符串或自己的字节时可直接 `db.put(b"name", b"Alice")`。

## 手动生成

生成工具位于 `FengquDB` 文件夹内。Windows 双击 `FengquDB/fqdb-generate.bat`；macOS 双击 `FengquDB/fqdb-generate.command`；Linux 在项目目录执行 `sh FengquDB/fqdb-generate.command`。

脚本自动定位业务项目，使用包内 Tools 目录的生成器。成功显示 `Entity codecs are up to date.`，首次输出在项目 `Generated`；已有自动生成配置时复用它的输出位置。实体修改后再次编译或双击即可。`FengquDB.schema.lock.json` 保存字段编号，随业务代码保存。

## 可选：存档路径、多个数据库和写入模式

前面的接入示例已包含默认配置。需要调整时参考本节。

根目录由项目设置，使用可写的存档目录。不同子目录就是不同数据库：

```rust
use std::path::Path;
use fengqudb::{WriteMode, WriteOptions};

let root = Path::new("save"); // 换成宿主提供的存档目录
std::fs::create_dir_all(root)?;
let players = Store::open(root.join("players"), &Options::default())?;
let world = Store::open(root.join("world"), &Options::default())?;
let mut options = WriteOptions::default();
options.mode = WriteMode::Performance;
options.period_ms = 50;
options.threshold_bytes = 1024 * 1024;
players.configure_write(&options)?;

options.mode = WriteMode::Reliable;
players.configure_write(&options)?; // 这个库改为每笔可靠落盘
// players.reset_write_options()?; // 恢复默认性能模式、50 ms / 1 MiB
```

新库默认性能模式：内存写入成功即可返回并读取新值，按周期或字节阈值刷盘，空闲不空刷；断电或崩溃可能丢失尚未落盘的数据。可靠模式等待落盘成功再返回。数据和配置按库独立保存，配置重开后保留；切换可能等待刷盘，建议初始化时设置。同一库打开后复用。

发布执行 cargo build --release，复制 target/release 下的可执行文件和旁边的数据库运行库。已有自动流程时，手动脚本复用 Cargo 记录的输出位置；首次手动生成写到 Generated，正常构建继续使用 OUT_DIR。
