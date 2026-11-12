# Windows host: run cargo test + clippy inside the fqdb-lang-dev:1 container.
# Keep this file ASCII-only: Windows PowerShell 5.1 reads BOM-less UTF-8 as ANSI, and a
# multi-byte character at the end of a comment can swallow the line break.
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path -replace '\\', '/'
docker run --rm --entrypoint sh `
    -v "$root/build/lang:/lang" -v "$root/bindings/rust:/src" -w /src `
    fqdb-lang-dev:1 /src/scripts/test.sh @args
exit $LASTEXITCODE
