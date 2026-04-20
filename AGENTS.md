# AGENTS.md

This file provides guidance to AI coding assistants when working with code in this repository.

**Note**: Also check `AGENTS.local.md` for additional local development instructions when present.

## Project Overview

This is a Prometheus metrics exporter extension for VillageSQL. It exposes a minimal HTTP server on a configurable port that serves Prometheus-format metrics at `GET /metrics`.

Metrics are collected at scrape time via `villagesql::run_query()` and cover:
- `SHOW GLOBAL STATUS` (numeric values only)
- `SHOW GLOBAL VARIABLES` (numeric values only)
- `INFORMATION_SCHEMA.INNODB_METRICS` (enabled metrics)
- `SHOW REPLICA STATUS` (IO/SQL running state)

## Build System

**Configure and Build:**
```bash
mkdir build && cd build
cmake .. -DVillageSQL_BUILD_DIR=/path/to/villagesql/build
make
```

The build produces `prometheus_exporter.veb` in the build directory.

**Requirements:**
- VillageSQL build tree (specified via `VillageSQL_BUILD_DIR`)
- C++17 compiler
- Linux (uses `pipe(2)` + `poll(2)` for the HTTP listener shutdown path)

**Dev headers note:**
This extension uses pre-release ABI features (`InstallResult`, `run_query`,
`background_thread`, `on_change`). `CMakeLists.txt` sets
`VillageSQL_USE_DEV_HEADERS=ON` automatically, which causes `FindVillageSQL.cmake`
to use the `include-dev/` tree from the SDK instead of the stable `include/`.

**Install (copies VEB to the server's veb_output_directory):**
```bash
make install
```

**CMake Variables:**
- `VillageSQL_BUILD_DIR`: Path to VillageSQL build directory (required)

## Testing

Run `make install` first, then point MTR at the test suite:

```bash
cd $HOME/githome/villagesql-server/build-debug
./mysql-test/mysql-test-run.pl \
  --suite=$HOME/githome/vsql-prometheus-exporter/mysql-test \
  basic
```

To record the result file after changes:

```bash
./mysql-test/mysql-test-run.pl \
  --suite=$HOME/githome/vsql-prometheus-exporter/mysql-test \
  --record basic
```

## Architecture

**Core Components:**
- `src/extension.cc` — all extension logic and registration

**Extension Registration:**
```sql
INSTALL EXTENSION 'prometheus_exporter';
```

**System Variables:**
- `prometheus_exporter.enabled` — BOOL, default OFF. Set ON to start the listener.
- `prometheus_exporter.port` — INT, default 9104. TCP port to listen on.
- `prometheus_exporter.bind_address` — STRING, default "127.0.0.1".

The listener starts when `enabled` is set to ON (either at install time or via
`SET GLOBAL prometheus_exporter.enabled = ON` afterwards).

**Lifecycle:**
- `on_install`: starts the HTTP listener if `enabled = ON`
- `on_uninstall`: stops the listener (signals wakeup pipe, joins thread)
- `on_sys_var_change("enabled")`: dynamically starts or stops the listener

**Background Thread:**
The HTTP listener runs as a `pthread` registered with MySQL's process list via
`villagesql::background_thread::register_background_thread()`. It appears in
`INFORMATION_SCHEMA.PROCESSLIST` as `prometheus_exporter/listener`.
