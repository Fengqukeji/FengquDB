# Windows host: build bench_c and the Rust benchmark inside the container and run both.
# Usage: run-bench.ps1 [seconds=2] [rounds=1]
# Keep this file ASCII-only (see run-tests.ps1).
param([int]$Seconds = 2, [int]$Rounds = 1)
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path -replace '\\', '/'
docker run --rm --entrypoint sh `
    -v "$root/build/lang:/lang" -v "$root/bindings/rust:/src" -v "$root/tools/lang_conformance:/tools:ro" -w /src `
    fqdb-lang-dev:1 /src/scripts/bench.sh $Seconds $Rounds
exit $LASTEXITCODE
