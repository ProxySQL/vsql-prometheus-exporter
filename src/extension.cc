
// VillageSQL Prometheus Exporter Extension
//
// Exposes a minimal HTTP server on a configurable port that serves
// Prometheus-format metrics at GET /metrics.
//
// System variables:
//   prometheus_exporter_enabled      BOOL   (default: OFF)
//   prometheus_exporter_port         INT    (default: 9104)
//   prometheus_exporter_bind_address STRING (default: "127.0.0.1")
//
// The HTTP listener runs in a background thread registered with MySQL's
// process list. Metrics are collected via run_query() at scrape time.
//
// Uses a pipe + poll(2) for clean shutdown signalling.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <string>
#include <string_view>
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
// Status variables (visible via SHOW GLOBAL STATUS LIKE 'prometheus_exporter%')
// =============================================================================

static long long g_requests_total = 0;   // total /metrics requests served
static long long g_errors_total = 0;     // requests that failed (non-200)
static long long g_scrape_duration_us = 0;  // last scrape duration in microseconds

// =============================================================================
// Runtime state
// =============================================================================

struct PrometheusContext {
  pthread_t listener_thread{};
  int listen_fd{-1};
  int wakeup_pipe_r{-1};  // read end — polled by listener thread
  int wakeup_pipe_w{-1};  // write end — written by stop_listener()
  std::atomic<bool> shutdown_requested{false};
};

static PrometheusContext *g_ctx = nullptr;

// =============================================================================
// Metric collection helpers
// =============================================================================

// Returns true if s is a valid Prometheus numeric value (integer or float,
// consumed entirely by strtod with no trailing characters).
static bool is_numeric(const std::string &s) {
  if (s.empty()) return false;
  char *end = nullptr;
  strtod(s.c_str(), &end);
  return end != nullptr && *end == '\0';
}

// Sanitize a MySQL variable name to a Prometheus-safe metric name suffix:
// lower-case, replace any character outside [a-z0-9] with '_'.
static void append_sanitized_name(std::string &out, const std::string &name) {
  for (char c : name) {
    char lc = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    out += (lc >= 'a' && lc <= 'z') || (lc >= '0' && lc <= '9') ? lc : '_';
  }
}

static void collect_global_status(std::string &out) {
  char error_msg[VEF_MAX_ERROR_LEN] = {};
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
        out += "mysql_global_status_";
        append_sanitized_name(out, name);
        out += ' ';
        out += value;
        out += '\n';
      },
      error_msg);
}

static void collect_global_variables(std::string &out) {
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
        out += "mysql_global_variables_";
        append_sanitized_name(out, name);
        out += ' ';
        out += value;
        out += '\n';
      },
      error_msg);
}

static void collect_innodb_metrics(std::string &out) {
  char error_msg[VEF_MAX_ERROR_LEN] = {};
  // name, subsystem, count, max_count, min_count, avg_count, count_reset,
  // max_count_reset, min_count_reset, avg_count_reset, time_enabled,
  // time_disabled, time_elapsed, time_reset, status, type, comment
  villagesql::run_query(
      "SELECT name, count FROM INFORMATION_SCHEMA.INNODB_METRICS"
      " WHERE status = 'enabled'",
      nullptr,
      [&](const std::vector<std::string_view> &row) {
        if (row.size() < 2 || row[0].data() == nullptr ||
            row[1].data() == nullptr)
          return;
        out += "mysql_info_schema_innodb_metrics_";
        append_sanitized_name(out, std::string(row[0]));
        out += ' ';
        out += std::string(row[1]);
        out += '\n';
      },
      error_msg);
}

static void collect_replica_status(std::string &out) {
  // Replica_IO_Running / Replica_SQL_Running map to 1/0.
  char error_msg[VEF_MAX_ERROR_LEN] = {};
  villagesql::run_query(
      "SHOW REPLICA STATUS",
      nullptr,
      [&](const std::vector<std::string_view> &row) {
        // SHOW REPLICA STATUS returns a single row with many columns.
        // We emit a simple "replica_running" gauge.
        if (row.size() < 2) return;
        // Column indices: Replica_IO_Running=10, Replica_SQL_Running=11
        // (may vary by version — use safe defaults)
        auto io = row.size() > 10 ? row[10] : std::string_view{};
        auto sql = row.size() > 11 ? row[11] : std::string_view{};
        out += "mysql_replica_io_running ";
        out += (io == "Yes" ? "1" : "0");
        out += '\n';
        out += "mysql_replica_sql_running ";
        out += (sql == "Yes" ? "1" : "0");
        out += '\n';
      },
      error_msg);
}

static std::string collect_metrics() {
  std::string out;
  collect_global_status(out);
  collect_global_variables(out);
  collect_innodb_metrics(out);
  collect_replica_status(out);
  return out;
}

// =============================================================================
// HTTP server helpers
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
    if (strstr(buf, "\r\n") != nullptr && total >= 13) break;
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
      std::string body = collect_metrics();
      gettimeofday(&t1, nullptr);
      g_scrape_duration_us =
          (t1.tv_sec - t0.tv_sec) * 1000000LL + (t1.tv_usec - t0.tv_usec);
      g_requests_total++;

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

// Starts the HTTP listener. Returns false on success, true on failure (writing
// a human-readable message to error_msg, which is VEF_MAX_ERROR_LEN bytes).
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

// Stops the HTTP listener if running.
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
    // Errors are silently dropped — the user can observe the state by
    // checking whether the port is listening.
    start_listener(error_msg);
  } else if (!g_enabled && g_ctx != nullptr) {
    stop_listener();
  }
}

// =============================================================================
// Registration
// =============================================================================

VEF_GENERATE_ENTRY_POINTS(
    make_extension("prometheus_exporter", "0.0.1")
        .sys_var(make_sys_var_bool("enabled",
                                   "Enable the Prometheus HTTP exporter",
                                   &g_enabled, false)
                     .on_change(&on_sys_var_change))
        .sys_var(make_sys_var_int("port", "TCP port for the Prometheus endpoint",
                                  &g_port, 9104, 1024, 65535))
        .sys_var(make_sys_var_str("bind_address",
                                   "IP address to bind the Prometheus endpoint",
                                   &g_bind_address, "127.0.0.1"))
        .status_var(make_status_var_int("requests_total", &g_requests_total))
        .status_var(make_status_var_int("errors_total", &g_errors_total))
        .status_var(
            make_status_var_int("scrape_duration_us", &g_scrape_duration_us))
        .on_install<&on_install>()
        .on_uninstall<&on_uninstall>())
