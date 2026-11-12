# 宿主 Windows：在容器里编译 bench_c 与 Rust 基准并依次运行。
# 用法: run-bench.ps1 [秒数=2] [轮数=1]
param([int]$Seconds = 2, [int]$Rounds = 1)
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path -replace '\\', '/'
docker run --rm --entrypoint sh `
    -v "$root/build/lang:/lang" -v "$root/bindings/rust:/src" -v "$root/tools/lang_conformance:/tools:ro" -w /src `
    fqdb-lang-dev:1 /src/scripts/bench.sh $Seconds $Rounds
