#!/usr/bin/env bash
#
# run.sh — Run the Broker-Grade Trading System
#
# Usage:
#   ./scripts/run.sh                           # Interactive mode (TCP server)
#   ./scripts/run.sh server --port 9000        # Server mode, custom port
#   ./scripts/run.sh bench [bench_opts]        # Run with benchmark
#
# Examples:
#   ./scripts/run.sh                           # TCP server on port 9000
#   ./scripts/run.sh server --port 8080        # Custom port
#   ./scripts/run.sh bench                     # Default benchmark
#   ./scripts/run.sh bench --bench 200000 --symbols 20

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build}"
BINARY="${BINARY:-${BUILD_DIR}/bt_trading}"

# ── Colors ───────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; }

# ── Ensure binary exists ─────────────────────────────────────────────
if [[ ! -x "${BINARY}" ]]; then
    error "Binary not found: ${BINARY}"
    error "Run ./scripts/build.sh first."
    exit 1
fi

# ── Parse mode ───────────────────────────────────────────────────────
MODE="${1:-server}"
shift || true

case "${MODE}" in
    server|srv)
        info "Starting trading server..."
        info "  Binary: ${BINARY}"
        exec "${BINARY}" --no-bench "$@"
        ;;
    bench|b)
        info "Running benchmark..."
        info "  Binary: ${BINARY}"
        exec "${BINARY}" "$@"
        ;;
    *)
        error "Unknown mode: ${MODE}"
        error "Usage: $0 [server|bench] [args...]"
        exit 1
        ;;
esac
