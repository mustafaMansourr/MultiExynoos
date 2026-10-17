# No-cmake fallback build: drives the NDK clang++ directly.
# Use when CMake/Ninja are unavailable (the CI uses the CMake path).
# Usage: ./scripts/build-layer-direct.ps1 -Ndk <ndk_path> -Version 0.0.1
param(
    [Parameter(Mandatory = $true)][string]$Ndk,
    [string]$Version = "0.0.1",
    [string]$OutDir = "build-android",
    [string]$Headers = "external/Vulkan-Headers"
)
$ErrorActionPreference = "Stop"

$toolchain = Join-Path $Ndk "toolchains/llvm/prebuilt/windows-x86_64/bin"
$clang = Join-Path $toolchain "clang++.cmd"
if (-not (Test-Path $clang)) { throw "clang++ not found: $clang" }
if (-not (Test-Path (Join-Path $Headers "include/vulkan/vulkan.h"))) {
    throw "Vulkan headers not found at $Headers — run: git clone --depth 1 --branch v1.3.296 https://github.com/KhronosGroup/Vulkan-Headers.git external/Vulkan-Headers"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$out = Join-Path $OutDir "libVkLayer_MultiExynoos.so"

& $clang `
    --target=aarch64-linux-android26 `
    --sysroot="$Ndk/toolchains/llvm/prebuilt/windows-x86_64/sysroot" `
    -std=c++17 -O2 -fPIC -shared `
    -Wall -Wextra -Werror `
    "-DMX_VERSION_STR=`"$Version`"" `
    -I (Join-Path $Headers "include") `
    layer/src/layer.cpp `
    -llog `
    -Wl,--exclude-libs,ALL -Wl,--build-id=sha1 `
    -o $out
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

Write-Host "Built: $out"
