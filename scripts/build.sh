#!/usr/bin/env bash
#
# build.sh — Build the Broker-Grade Trading System
#
# Usage:
#   ./scripts/build.sh              # Release build (default)
#   ./scripts/build.sh debug        # Debug build with ASan/UBSan
#   ./scripts/build.sh clean        # Clean build directory
#   ./scripts/build.sh release      # Explicit release build
#
# Environment variables:
#   BUILD_DIR       Build directory (default: build/)
#   BUILD_JOBS      Parallel jobs  (default: nproc)
#   CMAKE_EXTRA     Extra cmake flags

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
BUILD_TYPE="${1:-release}"

# ── Colors ───────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; }

# ── Clean ────────────────────────────────────────────────────────────
if [[ "${BUILD_TYPE}" == "clean" ]]; then
    info "Cleaning build directory: ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
    info "Clean complete."
    exit 0
fi

# ── Validate build type ──────────────────────────────────────────────
case "${BUILD_TYPE}" in
    release|Release)
        CMAKE_BUILD_TYPE="Release"
        ;;
    debug|Debug)
        CMAKE_BUILD_TYPE="Debug"
        ;;
    *)
        error "Unknown build type: ${BUILD_TYPE}"
        error "Usage: $0 [release|debug|clean]"
        exit 1
        ;;
esac

# ── Create build directory ───────────────────────────────────────────
mkdir -p "${BUILD_DIR}"

# ── CMake configure ──────────────────────────────────────────────────
info "Configuring ${CMAKE_BUILD_TYPE} build..."
info "  Project dir: ${PROJECT_DIR}"
info "  Build dir:   ${BUILD_DIR}"
info "  Source dir:  ${PROJECT_DIR}/src"

cmake -S "${PROJECT_DIR}/src" \
      -B "${BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
      ${CMAKE_EXTRA:-}

# ── Build ────────────────────────────────────────────────────────────
info "Building with ${BUILD_JOBS} parallel jobs..."
cmake --build "${BUILD_DIR}" --parallel "${BUILD_JOBS}"

# ── Verify output ────────────────────────────────────────────────────
BINARY="${BUILD_DIR}/bt_trading"
if [[ -x "${BINARY}" ]]; then
    BIN_SIZE=$(du -h "${BINARY}" | cut -f1)
    info "Build successful!"
    info "  Binary: ${BINARY}"
    info "  Size:   ${BIN_SIZE}"
    info "  Type:   $(file -b "${BINARY}")"
else
    error "Build failed: binary not found at ${BINARY}"
    exit 1
fi
