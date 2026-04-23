#!/bin/bash
# Local CI simulation script for testing CI workflow changes
# Runs the same steps as .github/workflows/test.yml but locally in Docker

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXTENSION_DIR="$(dirname "$SCRIPT_DIR")"
VILLAGESQL_SOURCE_DIR="${VILLAGESQL_SOURCE_DIR:-/data/rene/villagesql-server}"
VILLAGESQL_BUILD_DIR="${VILLAGESQL_BUILD_DIR:-/data/rene/build}"
USE_LOCAL_BUILD="${USE_LOCAL_BUILD:-true}"

CONTAINER_NAME="vsql-ci-local-$$"
DOCKER_IMAGE="vsql-ci-validation:latest"

log() {
    echo "[local-ci] $(date '+%H:%M:%S') - $1"
}

cleanup() {
    log "Cleaning up container..."
    docker rm -f "$CONTAINER_NAME" 2>/dev/null || true
}
trap cleanup EXIT

build_docker_image() {
    log "Building Docker image..."
    docker build -t "$DOCKER_IMAGE" "$EXTENSION_DIR/docker/ci-validation" 2>&1 | tail -5
}

run_ci_step() {
    local step_name="$1"
    local cmd="$2"
    log "STEP: $step_name"
    echo "---"
    eval "$cmd"
    echo "---"
}

# Build Docker image if needed
if ! docker image inspect "$DOCKER_IMAGE" &>/dev/null; then
    build_docker_image
fi

# Create test workspace
WORKSPACE_DIR="/tmp/vsql-ci-workspace-$$"
rm -rf "$WORKSPACE_DIR"
mkdir -p "$WORKSPACE_DIR"

log "Starting CI simulation..."
log "VillageSQL source: $VILLAGESQL_SOURCE_DIR"
log "VillageSQL build: $VILLAGESQL_BUILD_DIR"
log "Extension: $EXTENSION_DIR"

# Start container with volume mounts
log "Starting container..."
docker run -d --name "$CONTAINER_NAME" \
    -v "$VILLAGESQL_SOURCE_DIR:/villagesql-source:ro" \
    -v "$VILLAGESQL_BUILD_DIR:/villagesql-build:ro" \
    -v "$EXTENSION_DIR:/extension:ro" \
    -v "$WORKSPACE_DIR:/workspace" \
    -w /workspace \
    "$DOCKER_IMAGE" \
    sleep infinity >/dev/null

# Copy VillageSQL to workspace if not using local build, or symlink
if [ "$USE_LOCAL_BUILD" = "true" ]; then
    log "Using local VillageSQL build (skipping build step)"
    # Symlink build into workspace structure
    docker exec "$CONTAINER_NAME" bash -c "mkdir -p /workspace/villagesql-server && ln -s /villagesql-build /workspace/villagesql-server/build"
else
    log "Copying VillageSQL source..."
    docker exec "$CONTAINER_NAME" bash -c "cp -r /villagesql-source /workspace/villagesql-server"
fi

# Create sibling extension directory
docker exec "$CONTAINER_NAME" bash -c "ln -s /extension /workspace/extension"

# Step 1: Install dependencies (already in Docker image, but run anyway for consistency)
run_ci_step "Install dependencies" '
docker exec '"$CONTAINER_NAME"' bash -c "sudo apt-get update -qq && sudo apt-get install -y -qq cmake gcc make libtirpc-dev libcurl4-openssl-dev netcat-openbsd 2>/dev/null || true"
'

# Step 2: Configure (if not using local build)
if [ "$USE_LOCAL_BUILD" != "true" ]; then
    run_ci_step "Configure VillageSQL" '
    docker exec '"$CONTAINER_NAME"' bash -c "cd /workspace/villagesql-server && mkdir -p build && cd build && cmake .. -DWITH_DEBUG=1 -DWITH_UNIT_TESTS=ON -DWITH_ROUTER=OFF"
    '

    # Step 3: Build
    run_ci_step "Build VillageSQL" '
    docker exec '"$CONTAINER_NAME"' bash -c "cd /workspace/villagesql-server/build && make -j\$(nproc)"
    '
fi

# Step 4: Initialize datadir
run_ci_step "Initialize datadir" '
docker exec '"$CONTAINER_NAME"' bash -c "cd /workspace/villagesql-server && mkdir -p var && ./build/runtime_output_directory/mysqld --initialize-insecure --datadir=var/mysqld.1 --log-error=var/mysqld.log"
'

# Step 5: Start MySQL server
run_ci_step "Start MySQL server" '
docker exec '"$CONTAINER_NAME"' bash -c "cd /workspace/villagesql-server && ./build/runtime_output_directory/mysqld --datadir=var/mysqld.1 --socket=var/mysql.sock --port=3306 --log-error=var/mysqld.log &>/dev/null &" && sleep 2 && docker exec '"$CONTAINER_NAME"' bash -c "for i in {1..30}; do mysql -S /workspace/villagesql-server/var/mysql.sock -u root -e \"SELECT 1\" 2>/dev/null && echo \"MySQL ready\" && break || sleep 1; done"
'

# Step 6: Build extension
run_ci_step "Build extension" '
docker exec '"$CONTAINER_NAME"' bash -c "cd /workspace/extension && mkdir -p build && cd build && cmake .. -DVillageSQL_BUILD_DIR=/workspace/villagesql-server/build && make -j\$(nproc)"
'

# Step 7: Install extension
run_ci_step "Install extension" '
docker exec '"$CONTAINER_NAME"' bash -c "mysql -S /workspace/villagesql-server/var/mysql.sock -u root -e \"INSTALL EXTENSION /workspace/extension/build/prometheus_exporter.veb\""
'

# Step 8: Run MTR tests
log "Running MTR tests..."
TEST_OUTPUT="/tmp/vsql-ci-test-output-$$"
mkdir -p "$TEST_OUTPUT"

run_ci_step "Run MTR tests" '
docker exec '"$CONTAINER_NAME"' bash -c "cd /workspace/villagesql-server/build/mysql-test && perl mysql-test-run.pl --suite=/workspace/extension/mysql-test --parallel=auto --tmpdir=/workspace/villagesql-server/var --socket=/workspace/villagesql-server/var/mysql.sock --report-features 2>&1" | tee "'"$TEST_OUTPUT"'/mtr_output.txt"
'

# Copy test logs
log "Copying test artifacts..."
docker cp "$CONTAINER_NAME:/workspace/villagesql-server/build/mysql-test/var/log" "$TEST_OUTPUT/" 2>/dev/null || true
docker cp "$CONTAINER_NAME:/workspace/villagesql-server/var/mysqld.log" "$TEST_OUTPUT/" 2>/dev/null || true

# Check test results
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