# No-cmake fallback build: drives the NDK's target clang++ directly.
# Use when CMake/Ninja are unavailable (CI uses the CMake path).
# Usage: ./scripts/build-layer-direct.ps1 -Ndk <ndk_path> -Version 0.0.11
param(
    [Parameter(Mandatory = $true)][string]$Ndk,
    [string]$Version = "0.0.11",
    [string]$OutDir = "build-android",
    [string]$Headers = "external/Vulkan-Headers",
    [string]$Api = "26"
)
$ErrorActionPreference = "Stop"

$bin = Join-Path $Ndk "toolchains/llvm/prebuilt/windows-x86_64/bin"
$clang = Join-Path $bin "aarch64-linux-android$Api-clang++.cmd"
if (-not (Test-Path $clang)) {
    # Older NDKs named the plain driver clang++.cmd; fall back to it.
    $clang = Join-Path $bin "clang++.cmd"
    if (-not (Test-Path $clang)) { $clang = Join-Path $bin "clang++.exe" }
}
if (-not (Test-Path $clang)) { throw "clang++ not found under $bin" }
if (-not (Test-Path (Join-Path $Headers "include/vulkan/vk_layer.h"))) {
    throw "Vulkan headers not found at $Headers - run: git clone --depth 1 --branch v1.3.296 https://github.com/KhronosGroup/Vulkan-Headers.git external/Vulkan-Headers"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$out = Join-Path $OutDir "libVkLayer_MultiExynoos.so"

# Pass the version via a generated header: PowerShell's native-argument quoting
# strips the quotes around -D values, which would yield an invalid macro token.
$verHdr = Join-Path $OutDir "mx_version.h"
@(
    "#pragma once",
    "#define MX_VERSION_STR `"$Version`""
) | Set-Content -Path $verHdr -Encoding ascii

& $clang `
    -std=c++17 -O2 -fPIC -shared `
    -Wall -Wextra -Werror `
    "-include" `
    $verHdr `
    "-I$(Join-Path $Headers 'include')" `
    layer/src/layer.cpp `
    -static-libstdc++ `
    -llog `
    "-Wl,--exclude-libs,ALL" `
    -o $out
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }

Write-Host "Built: $out"
