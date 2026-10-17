param(
    [string]$Ndk = $env:ANDROID_NDK_HOME,
    [string]$BuildDir = "build-android",
    [string]$Version = "0.0.1",
    [string]$Generator = ""
)
$ErrorActionPreference = "Stop"
if (-not $Ndk) { throw "ANDROID_NDK_HOME not set and -Ndk not passed" }

$common = @(
    "-S", "layer", "-B", $BuildDir,
    "-DCMAKE_TOOLCHAIN_FILE=$Ndk/build/cmake/android.toolchain.cmake",
    "-DANDROID_ABI=arm64-v8a",
    "-DANDROID_PLATFORM=android-26",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DMX_VERSION=$Version"
)
if ($Generator) { cmake -G $Generator @common } else { cmake @common }
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

cmake --build $BuildDir -j 8
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

Write-Host "Built: $BuildDir/libVkLayer_MultiExynoos.so"
