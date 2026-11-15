#!/bin/sh
# Producer-side Apple build. Runs on a macOS machine/hosted macOS CI runner.
# No Apple SDK or engine source is shipped to database users.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo"
if [ "$(uname -s)" != Darwin ]; then
  printf '%s\n' 'This producer build needs macOS/Xcode; a hosted macOS CI runner is sufficient.' >&2
  exit 1
fi
output=${1:-artifacts/apple-sdk}
work=${2:-build/apple-sdk}
mkdir -p "$output" "$work"
for arch in arm64 x86_64; do
  cmake -S . -B "$work/macos-$arch" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
    -DFQDB_SHARED=ON -DFQDB_RELEASE_IPO=OFF \
    -DBUILD_TESTING=OFF -DFQDB_BUILD_TOOLS=OFF -DFQDB_BUILD_DIAGNOSTICS=OFF
  cmake --build "$work/macos-$arch" --target fengqudb --parallel 2
  mkdir -p "$output/macos/$arch"
  cp "$work/macos-$arch/libfengqudb.dylib" "$output/macos/$arch/libfengqudb.dylib"
  xcrun --sdk macosx clang++ -arch "$arch" -mmacosx-version-min=11.0 \
    -std=c++17 -Iinclude tests/apple/link_probe.cpp \
    "$output/macos/$arch/libfengqudb.dylib" -Wl,-rpath,@executable_path \
    -o "$work/macos-$arch/link-probe"
  if [ "$(uname -m)" = "$arch" ]; then
    (cd "$work/macos-$arch" && ./link-probe)
  fi
  cmake -S tools/codegen -B "$work/codegen-$arch" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
  cmake --build "$work/codegen-$arch" --parallel 2
  mkdir -p "$output/Tools/$arch"
  cp "$work/codegen-$arch/fqdb-codegen" "$output/Tools/$arch/fqdb-codegen"
done
for sdk in iphoneos iphonesimulator; do
  cmake -S . -B "$work/$sdk" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$sdk" -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
    -DFQDB_SHARED=OFF -DFQDB_RELEASE_IPO=OFF -DBUILD_TESTING=OFF \
    -DFQDB_BUILD_TOOLS=OFF -DFQDB_BUILD_DIAGNOSTICS=OFF
  cmake --build "$work/$sdk" --target fengqudb --parallel 2
  mkdir -p "$output/$sdk/arm64"
  cp "$work/$sdk/libfengqudb.a" "$output/$sdk/arm64/libfengqudb.a"
  target=arm64-apple-ios13.0
  if [ "$sdk" = iphonesimulator ]; then target=$target-simulator; fi
  xcrun --sdk "$sdk" clang++ -target "$target" -isysroot "$(xcrun --sdk "$sdk" --show-sdk-path)" \
    -std=c++17 -DFQDB_STATIC -Iinclude tests/apple/link_probe.cpp \
    "$output/$sdk/arm64/libfengqudb.a" -o "$work/$sdk/link-probe"
done
dotnet build bindings/csharp/FengquDB.csproj -c Release -p:DefineConstants=UNITY_IOS \
  -o "$output/managed-ios" --nologo
cp LICENSE NOTICE THIRD_PARTY_NOTICES.md "$output/"
cp -R licenses "$output/"
printf '%s\n' 'PASS: Apple libraries and both macOS codegen tools compiled; native-host probe ran; iOS device/simulator C ABI linked; iOS managed variant built.'
