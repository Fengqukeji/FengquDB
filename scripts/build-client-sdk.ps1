param(
    [Parameter(Mandatory=$true)][string]$NativeDir,
    [Parameter(Mandatory=$true)][string]$Codegen,
    [Parameter(Mandatory=$true)][ValidateSet('win-x64','linux-x64')][string]$Platform,
    [string]$BuildDir = 'build/client-release',
    [string]$OutputDir = 'artifacts/client',
    [string]$Languages = 'c,cpp,csharp,java,python,go,rust,node'
)
$ErrorActionPreference = 'Stop'
function Check-Exit { if ($LASTEXITCODE) { throw "Command failed with $LASTEXITCODE" } }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Push-Location $repo
try {
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    $work = (Resolve-Path $BuildDir).Path
    $native = (Resolve-Path $NativeDir).Path
    $version = (& node -p 'process.version').Trim()
    $headers = Join-Path $work "node-$version-headers.tar.gz"
    Invoke-WebRequest "https://nodejs.org/download/release/$version/node-$version-headers.tar.gz" -OutFile $headers
    Invoke-WebRequest "https://nodejs.org/download/release/$version/SHASUMS256.txt" -OutFile "$work/node-checksums.txt"
    $expected = (Get-Content "$work/node-checksums.txt" | Where-Object { $_ -match "\snode-$version-headers.tar.gz$" }) -split '\s+'
    if (-not $expected -or (Get-FileHash $headers -Algorithm SHA256).Hash.ToLower() -ne $expected[0]) { throw 'Node header checksum mismatch' }
    & tar -xzf $headers -C $work
    Check-Exit
    $nodeInclude = "$work/node-$version/include/node"
    $nodeArgs = @('-S','bindings/node','-B',"$work/node",'-DCMAKE_BUILD_TYPE=Release',"-DFQDB_INCLUDE_DIR=$repo/include","-DFQDB_LIB_DIR=$native","-DNODE_INCLUDE_DIR=$nodeInclude",'-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON')
    if ($Platform -eq 'win-x64') {
        Invoke-WebRequest "https://nodejs.org/download/release/$version/win-x64/node.lib" -OutFile "$work/node.lib"
        $expectedLib = (Get-Content "$work/node-checksums.txt" | Where-Object { $_ -match '\swin-x64/node.lib$' }) -split '\s+'
        if (-not $expectedLib -or (Get-FileHash "$work/node.lib" -Algorithm SHA256).Hash.ToLower() -ne $expectedLib[0]) { throw 'Node import library checksum mismatch' }
        $nodeArgs += "-DNODE_LIB=$work/node.lib"
    }
    & cmake @nodeArgs
    Check-Exit
    & cmake --build "$work/node" --config Release --parallel 2
    Check-Exit
    & npm install --prefix "$work/typescript" --ignore-scripts --no-audit --no-fund typescript@5.6.3 '@types/node@22'
    Check-Exit
    & node "$work/typescript/node_modules/typescript/lib/tsc.js" -p bindings/node/tsconfig.json --typeRoots "$work/typescript/node_modules/@types" --outDir "$work/dist"
    Check-Exit
    & python scripts/build-client-assets.py --java-bin "$env:JAVA_HOME/bin" --node-dist "$work/dist/src" --output "$work/modules"
    Check-Exit
    $library = if ($Platform -eq 'win-x64') { "$native/fengqudb.dll" } else { "$native/libfengqudb.so" }
    $jni = if ($Platform -eq 'win-x64') { "$native/fengqudb_jni.dll" } else { "$native/libfengqudb_jni.so" }
    & python scripts/package-client.py --languages $Languages --platform $Platform --library $library --jni $jni --codegen $Codegen --clients "$work/modules" --node-addon "$work/node/fengqudb_native.node" --output $OutputDir
    Check-Exit
} finally { Pop-Location }
