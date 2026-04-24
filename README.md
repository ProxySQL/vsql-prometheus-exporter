# vsql-prometheus-exporter

[![CI](https://github.com/ProxySQL/vsql-prometheus-exporter/actions/workflows/test.yml/badge.svg)](https://github.com/ProxySQL/vsql-prometheus-exporter/actions/workflows/test.yml)

Prometheus exporter extension for VillageSQL Server, built using the VillageSQL Extension Framework (VEF).

Exposes MySQL server metrics at an HTTP `/metrics` endpoint in Prometheus exposition format.

## Metrics

| Prefix | Source | Description |
|---|---|---|
| `mysql_global_status_` | `SHOW GLOBAL STATUS` | Server status variables |
| `mysql_global_variables_` | `SHOW GLOBAL VARIABLES` | Numeric server variables |
| `mysql_info_schema_innodb_metrics_` | `information_schema.innodb_metrics` | InnoDB internal metrics |
| `mysql_replica_` | `SHOW REPLICA STATUS` | Replication status (when applicable) |

Built-in operational metrics:

| Metric | Description |
|---|---|
| `requests_total` | Total HTTP scrape requests |
| `scrape_duration_us` | Scrape duration in microseconds |
| `errors_total` | Total scrape errors |

## System Variables

| Variable | Default | Description |
|---|---|---|
| `prometheus_exporter.enabled` | `OFF` | Enable/disable the exporter |
| `prometheus_exporter.port` | `9104` | HTTP listen port |
| `prometheus_exporter.bind_address` | `127.0.0.1` | HTTP listen address |

## CI Testing

Every pull request is tested against VillageSQL Server (tomas/dev branch). The CI workflow:

1. Builds VillageSQL Server from source
2. Builds the extension and packages it as a `.veb` bundle
3. Runs the MTR test suite with 6 tests:

| Test | What it verifies |
|---|---|
| `basic` | Install/uninstall lifecycle, system variables, HTTP listener start/stop |
| `format_validation` | Prometheus exposition format correctness (metric lines, numeric values) |
| `global_variables` | `mysql_global_variables_` metrics exported for known numeric variables |
| `metrics_endpoint` | HTTP 200 on `/metrics`, 404 on unknown paths, all collector prefixes present |
| `replica_status` | `mysql_replica_*` metrics behavior on non-replica server |
| `scrape_counter` | `requests_total` increments per scrape, `scrape_duration_us` > 0, no errors |

## Quick Start

```sql
INSTALL EXTENSION prometheus_exporter;
SET GLOBAL prometheus_exporter.port = 9104;
SET GLOBAL prometheus_exporter.enabled = ON;
```

Then scrape `http://127.0.0.1:9104/metrics`.
