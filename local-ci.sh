#!/bin/bash
# Local CI simulation - mirrors what the GitHub Actions workflow should do
# Key insight: MTR manages its own MySQL server. Do NOT start one manually.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXTENSION_DIR="$SCRIPT_DIR"
VILLAGESQL_BUILD_DIR="${VILLAGESQL_BUILD_DIR:-/data/rene/build}"
VEB_DIR="/tmp/vsql-ci-veb-$$"

log() { echo "[local-ci] $(date '+%H:%M:%S') - $1"; }

cleanup() {
    log "Cleaning up..."
    rm -rf "$VEB_DIR" 2>/dev/null || true
}
trap cleanup EXIT

# Step 1: Build extension
log "Building extension..."
cd "$EXTENSION_DIR"
rm -rf build
mkdir -p build && cd build
cmake .. -DVillageSQL_BUILD_DIR="$VILLAGESQL_BUILD_DIR" 2>&1 | tail -5
make -j"$(nproc)" 2>&1 | tail -5

if [ ! -f prometheus_exporter.veb ]; then
    log "ERROR: prometheus_exporter.veb not found after build"
    exit 1
fi
log "Extension built successfully"

# Step 2: Set up VEB directory
mkdir -p "$VEB_DIR"
cp prometheus_exporter.veb "$VEB_DIR/"
log "VEB placed in $VEB_DIR"

# Step 3: Run MTR - let it manage its own server
# --mysqld=--veb-dir=... tells MTR to pass that flag to the server it starts
log "Running MTR tests..."
cd "$VILLAGESQL_BUILD_DIR"

perl ./mysql-test/mysql-test-run.pl \
    --do-suite=villagesql/prometheus_exporter \
    --parallel=1 \
    --nounit-tests \
    --mysqld=--veb-dir="$VEB_DIR"

log "MTR finished with exit code: $?"
