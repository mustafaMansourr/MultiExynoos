# Packages the Phase-1 layer into the release asset:
#   dist/MultiExynoos-layer-<version>_g<sha>.zip
# Contents (zip root): libVkLayer_MultiExynoos.so, VkLayer_MultiExynoos.json, meta.json
param(
    [string]$Version = "0.1.0",
    [string]$BuildDir = "build-android",
    [string]$OutDir = "dist"
)
$ErrorActionPreference = "Stop"

$so = Join-Path $BuildDir "libVkLayer_MultiExynoos.so"
if (-not (Test-Path $so)) { throw "not found: $so (build first)" }

$sha = (git rev-parse --short HEAD).Trim()
$stage = Join-Path $OutDir "stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item $so $stage
Copy-Item layer/VkLayer_MultiExynoos.json $stage
Copy-Item packaging/meta.json $stage

$zip = Join-Path $OutDir "MultiExynoos-layer-${Version}_g$sha.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage/*" -DestinationPath $zip
Remove-Item -Recurse -Force $stage

Write-Host "Asset: $zip"
