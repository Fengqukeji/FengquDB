# 宿主 Windows：在 fqdb-lang-dev:1 容器里运行 cargo test + clippy。
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path -replace '\\', '/'
docker run --rm --entrypoint sh `
    -v "$root/build/lang:/lang" -v "$root/bindings/rust:/src" -w /src `
    fqdb-lang-dev:1 /src/scripts/test.sh @args
