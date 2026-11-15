// Merge into an existing build.rs. FQDB_CODEGEN points to the host binary.
use std::{env, path::PathBuf, process::Command};

fn has_entities(directory: &std::path::Path) -> bool {
    std::fs::read_dir(directory).expect("read Rust source directory").any(|entry| {
        let entry = entry.expect("read source entry");
        let kind = entry.file_type().expect("read source file type");
        if kind.is_dir() { has_entities(&entry.path()) }
        else if kind.is_file() && entry.path().extension().is_some_and(|e| e == "rs") {
            std::fs::read_to_string(entry.path()).expect("read Rust source").contains("fqdb:entity")
        } else { false }
    })
}

fn deploy_runtime(root: &std::path::Path, out: &std::path::Path) {
    println!("cargo:rerun-if-env-changed=FQDB_LIB_DIR");
    println!("cargo:rerun-if-env-changed=FQDB_STATIC");
    if env::var_os("FQDB_STATIC").is_some() { return; }
    let native = env::var_os("FQDB_LIB_DIR").map(PathBuf::from)
        .unwrap_or_else(|| root.join("FengquDB/Native"));
    let target = env::var("CARGO_CFG_TARGET_OS").unwrap();
    let file = match target.as_str() {
        "windows" => "fengqudb.dll", "macos" => "libfengqudb.2.dylib",
        "linux" => "libfengqudb.so.2", _ => return,
    };
    let source = native.join(file);
    // OUT_DIR is <target>/<profile>/build/<package>/out, also with CARGO_TARGET_DIR.
    let profile = out.ancestors().nth(3).expect("Cargo profile directory");
    let bytes = std::fs::read(&source).expect("read SDK native runtime");
    for destination in [profile.to_path_buf(), profile.join("deps"), profile.join("examples")] {
        std::fs::create_dir_all(&destination).expect("create runtime directory");
        let destination = destination.join(file);
        // Avoid rewriting a loaded Windows DLL during incremental builds.
        if std::fs::read(&destination).ok().as_deref() != Some(bytes.as_slice()) {
            std::fs::write(destination, &bytes).expect("deploy SDK native runtime");
        }
    }
    println!("cargo:rerun-if-changed={}", source.display());
    if target == "linux" { println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN"); }
    if target == "macos" { println!("cargo:rustc-link-arg=-Wl,-rpath,@loader_path"); }
}

fn main() {
    let root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let cargo_out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    deploy_runtime(&root, &cargo_out);
    let out = cargo_out.join("fengqudb");
    println!("cargo:rerun-if-changed=src");
    println!("cargo:rustc-env=FQDB_GENERATED_DIR={}", out.display());
    if !has_entities(&root.join("src")) { return; }
    let executable = if cfg!(windows) { "fqdb-codegen.exe" } else { "fqdb-codegen" };
    let bundled = root.join("FengquDB").join("Tools").join(executable);
    let local = if bundled.is_file() { bundled } else { root.join(executable) };
    let compiler = env::var_os("FQDB_CODEGEN").unwrap_or_else(|| {
        if local.is_file() { local.into_os_string() } else { "fqdb-codegen".into() }
    });
    if PathBuf::from(&compiler).is_file() {
        println!("cargo:rerun-if-changed={}", PathBuf::from(&compiler).display());
    }
    let status = Command::new(&compiler).arg("generate").arg("--source").arg(&root)
        .args(["--source-language", "rust", "--output"]).arg(&out)
        .arg("--schema-lock").arg(root.join("FengquDB.schema.lock.json"))
        .status().expect("start the host fqdb-codegen");
    assert!(status.success(), "FengquDB source generation failed");
    println!("cargo:rerun-if-changed=FengquDB.schema.lock.json");
    println!("cargo:rerun-if-env-changed=FQDB_CODEGEN");
}
