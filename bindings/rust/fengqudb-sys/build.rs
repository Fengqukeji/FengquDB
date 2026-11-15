//! 链接 libfengqudb。
//!
//! 库目录来自环境变量 `FQDB_LIB_DIR`；未设置时依次尝试仓库内的
//! `build/lang/linux/lib`、`build/lib`（相对本 crate 的上三级目录）以及系统默认搜索路径。

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=FQDB_LIB_DIR");
    println!("cargo:rerun-if-env-changed=FQDB_STATIC");

    let mut dirs: Vec<PathBuf> = Vec::new();
    if let Some(dir) = env::var_os("FQDB_LIB_DIR") {
        dirs.push(PathBuf::from(dir));
    } else {
        let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap_or_default());
        // A distributed client lives at SDK/rust/fengqudb-sys.
        if let Some(sdk) = manifest.ancestors().nth(2) {
            if sdk.join("Native").is_dir() {
                dirs.push(sdk.join("Native"));
            }
        }
        // bindings/rust/fengqudb-sys -> 仓库根目录
        if let Some(root) = manifest.ancestors().nth(3) {
            for rel in ["build/lang/linux/lib", "build/lib"] {
                let candidate = root.join(rel);
                if candidate.is_dir() {
                    dirs.push(candidate);
                }
            }
        }
    }
    for dir in &dirs {
        println!("cargo:rustc-link-search=native={}", dir.display());
    }
    let kind = if env::var_os("FQDB_STATIC").is_some() { "static" } else { "dylib" };
    println!("cargo:rustc-link-lib={kind}=fengqudb");
}
