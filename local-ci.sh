#!/bin/bash
# Local CI simulation - runs on host directly
# Mirrors .github/workflows/test.yml steps locally for fast iteration

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXTENSION_DIR="$SCRIPT_DIR"
VILLAGESQL_SOURCE_DIR="${VILLAGESQL_SOURCE_DIR:-/data/rene/villagesql-server}"
VILLAGESQL_BUILD_DIR="${VILLAGESQL_BUILD_DIR:-/data/rene/build}"
USE_LOCAL_BUILD="${USE_LOCAL_BUILD:-true}"

WORKSPACE="/tmp/vsql-ci-local-$$"
VAR_DIR="$WORKSPACE/var"

log() {
    echo "[local-ci] $(date '+%H:%M:%S') - $1"
}

cleanup() {
    log "Cleaning up..."
    pkill -f "mysqld.*vsql-ci.*" 2>/dev/null || true
    rm -rf "$WORKSPACE" 2>/dev/null || true
}
trap cleanup EXIT

mkdir -p "$WORKSPACE"
mkdir -p "$VAR_DIR"

export LD_LIBRARY_PATH="$VILLAGESQL_BUILD_DIR/library_output_directory:$VILLAGESQL_BUILD_DIR/plugin_output_directory:$LD_LIBRARY_PATH"

log "Starting local CI simulation..."
log "VillageSQL source: $VILLAGESQL_SOURCE_DIR"
log "VillageSQL build: $VILLAGESQL_BUILD_DIR"
log "Extension: $EXTENSION_DIR"

log "Initializing datadir..."
cd "$VILLAGESQL_SOURCE_DIR"
"$VILLAGESQL_BUILD_DIR/runtime_output_directory/mysqld" \
    --initialize-insecure \
    --datadir="$VAR_DIR/mysqld.1" \
    --log-error="$VAR_DIR/mysqld.log" 2>&1 | tail -5

log "Starting MySQL..."
"$VILLAGESQL_BUILD_DIR/runtime_output_directory/mysqld" \
    --datadir="$VAR_DIR/mysqld.1" \
    --socket="$VAR_DIR/mysql.sock" \
    --port=3306 \
    --log-error="$VAR_DIR/mysqld.log" &>/dev/null &

log "Waiting for MySQL to start..."
for i in {1..30}; do
    if "$VILLAGESQL_BUILD_DIR/runtime_output_directory/mysql" -S "$VAR_DIR/mysql.sock" -u root -e "SELECT 1" 2>/dev/null; then
        log "MySQL is ready"
        break
    fi
    sleep 1
done

log "Building extension..."
cd "$EXTENSION_DIR"
rm -rf build
mkdir -p build
cd build
cmake .. -DVillageSQL_BUILD_DIR="$VILLAGESQL_BUILD_DIR" 2>&1 | tail -3
make -j$(nproc) 2>&1 | tail -3

log "Installing extension..."
"$VILLAGESQL_BUILD_DIR/runtime_output_directory/mysql" -S "$VAR_DIR/mysql.sock" -u root \
    -e "INSTALL EXTENSION $EXTENSION_DIR/build/prometheus_exporter.veb" 2>&1

log "Running MTR tests..."
cd "$VILLAGESQL_BUILD_DIR/mysql-test"
TEST_OUTPUT="/tmp/vsql-ci-test-output-$$"
mkdir -p "$TEST_OUTPUT"

perl mysql-test-run.pl \
    --suite="$EXTENSION_DIR/mysql-test" \
    --parallel=auto \
    --tmpdir="$VAR_DIR" \
    --socket="$VAR_DIR/mysql.sock" \
    --report-features 2>&1 | tee "$TEST_OUTPUT/mtr_output.txt"

cp "$VAR_DIR/mysqld.log" "$TEST_OUTPUT/" 2>/dev/null || true

PASSED=$(grep -c "mysql-test-run: .* \[ pass \]" "$TEST_OUTPUT/mtr_output.txt" 2>/dev/null || echo "0")
FAILED=$(grep -c "mysql-test-run: .* \[ fail \]" "$TEST_OUTPUT/mtr_output.txt" 2>/dev/null || echo "0")

log "============================================"
log "TEST RESULTS: $PASSED passed, $FAILED failed"
log "Full output: $TEST_OUTPUT/mtr_output.txt"
log "============================================"

if [ "$FAILED" -gt "0" ]; then
    log "TESTS FAILED - CI workflow needs fixes"
    exit 1
else
    log "ALL TESTS PASSED - CI workflow is valid"
    exit 0
fi