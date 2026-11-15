"""Assemble self-contained Python and C++ foundation SDKs from reviewed trees."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile


ROOT = Path(__file__).resolve().parents[1]


def copy_tree(source, destination):
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))


def check_shared_sources(rag, wiki):
    # Each branch may own a different optional foundation, but must not ship a
    # different Core underneath its public handle/allocator bridge.
    shared = [p.relative_to(ROOT) for p in (ROOT / "src").rglob("*")
              if p.is_file() and "index" not in p.relative_to(ROOT / "src").parts]
    shared += [p.relative_to(ROOT) for p in (ROOT / "include").iterdir()
               if p.is_file() and not p.name.startswith("fengqudb_index")]
    shared += [Path("bindings/python/fengqudb.py")]
    mismatches = []
    for tree in (rag, wiki):
        paths = shared + ([Path("bindings/python") / name for name in
                          ("fengqudb_index.py", "fengqudb_index_wire.py")] if tree == wiki else [])
        for path in paths:
            if not (tree / path).is_file() or (ROOT / path).read_bytes() != (tree / path).read_bytes():
                mismatches.append(str(tree / path))
    if mismatches:
        raise ValueError("Synchronize shared Core/bindings before packaging:\n" + "\n".join(mismatches))


def build(output, rag, wiki, legal_root=None):
    legal_root = legal_root or ROOT
    check_shared_sources(rag, wiki)
    # Refuse stale mixed packages; a caller chooses a fresh output directory.
    output.mkdir(parents=True, exist_ok=False)
    python = output / "python/FengquDB"
    cpp = output / "cpp/FengquDB"
    (python / "bindings/python").mkdir(parents=True)
    (python / "integrations").mkdir()
    for name in ("", "bindings", "bindings/python", "integrations"):
        (python / name / "__init__.py").write_text("", encoding="utf-8")
    for name in ("fengqudb.py", "fengqudb_query.py", "fengqudb_select.py",
                 "fengqudb_index.py", "fengqudb_index_wire.py"):
        shutil.copy2(ROOT / "bindings/python" / name, python / "bindings/python" / name)
    copy_tree(rag / "bindings/python/fengqudb_rag", python / "bindings/python/fengqudb_rag")
    copy_tree(rag / "integrations/wordgame_rag", python / "integrations/wordgame_rag")
    copy_tree(wiki / "llmwiki", python / "llmwiki")
    (python / "__init__.py").write_text('''"""FengquDB native storage, retrieval, RAG and Wiki foundations."""
from .bindings.python.fengqudb import Store, Error, StoreWriteMode
from .bindings.python.fengqudb_index import NativeIndex, QueryControl
from .bindings.python.fengqudb_index_wire import Document, Mutation, Query, Predicate, Phrase, Maintenance, Projection, vector
from .bindings.python.fengqudb_rag import (MemoryStore, MemoryWrite, MemoryImport,
    MemorySearch, MemoryList, MemoryDelete, EmbeddingContract, CallbackEmbedding, QueryBudget)
from .llmwiki import Wiki, WikiError

__all__ = ["Store", "Error", "StoreWriteMode", "NativeIndex", "QueryControl", "Document", "Mutation", "Query",
    "Predicate", "Phrase", "Maintenance", "Projection", "vector", "MemoryStore", "MemoryWrite", "MemoryImport",
    "MemorySearch", "MemoryList", "MemoryDelete", "EmbeddingContract", "CallbackEmbedding", "QueryBudget",
    "Wiki", "WikiError"]
''', encoding="utf-8")
    (cpp / "include").mkdir(parents=True)
    # Only public headers and finished binaries, never build trees or fixtures.
    for source in (ROOT / "include").iterdir():
        if source.suffix in (".h", ".hpp"):
            shutil.copy2(source, cpp / "include" / source.name)
    binaries = {
        "win-x64": {"fengqudb.dll": ROOT / "build/index-win/Release/fengqudb.dll",
                    "fengqudb_index.dll": ROOT / "build/index-win/Release/fengqudb_index.dll"},
        "linux-x64": {"libfengqudb.so.2": ROOT / "build/index-portable/libfengqudb.so.2",
                      "libfengqudb_index.so.1": ROOT / "build/index-portable/libfengqudb_index.so.1"},
    }
    for sdk in (python, cpp):
        for platform, libraries in binaries.items():
            destination = sdk / "native" / platform
            destination.mkdir(parents=True)
            for name, source in libraries.items():
                shutil.copy2(source, destination / name)
    for name in ("fengqudb.lib", "fengqudb_index.lib"):
        shutil.copy2(ROOT / "build/index-win/Release" / name, cpp / "native/win-x64" / name)
    (cpp / "cmake").mkdir()
    (cpp / "cmake/FengquDBConfig.cmake").write_text('''# Relocatable binary-only foundation SDK.
if(TARGET FengquDB::Index)
  return()
endif()
get_filename_component(_fqdb_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|x64)$")
  message(FATAL_ERROR "This SDK contains Windows/Linux x64 binaries")
endif()
add_library(FengquDB::Core SHARED IMPORTED)
add_library(FengquDB::Index SHARED IMPORTED)
if(WIN32)
  set_target_properties(FengquDB::Core PROPERTIES IMPORTED_LOCATION "${_fqdb_root}/native/win-x64/fengqudb.dll" IMPORTED_IMPLIB "${_fqdb_root}/native/win-x64/fengqudb.lib")
  set_target_properties(FengquDB::Index PROPERTIES IMPORTED_LOCATION "${_fqdb_root}/native/win-x64/fengqudb_index.dll" IMPORTED_IMPLIB "${_fqdb_root}/native/win-x64/fengqudb_index.lib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set_target_properties(FengquDB::Core PROPERTIES IMPORTED_LOCATION "${_fqdb_root}/native/linux-x64/libfengqudb.so.2")
  set_target_properties(FengquDB::Index PROPERTIES IMPORTED_LOCATION "${_fqdb_root}/native/linux-x64/libfengqudb_index.so.1")
else()
  message(FATAL_ERROR "This SDK contains Windows/Linux x64 binaries")
endif()
set_target_properties(FengquDB::Core PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_fqdb_root}/include" INTERFACE_COMPILE_FEATURES cxx_std_17)
set_target_properties(FengquDB::Index PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_fqdb_root}/include" INTERFACE_LINK_LIBRARIES FengquDB::Core INTERFACE_COMPILE_FEATURES cxx_std_20)
''', encoding="utf-8")
    for language, target in (("PYTHON", python), ("CPP", cpp)):
        for kind, title in (("SETUP", "接入"), ("API", "API")):
            shutil.copy2(ROOT / "docs" / f"FOUNDATION_{language}_{kind}.md", target / (title + ".md"))
        for name in ("LICENSE", "NOTICE"):
            if (legal_root / name).is_file():
                shutil.copy2(legal_root / name, target / name)
        if (legal_root / "licenses").is_dir():
            copy_tree(legal_root / "licenses", target / "licenses")
        notices = legal_root / "THIRD_PARTY_NOTICES.md"
        if notices.is_file():
            (target / "licenses").mkdir(exist_ok=True)
            text = notices.read_text(encoding="utf-8")
            text = text.replace("(LICENSE)", "(../LICENSE)").replace("(NOTICE)", "(../NOTICE)").replace("(licenses/", "(")
            (target / "licenses/THIRD_PARTY_NOTICES.md").write_text(text, encoding="utf-8")
        json_license = legal_root / "third_party/json/LICENSE.MIT"
        if json_license.is_file():
            (target / "licenses").mkdir(exist_ok=True)
            shutil.copy2(json_license, target / "licenses/nlohmann-json.LICENSE.MIT")
    manifest = {"format": 1, "files": {}}
    for file in sorted(output.rglob("*")):
        if file.is_file():
            manifest["files"][file.relative_to(output).as_posix()] = {
                "bytes": file.stat().st_size, "sha256": hashlib.sha256(file.read_bytes()).hexdigest()}
    manifest["release_id"] = hashlib.sha256(json.dumps(manifest["files"], sort_keys=True).encode()).hexdigest()
    # A consumer copies just one FengquDB folder, so its release identity and
    # hashes must travel inside that folder (and therefore inside its ZIP).
    for sdk in (python, cpp):
        prefix = sdk.relative_to(output).as_posix() + "/"
        sdk_manifest = {"format": 1, "release_id": manifest["release_id"], "files": {
            name[len(prefix):]: value for name, value in manifest["files"].items() if name.startswith(prefix)}}
        (sdk / "manifest.json").write_text(json.dumps(sdk_manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for folder, name in (("python", "Python"), ("cpp", "CPP")):
        with zipfile.ZipFile(output / f"FengquDB-{name}.zip", "w", zipfile.ZIP_DEFLATED) as archive:
            for file in sorted((output / folder).rglob("*")):
                if file.is_file():
                    archive.write(file, file.relative_to(output / folder).as_posix())
    print(json.dumps({"output": str(output), "release_id": manifest["release_id"], "files": len(manifest["files"]),
                      "zip_bytes": {p.name: p.stat().st_size for p in output.glob("*.zip")}}, ensure_ascii=False))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rag", type=Path, required=True)
    parser.add_argument("--wiki", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/foundation")
    parser.add_argument("--legal-root", type=Path, default=ROOT)
    args = parser.parse_args()
    build(args.output.resolve(), args.rag.resolve(), args.wiki.resolve(), args.legal_root.resolve())
