#!/usr/bin/env bash
# Build the altair-cpm-mcp host tool used by the MCP build server.
# Output binary: mcp_app_build_server/build/altair-cpm-mcp

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$REPO_ROOT/mcp_app_build_server"
BUILD_DIR="$SRC_DIR/build"

CLEAN=0
BUILD_TYPE="Release"

for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        --debug) BUILD_TYPE="Debug" ;;
        -h|--help)
            cat <<EOF
Usage: $(basename "$0") [--clean] [--debug]

  --clean   Remove the build directory before configuring.
  --debug   Configure a Debug build (default: Release).
EOF
            exit 0
            ;;
        *)
            echo "Unknown option: $arg" >&2
            exit 1
            ;;
    esac
done

if [[ $CLEAN -eq 1 && -d "$BUILD_DIR" ]]; then
    echo "Removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

echo "Configuring ($BUILD_TYPE)..."
cmake -S "$SRC_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

echo "Building..."
cmake --build "$BUILD_DIR" -j

BIN="$BUILD_DIR/altair-cpm-mcp"
if [[ -x "$BIN" ]]; then
    echo
    echo "Built: $BIN"
else
    echo "Build finished but binary not found at $BIN" >&2
    exit 1
fi
