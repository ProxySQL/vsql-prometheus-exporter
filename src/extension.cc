// VillageSQL Prometheus Exporter Extension
//
// Exposes an HTTP server on a configurable port that serves
// Prometheus text exposition format metrics at GET /metrics.
//
// System variables:
//   prometheus_exporter_enabled      BOOL   (default: OFF)
//   prometheus_exporter_port        INT    (default: 9104)
//   prometheus_exporter_bind_address STRING (default: "127.0.0.1")
//
// Status variables (SHOW GLOBAL STATUS LIKE 'prometheus_exporter%'):
//   prometheus_exporter_requests_total
//   prometheus_exporter_errors_total
//   prometheus_exporter_scrape_duration_microseconds
//
// Metrics emitted:
//   mysql_global_status_*  - SHOW GLOBAL STATUS (gauge/untyped)
//   mysql_global_variables_* - SHOW GLOBAL VARIABLES (gauge)
//   mysql_innodb_metrics_*  - INFORMATION_SCHEMA.INNODB_METRICS (counter/gauge)
//   mysql_replica_*         - SHOW REPLICA STATUS (gauge, channel label)
//   mysql_binlog_*          - SHOW BINARY LOGS (gauge)
//
// The HTTP listener runs in a background thread registered with MySQL's
// process list. Metrics are collected via run_query() at scrape time.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <villagesql/vsql.h>

using namespace vsql;

// =============================================================================
// Config variables
// =============================================================================

static bool g_enabled = false;
static long long g_port = 9104;
static char *g_bind_address = nullptr;

// =============================================================================
// Status variables
// =============================================================================

static long long g_requests_total = 0;
static long long g_errors_total = 0;
static long long g_scrape_duration_us = 0;

// =============================================================================
// Runtime state
// =============================================================================

struct PrometheusContext {
  pthread_t listener_thread{};
  int listen_fd{-1};
  int wakeup_pipe_r{-1};
  int wakeup_pipe_w{-1};
  std::atomic<bool> shutdown_requested{false};
};

static PrometheusContext *g_ctx = nullptr;

// =============================================================================
// Gauge variables for global status (values that can go up AND down)
// =============================================================================

static const char *gauge_variables[] = {
    "Threads_connected",     "Threads_running",        "Threads_cached",
    "Threads_created",       "Open_tables",             "Open_files",
    "Open_streams",          "Open_table_definitions", "Opened_tables",
    "Innodb_buffer_pool_pages_data",   "Innodb_buffer_pool_pages_dirty",
    "Innodb_buffer_pool_pages_free",    "Innodb_buffer_pool_pages_misc",
    "Innodb_buffer_pool_pages_total",   "Innodb_buffer_pool_bytes_data",
    "Innodb_buffer_pool_bytes_dirty",  "Innodb_page_size",
    "Innodb_data_pending_reads",        "Innodb_data_pending_writes",
    "Innodb_data_pending_fsyncs",      "Innodb_os_log_pending_writes",
    "Innodb_os_log_pending_fsyncs",    "Innodb_row_lock_current_waits",
    "Key_blocks_unused",     "Key_blocks_used",         "Key_blocks_not_flushed",
    "Max_used_connections",  "Uptime",                  "Uptime_since_flush_status",
    nullptr};

static bool is_gauge(const std::string &name) {
  for (const char **p = gauge_variables; *p != nullptr; ++p) {
    if (strcasecmp(name.c_str(), *p) == 0) return true;
  }
  return false;
}

// =============================================================================
// Helper: sanitize MySQL name to Prometheus metric suffix
// =============================================================================

static void append_sanitized_name(std::string &out, const std::string &name) {
  for (char c : name) {
    char lc = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    out += (lc >= 'a' && lc <= 'z') || (lc >= '0' && lc <= '9') ? lc : '_';
  }
}

// =============================================================================
// Helper: check if string is valid Prometheus numeric value
// =============================================================================

