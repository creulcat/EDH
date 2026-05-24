#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
DIST_DIR="${ROOT}/dist"
SDK_HEADER="${ROOT}/tools/scs_sdk/include/scssdk_telemetry.h"

CONFIG="${1:-Release}"
JOBS="${JOBS:-}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [Release|Debug]

Build Towhitch and copy the plugin to dist/.

Environment:
  JOBS    Parallel build jobs (default: cmake default)
EOF
}

if [[ "$CONFIG" == "-h" || "$CONFIG" == "--help" ]]; then
    usage
    exit 0
fi

if [[ "$CONFIG" != "Release" && "$CONFIG" != "Debug" ]]; then
    echo "Error: configuration must be Release or Debug." >&2
    usage >&2
    exit 1
fi

if [[ ! -f "$SDK_HEADER" ]]; then
    cat >&2 <<EOF
Error: SCS Telemetry SDK not found.

Download the SDK from:
  https://modding.scssoft.com/wiki/Documentation/Engine/SDK/Telemetry

Extract it so this file exists:
  ${SDK_HEADER}
EOF
    exit 1
fi

CMAKE_ARGS=(-S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$CONFIG")

if [[ "$(uname -s)" == "Darwin" ]] && command -v brew >/dev/null 2>&1; then
    BREW_PREFIX="$(brew --prefix)"
    if [[ -d "${BREW_PREFIX}/opt/curl" ]]; then
        CMAKE_ARGS+=(-DCURL_ROOT="${BREW_PREFIX}/opt/curl")
    fi
fi

echo "Configuring Towhitch (${CONFIG})..."
cmake "${CMAKE_ARGS[@]}"

BUILD_ARGS=(--build "$BUILD_DIR" --target Towhitch dist)
if [[ -n "$JOBS" ]]; then
    BUILD_ARGS+=(--parallel "$JOBS")
else
    BUILD_ARGS+=(--parallel)
fi

echo "Building Towhitch..."
cmake "${BUILD_ARGS[@]}"

artifact="$(find "$DIST_DIR" -maxdepth 1 -type f | head -n 1 || true)"
if [[ -n "$artifact" ]]; then
    echo ""
    echo "Build complete:"
    echo "  ${artifact}"
else
    echo "Warning: build finished but no artifact was found in dist/." >&2
fi
