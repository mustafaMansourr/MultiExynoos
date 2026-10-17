#!/usr/bin/env bash
# MultiExynoos layer cross-build (Android NDK, arm64-v8a).
# Usage: ./scripts/build-layer.sh [NDK_PATH]
set -euo pipefail
NDK="${1:-${ANDROID_NDK_HOME:-}}"
[ -n "$NDK" ] || { echo "usage: $0 <ndk_path> (or set ANDROID_NDK_HOME)"; exit 1; }
VERSION="${MX_VERSION:-0.0.11}"
BUILD_DIR="${BUILD_DIR:-build-android}"

cmake -S layer -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release \
  -DMX_VERSION="$VERSION"
cmake --build "$BUILD_DIR" -j "$(nproc)"
echo "Built: $BUILD_DIR/libVkLayer_MultiExynoos.so"