static bool is_numeric(const std::string &s) {
  if (s.empty()) return false;
  char *end = nullptr;
  strtod(s.c_str(), &end);
  return end != nullptr && *end == '\0';
}

// =============================================================================
// Helper: escape special characters in label values
// =============================================================================

static void append_escaped_value(std::string &out, const std::string &value) {
  for (char ch : value) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      default: out += ch; break;
    }
  }
}

// =============================================================================
// Metric collection: global status
// =============================================================================

static void collect_global_status(std::string &out, bool &error) {
  char error_msg[VEF_MAX_ERROR_LEN] = {};
  bool first = true;

  auto type_fn = [](const std::string &name) {
    return is_gauge(name) ? "gauge" : "untyped";
  };

  villagesql::run_query(
      "SHOW GLOBAL STATUS",
      nullptr,
      [&](const std::vector<std::string_view> &row) {
        if (row.size() < 2 || row[0].data() == nullptr ||
            row[1].data() == nullptr)
          return;
        std::string name(row[0]);
        std::string value(row[1]);
        if (!is_numeric(value)) return;

        std::string metric_name = "mysql_global_status_";
        append_sanitized_name(metric_name, name);

        if (first) {
          out += "# TYPE mysql_global_status_ gauge\n";
          out += "# TYPE mysql_global_status_ untyped\n";
          first = false;
        }

        const char *type_str = type_fn(name);
        out += metric_name;
        out += ' ';
        out += value;
        out += '\n';
      },
      error_msg);
  if (error_msg[0] != '\0') error = true;
}

// =============================================================================
// Metric collection: global variables
// =============================================================================

static void collect_global_variables(std::string &out, bool &error) {
  char error_msg[VEF_MAX_ERROR_LEN] = {};

  villagesql::run_query(
      "SHOW GLOBAL VARIABLES",
      nullptr,
      [&](const std::vector<std::string_view> &row) {
        if (row.size() < 2 || row[0].data() == nullptr ||
            row[1].data() == nullptr)
          return;
        std::string name(row[0]);
        std::string value(row[1]);
        if (!is_numeric(value)) return;

        std::string metric_name = "mysql_global_variables_";
        append_sanitized_name(metric_name, name);

        out += "# TYPE ";
        out += metric_name;
        out += " gauge\n";
        out += metric_name;
        out += ' ';
        out += value;
        out += '\n';
      },
      error_msg);
  if (error_msg[0] != '\0') error = true;
}

// =============================================================================
// Metric collection: InnoDB metrics
// =============================================================================

static void collect_innodb_metrics(std::string &out, bool &error) {
  char error_msg[VEF_MAX_ERROR_LEN] = {};

  villagesql::run_query(
      "SELECT name, type, count FROM INFORMATION_SCHEMA.INNODB_METRICS"
      " WHERE status = 'enabled'",
      nullptr,
      [&](const std::vector<std::string_view> &row) {
        if (row.size() < 3 || row[0].data() == nullptr ||
            row[1].data() == nullptr || row[2].data() == nullptr)
          return;

        std::string metric_name = "mysql_innodb_metrics_";
        append_sanitized_name(metric_name, std::string(row[0]));

        std::string type(row[1]);
        const char *prom_type = (type == "counter") ? "counter" : "gauge";

        out += "# TYPE ";
        out += metric_name;
        out += ' ';
        out += prom_type;
        out += '\n';
        out += metric_name;
        out += ' ';
        out += std::string(row[2]);
        out += '\n';
      },
      error_msg);
  if (error_msg[0] != '\0') error = true;
}

// =============================================================================
// Metric collection: replica status (multi-channel)
// =============================================================================

