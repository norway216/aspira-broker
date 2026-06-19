#!/usr/bin/env bash
#
# bench.sh — Benchmark the Broker-Grade Trading System
#
# Runs a series of benchmarks with increasing load to measure throughput
# and latency characteristics.
#
# Usage:
#   ./scripts/bench.sh              # Default benchmark suite
#   ./scripts/bench.sh quick        # Quick smoke test
#   ./scripts/bench.sh full         # Full benchmark suite
#   ./scripts/bench.sh custom [opts]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build}"
BINARY="${BINARY:-${BUILD_DIR}/bt_trading}"

# ── Colors ───────────────────────────────────────────────────────────
GREEN='\033[0;32m'; CYAN='\033[0;36m'; YELLOW='\033[1;33m'; NC='\033[0m'
BOLD='\033[1m'

info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
header(){ echo -e "\n${CYAN}${BOLD}═══ $* ═══${NC}\n"; }
result(){ echo -e "${YELLOW}$*${NC}"; }

# ── Ensure binary exists ─────────────────────────────────────────────
if [[ ! -x "${BINARY}" ]]; then
    echo "ERROR: Binary not found: ${BINARY}"
    echo "Run ./scripts/build.sh first."
    exit 1
fi

# ── Run single benchmark ─────────────────────────────────────────────
run_bench() {
    local orders="$1"
    local symbols="$2"
    local threads="${3:-4}"
    local timeout_sec="${4:-30}"

    header "Benchmark: ${orders} orders × ${symbols} symbols × ${threads} matchers"

    timeout "${timeout_sec}" "${BINARY}" \
        --bench "${orders}" \
        --symbols "${symbols}" \
        --matching-threads "${threads}" 2>&1 || true
}

# ── Modes ────────────────────────────────────────────────────────────
MODE="${1:-default}"
shift || true

case "${MODE}" in
    quick|smoke)
        header "Quick Smoke Test"
        run_bench 10000 5 2 10
        ;;
    default)
        header "Default Benchmark Suite"
        run_bench  50000  10 2 15
        run_bench 100000  10 4 20
        ;;
    full|stress)
        header "Full Benchmark Suite"
        run_bench   50000  10 2 15
        run_bench  100000  20 4 20
        run_bench  200000  30 4 25
        run_bench  500000  50 8 40
        ;;
    custom)
        header "Custom Benchmark"
        exec "${BINARY}" "$@"
        ;;
    *)
        echo "Usage: $0 [quick|default|full|custom] [opts]"
        echo ""
        echo "  quick    - Smoke test (10k orders)"
        echo "  default  - Standard suite (50k + 100k)"
        echo "  full     - Stress test (50k → 500k)"
        echo "  custom   - Pass args directly to bt_trading"
        exit 1
        ;;
esac

header "Benchmark Complete"