static void collect_replica_status(std::string &out, bool &error) {
  char error_msg[VEF_MAX_ERROR_LEN] = {};

  villagesql::run_query(
      "SHOW REPLICA STATUS",
      nullptr,
      [&](const std::vector<std::string_view> &row) {
        if (row.empty()) return;

        std::string channel_name;
        std::string io_running;
        std::string sql_running;
        std::string seconds_behind;
        std::string relay_log_space;
        std::string exec_source_log_pos;
        std::string read_source_log_pos;

        for (size_t i = 0; i < row.size(); ++i) {
          if (row[i].data() == nullptr) continue;

          std::string col_name(row[i]);

          if (i + 1 < row.size() && row[i + 1].data() != nullptr) {
            std::string value(row[i + 1]);

            if (col_name == "Channel_Name") {
              channel_name = value;
            } else if (col_name == "Replica_IO_Running") {
              io_running = value;
            } else if (col_name == "Replica_SQL_Running") {
              sql_running = value;
            } else if (col_name == "Seconds_Behind_Source") {
              seconds_behind = value;
            } else if (col_name == "Relay_Log_Space") {
              relay_log_space = value;
            } else if (col_name == "Exec_Source_Log_Pos") {
              exec_source_log_pos = value;
            } else if (col_name == "Read_Source_Log_Pos") {
              read_source_log_pos = value;
            }
          }
        }

        auto emit_gauge = [&](const std::string &metric_name,
                              const std::string &value) {
          out += "# TYPE ";
          out += metric_name;
          out += " gauge\n";
          out += metric_name;
          if (!channel_name.empty()) {
            out += "{channel=\"";
            append_escaped_value(out, channel_name);
            out += "\"}";
          } else {
            out += "{}";
          }
          out += ' ';
          out += value;
          out += '\n';
        };

        auto emit_bool = [&](const std::string &metric_name,
                             const std::string &value) {
          std::string num = (value == "Yes") ? "1" : "0";
          emit_gauge(metric_name, num);
        };

        if (!io_running.empty()) emit_bool("mysql_replica_io_running", io_running);
        if (!sql_running.empty()) emit_bool("mysql_replica_sql_running", sql_running);

        if (!seconds_behind.empty()) {
          if (seconds_behind == "NULL" || seconds_behind.empty()) {
            emit_gauge("mysql_replica_seconds_behind_source", "NaN");
          } else if (is_numeric(seconds_behind)) {
            emit_gauge("mysql_replica_seconds_behind_source", seconds_behind);
          }
        }

        if (is_numeric(relay_log_space)) {
          emit_gauge("mysql_replica_relay_log_space", relay_log_space);
        }
        if (is_numeric(exec_source_log_pos)) {
          emit_gauge("mysql_replica_exec_source_log_pos", exec_source_log_pos);
        }
        if (is_numeric(read_source_log_pos)) {
          emit_gauge("mysql_replica_read_source_log_pos", read_source_log_pos);
        }
      },
      error_msg);
  if (error_msg[0] != '\0') error = true;
}

// =============================================================================
// Metric collection: binlog
// =============================================================================

static void collect_binlog(std::string &out, bool &error) {
  char error_msg[VEF_MAX_ERROR_LEN] = {};
  int file_count = 0;
  long long total_size = 0;

  villagesql::run_query(
      "SHOW BINARY LOGS",
      nullptr,
      [&](const std::vector<std::string_view> &row) {
        if (row.size() < 2 || row[0].data() == nullptr ||
            row[1].data() == nullptr)
          return;
        file_count++;
        std::string size_str(row[1]);
        if (is_numeric(size_str)) {
          total_size += std::stoll(size_str);
        }
      },
      error_msg);

  out += "# TYPE mysql_binlog_file_count gauge\n";
  out += "mysql_binlog_file_count ";
  out += std::to_string(file_count);
  out += '\n';

  out += "# TYPE mysql_binlog_size_bytes_total gauge\n";
  out += "mysql_binlog_size_bytes_total ";
  out += std::to_string(total_size);
  out += '\n';

  if (error_msg[0] != '\0') error = true;
}

// =============================================================================
// Collect all metrics
// =============================================================================

static std::string collect_metrics(bool &error) {
  std::string out;
  error = false;
  collect_global_status(out, error);
  collect_global_variables(out, error);
  collect_innodb_metrics(out, error);
  collect_replica_status(out, error);
  collect_binlog(out, error);
  return out;
}

// =============================================================================
// HTTP helpers
// =============================================================================

static int setup_listen_socket(const char *bind_addr, int port) {
  if (bind_addr == nullptr || *bind_addr == '\0') return -1;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
    close(fd);
    return -1;
  }

  if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  if (listen(fd, 5) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

static void write_full(int fd, const char *buf, size_t len) {
  size_t written = 0;
  while (written < len) {
    ssize_t n = send(fd, buf + written, len - written, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    written += static_cast<size_t>(n);
  }
}

static ssize_t read_http_request(int fd, char *buf, size_t max_len) {
  size_t total = 0;
  while (total < max_len - 1) {
    ssize_t n = recv(fd, buf + total, max_len - 1 - total, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) break;
    total += static_cast<size_t>(n);
    buf[total] = '\0';
    if (strstr(buf, "\r\n\r\n") != nullptr) break;
  }
  buf[total] = '\0';
  return static_cast<ssize_t>(total);
}

// =============================================================================
// Listener thread
// =============================================================================

static void *listener_thread_main(void *arg) {
  auto *ctx = static_cast<PrometheusContext *>(arg);

  auto *handle = villagesql::background_thread::register_background_thread(
      "prometheus_exporter/listener");

  while (!ctx->shutdown_requested.load(std::memory_order_acquire)) {
    struct pollfd pfds[2];
    pfds[0].fd = ctx->listen_fd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = ctx->wakeup_pipe_r;
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;

    int ret = poll(pfds, 2, -1);
    if (ret < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (pfds[1].revents & POLLIN) break;
    if (!(pfds[0].revents & POLLIN)) continue;

    int client_fd = accept(ctx->listen_fd, nullptr, nullptr);
    if (client_fd < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
          errno == ECONNABORTED)
        continue;
      break;
    }

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char buf[4096];
    ssize_t n = read_http_request(client_fd, buf, sizeof(buf));
    if (n <= 0) {
      close(client_fd);
      continue;
    }

    if (n >= 12 && strncmp(buf, "GET /metrics", 12) == 0 &&
        (buf[12] == ' ' || buf[12] == '?' || buf[12] == '\r' ||
         buf[12] == '\0')) {
      struct timeval t0, t1;
      gettimeofday(&t0, nullptr);
      bool error = false;
      std::string body = collect_metrics(error);
      gettimeofday(&t1, nullptr);
      g_scrape_duration_us =
          (t1.tv_sec - t0.tv_sec) * 1000000LL + (t1.tv_usec - t0.tv_usec);
      g_requests_total++;

      if (error) g_errors_total++;

      std::string response =
          "HTTP/1.1 200 OK\r\n"
          "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
          "Content-Length: " +
          std::to_string(body.size()) +
          "\r\n"
          "Connection: close\r\n"
          "\r\n" +
          body;

      write_full(client_fd, response.c_str(), response.size());
    } else {
      g_errors_total++;
      const char *resp_404 =
          "HTTP/1.1 404 Not Found\r\n"
          "Connection: close\r\n"
          "\r\n";
      write_full(client_fd, resp_404, strlen(resp_404));
    }

    close(client_fd);
  }

  if (handle != nullptr)
    villagesql::background_thread::unregister_background_thread(handle);

  return nullptr;
}

// =============================================================================
// Lifecycle callbacks
// =============================================================================

static bool start_listener(char *error_msg) {
  const char *bind_addr =
      (g_bind_address != nullptr && *g_bind_address != '\0') ? g_bind_address
                                                              : "127.0.0.1";
  int port = static_cast<int>(g_port);

  auto *ctx = new (std::nothrow) PrometheusContext();
  if (ctx == nullptr) {
    snprintf(error_msg, VEF_MAX_ERROR_LEN,
             "prometheus_exporter: out of memory");
    return true;
  }

  ctx->listen_fd = setup_listen_socket(bind_addr, port);
  if (ctx->listen_fd < 0) {
    snprintf(error_msg, VEF_MAX_ERROR_LEN,
             "prometheus_exporter: failed to bind to %s:%d", bind_addr, port);
    delete ctx;
    return true;
  }

  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    snprintf(error_msg, VEF_MAX_ERROR_LEN,
             "prometheus_exporter: failed to create wakeup pipe: %s",
             strerror(errno));
    close(ctx->listen_fd);
    delete ctx;
    return true;
  }
  ctx->wakeup_pipe_r = pipe_fds[0];
  ctx->wakeup_pipe_w = pipe_fds[1];

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

  int rc = pthread_create(&ctx->listener_thread, &attr, listener_thread_main,
                          ctx);
  pthread_attr_destroy(&attr);

  if (rc != 0) {
    snprintf(error_msg, VEF_MAX_ERROR_LEN,
             "prometheus_exporter: failed to create listener thread");
    close(ctx->listen_fd);
    close(ctx->wakeup_pipe_r);
    close(ctx->wakeup_pipe_w);
    delete ctx;
    return true;
  }

  g_ctx = ctx;
  return false;
}

static void stop_listener() {
  PrometheusContext *ctx = g_ctx;
  if (ctx == nullptr) return;
  g_ctx = nullptr;

  ctx->shutdown_requested.store(true, std::memory_order_release);

  if (ctx->wakeup_pipe_w >= 0) {
    char byte = 1;
    ssize_t r = write(ctx->wakeup_pipe_w, &byte, 1);
    (void)r;
  }

  void *dummy;
  pthread_join(ctx->listener_thread, &dummy);

  if (ctx->listen_fd >= 0) close(ctx->listen_fd);
  if (ctx->wakeup_pipe_r >= 0) close(ctx->wakeup_pipe_r);
  if (ctx->wakeup_pipe_w >= 0) close(ctx->wakeup_pipe_w);

  delete ctx;
}

static void on_install(InstallResult &out) {
  if (!g_enabled) return;
  char error_msg[VEF_MAX_ERROR_LEN] = {};
  if (start_listener(error_msg)) out.fail(error_msg);
}

static void on_uninstall() { stop_listener(); }

static void on_sys_var_change(const char *var_name) {
  if (strcmp(var_name, "enabled") != 0) return;

  if (g_enabled && g_ctx == nullptr) {
    char error_msg[VEF_MAX_ERROR_LEN] = {};
    start_listener(error_msg);
  } else if (!g_enabled && g_ctx != nullptr) {
    stop_listener();
  }
}

// =============================================================================
// Registration
// =============================================================================

VEF_GENERATE_ENTRY_POINTS(
    make_extension("prometheus_exporter", "0.1.0")
        .sys_var(make_sys_var_bool("enabled",
                                   "Enable the Prometheus HTTP exporter",
                                   &g_enabled, false)
                     .on_change(&on_sys_var_change))
        .sys_var(make_sys_var_int("port", "TCP port for the Prometheus endpoint",
                                  &g_port, 9104, 1024, 65535))
        .sys_var(make_sys_var_str("bind_address",
                                   "IP address to bind the Prometheus endpoint",
                                   &g_bind_address, "127.0.0.1"))
        .status_var(
            make_status_var_int("requests_total",
                                &g_requests_total))
        .status_var(
            make_status_var_int("errors_total", &g_errors_total))
        .status_var(make_status_var_int(
            "scrape_duration_us",
            &g_scrape_duration_us))
        .on_install<&on_install>()
        .on_uninstall<&on_uninstall>())