#include "marketpipe.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/system/system_error.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <unistd.h>
#endif

#include "marketpipe/json.hpp"
#include "marketpipe/runtime.hpp"

namespace marketpipe {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

struct Endpoint {
  bool tls{true};
  std::string host;
  std::string port;
  std::string target;
  bool needs_subscribe{false};
};

bool parse_endpoint(const std::string& url, Endpoint& endpoint, std::string& error) {
  const std::string wss = "wss://";
  const std::string ws = "ws://";
  std::string rest;
  if (url.rfind(wss, 0) == 0) {
    endpoint.tls = true;
    endpoint.port = "443";
    rest = url.substr(wss.size());
  } else if (url.rfind(ws, 0) == 0) {
    endpoint.tls = false;
    endpoint.port = "80";
    rest = url.substr(ws.size());
  } else {
    error = "WebSocket URL must start with ws:// or wss://";
    return false;
  }
  const std::size_t slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  endpoint.target = slash == std::string::npos ? "/" : rest.substr(slash);
  if (authority.empty()) {
    error = "WebSocket URL has no host";
    return false;
  }
  if (authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string::npos) {
      error = "invalid IPv6 host";
      return false;
    }
    endpoint.host = authority.substr(1, close - 1);
    if (close + 1 < authority.size() && authority[close + 1] == ':') {
      endpoint.port = authority.substr(close + 2);
    }
  } else {
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
      endpoint.host = authority.substr(0, colon);
      endpoint.port = authority.substr(colon + 1);
    } else {
      endpoint.host = authority;
    }
  }
  if (endpoint.host.empty() || endpoint.port.empty()) {
    error = "invalid WebSocket host or port";
    return false;
  }
  endpoint.needs_subscribe =
      endpoint.target.find('@') == std::string::npos &&
      endpoint.target.find("streams=") == std::string::npos &&
      endpoint.target.find("stream=") == std::string::npos;
  return true;
}

bool is_loopback_host(const std::string& host) {
  return host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "[::1]";
}

std::string fields(std::initializer_list<std::string> parts) {
  std::string result;
  bool first = true;
  for (const auto& part : parts) {
    if (part.empty()) continue;
    if (!first) result.push_back(',');
    result += part;
    first = false;
  }
  return result;
}

std::string f_string(std::string_view key, std::string_view value) {
  return "\"" + std::string(key) + "\":\"" + json::escape(value) + "\"";
}
std::string f_uint(std::string_view key, std::uint64_t value) {
  return "\"" + std::string(key) + "\":" + std::to_string(value);
}
std::string f_int(std::string_view key, std::int64_t value) {
  return "\"" + std::string(key) + "\":" + std::to_string(value);
}
std::string f_double(std::string_view key, double value) {
  return "\"" + std::string(key) + "\":" + format_double(value);
}
std::string f_bool(std::string_view key, bool value) {
  return "\"" + std::string(key) + "\":" + (value ? "true" : "false");
}

struct RawFrame {
  std::uint64_t seq{};
  std::uint64_t generation{};
  std::string connection_id;
  std::string received_utc;
  std::int64_t received_steady_ns{};
  std::string payload;
};

struct ProcessItem {
  RawFrame frame;
  std::int64_t enqueued_ns{};
};

std::string raw_line(const RawFrame& frame, const std::string& run_id) {
  const std::string payload_hash = sha256_hex(frame.payload);
  std::ostringstream line;
  line << "{\"record_type\":\"raw_websocket_frame\",\"run_id\":\"" << json::escape(run_id)
       << "\",\"connection_id\":\"" << json::escape(frame.connection_id)
       << "\",\"connection_generation\":" << frame.generation << ",\"frame_seq\":" << frame.seq
       << ",\"received_at_utc\":\"" << json::escape(frame.received_utc)
       << "\",\"received_monotonic_ns\":" << frame.received_steady_ns
       << ",\"received_utc\":\"" << json::escape(frame.received_utc)
       << "\",\"received_ns\":" << frame.received_steady_ns << ",\"payload\":\""
       << json::escape(frame.payload) << "\",\"raw_payload\":\""
       << json::escape(frame.payload) << "\",\"payload_sha256\":\"" << payload_hash
       << "\",\"payload_bytes\":" << frame.payload.size() << '}';
  return line.str();
}

class CancelToken {
 public:
  void set(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = nullptr;
  }
  void invoke() {
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callback = callback_;
    }
    if (callback) callback();
  }

 private:
  std::mutex mutex_;
  std::function<void()> callback_;
};

class CancelController {
 public:
  void activate(asio::io_context& ioc, std::shared_ptr<CancelToken> token) {
    std::lock_guard<std::mutex> lock(mutex_);
    ioc_ = &ioc;
    token_ = std::move(token);
  }
  void deactivate() {
    std::lock_guard<std::mutex> lock(mutex_);
    token_.reset();
    ioc_ = nullptr;
  }
  bool active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<bool>(token_);
  }
  void cancel() {
    std::shared_ptr<CancelToken> token;
    asio::io_context* ioc = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      token = token_;
      ioc = ioc_;
    }
    if (!token || !ioc) return;
    // Cancellation is posted onto the io_context so the socket is only
    // touched from the thread that owns it.
    asio::post(*ioc, [token] { token->invoke(); });
  }

 private:
  mutable std::mutex mutex_;
  asio::io_context* ioc_{nullptr};
  std::shared_ptr<CancelToken> token_;
};

std::string connection_id(const std::string& run_id, std::uint64_t generation) {
  return run_id + "-conn-" + std::to_string(generation) + "-" +
         std::to_string(wall_ns() % 1000000);
}

std::filesystem::path executable_directory() {
#ifdef _WIN32
  std::wstring buffer(32768, L'\0');
  const DWORD length =
      GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length > 0 && length < buffer.size()) {
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
  }
#else
  char buffer[4096];
  const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length > 0) {
    buffer[length] = '\0';
    return std::filesystem::path(buffer).parent_path();
  }
#endif
  return std::filesystem::current_path();
}

bool configure_ca(asio::ssl::context& context, Logger& logger, const std::string& configured_file) {
  context.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                      asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                      asio::ssl::context::no_tlsv1_1);
  boost::system::error_code ec;
  context.set_default_verify_paths(ec);
  if (ec) {
    logger.warn("tls_default_verify_paths_failed", fields({f_string("error", ec.message())}));
  }

  std::vector<std::filesystem::path> candidates;
  bool explicit_required = false;
  if (!configured_file.empty()) {
    candidates.emplace_back(configured_file);
    explicit_required = true;
  }
  for (const char* variable : {"MARKETPIPE_CA_FILE", "SSL_CERT_FILE"}) {
    const char* value = std::getenv(variable);
    if (value && *value) {
      candidates.emplace_back(value);
      if (std::string(variable) == "MARKETPIPE_CA_FILE") explicit_required = true;
    }
  }
  const auto cwd = std::filesystem::current_path();
  const auto exe_dir = executable_directory();
  candidates.push_back(cwd / "certs" / "cacert.pem");
  candidates.push_back(cwd / "cacert.pem");
  candidates.push_back(exe_dir / "certs" / "cacert.pem");
  candidates.push_back(exe_dir / ".." / "certs" / "cacert.pem");
  candidates.push_back(exe_dir / "cacert.pem");
#ifndef _WIN32
  candidates.emplace_back("/etc/ssl/certs/ca-certificates.crt");
  candidates.emplace_back("/etc/pki/tls/certs/ca-bundle.crt");
#endif

  bool loaded_bundle = false;
  std::error_code exists_error;
  for (const auto& candidate : candidates) {
    if (!std::filesystem::exists(candidate, exists_error)) continue;
    boost::system::error_code load_error;
    context.load_verify_file(candidate.string(), load_error);
    if (load_error) {
      logger.error("tls_ca_bundle_load_failed",
                   fields({f_string("path", candidate.string()),
                           f_string("error", load_error.message())}));
      if (explicit_required) return false;
      continue;
    }
    logger.info("tls_ca_bundle_loaded",
                fields({f_string("path", std::filesystem::absolute(candidate).string())}));
    loaded_bundle = true;
    break;
  }
  if (explicit_required && !loaded_bundle) {
    logger.error("tls_ca_bundle_not_found", fields({f_string("path", configured_file)}));
    return false;
  }

#ifdef _WIN32
  // Supplement the bundle with the Windows Root store when available.
  std::size_t added = 0;
  for (DWORD location : {CERT_SYSTEM_STORE_CURRENT_USER, CERT_SYSTEM_STORE_LOCAL_MACHINE}) {
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_A, 0, 0,
                                     location | CERT_STORE_READONLY_FLAG, "ROOT");
    if (!store) {
      logger.warn("tls_windows_root_store_open_failed",
                  fields({f_uint("location", location), f_uint("win32_error", GetLastError())}));
      continue;
    }
    PCCERT_CONTEXT certificate = nullptr;
    while ((certificate = CertEnumCertificatesInStore(store, certificate)) != nullptr) {
      const unsigned char* encoded = certificate->pbCertEncoded;
      X509* x509 = d2i_X509(nullptr, &encoded, static_cast<long>(certificate->cbCertEncoded));
      if (x509) {
        if (X509_STORE_add_cert(SSL_CTX_get_cert_store(context.native_handle()), x509)) ++added;
        X509_free(x509);
      }
    }
    CertCloseStore(store, 0);
  }
  logger.info("tls_windows_root_store_scanned", fields({f_uint("certificates_added", added)}));
#endif
  return true;
}

struct NetworkState {
  const CaptureConfig& config;
  Endpoint endpoint;
  BoundedQueue<RawFrame>& raw_queue;
  Logger& logger;
  CaptureSummary& summary;
  CancelController& canceller;
  std::atomic<bool>& fatal;
  std::chrono::steady_clock::time_point deadline;
  asio::io_context* ioc{nullptr};
  std::uint64_t frame_seq{0};
  bool any_connected{false};
  bool tls_verified{false};
  std::optional<std::chrono::steady_clock::time_point> last_frame_time;
  std::string last_frame_utc;
  std::string last_disconnect_utc;
  int consecutive_connect_failures{0};
};

enum class SessionEnd { Stopped, Deadline, Disconnected, Fatal, Blocked };

struct SessionOutcome {
  SessionEnd end{SessionEnd::Disconnected};
  bool connected{false};
  std::string error;
  int http_status{0};
};

SessionEnd classify_end(const NetworkState& state) {
  if (state.summary.http_blocked) return SessionEnd::Blocked;
  if (state.summary.tls_failure) return SessionEnd::Fatal;
  if (stop_requested()) return SessionEnd::Stopped;
  if (state.fatal.load(std::memory_order_relaxed)) return SessionEnd::Fatal;
  if (std::chrono::steady_clock::now() >= state.deadline) return SessionEnd::Deadline;
  return SessionEnd::Disconnected;
}

bool deadline_reached(const NetworkState& state) {
  return std::chrono::steady_clock::now() >= state.deadline;
}

template <typename NextLayer>
asio::awaitable<SessionOutcome> websocket_session(
    websocket::stream<NextLayer>& ws, NetworkState& state, std::uint64_t generation,
    const std::string& connection_id_value) {
  auto token = std::make_shared<CancelToken>();
  token->set([&ws] {
    boost::system::error_code ignored;
    beast::get_lowest_layer(ws).socket().cancel(ignored);
    beast::get_lowest_layer(ws).socket().close(ignored);
  });
  state.canceller.activate(*state.ioc, token);
  struct Guard {
    CancelController& controller;
    std::shared_ptr<CancelToken> token;
    ~Guard() {
      token->clear();
      controller.deactivate();
    }
  } guard{state.canceller, token};

  websocket::stream_base::timeout timeout_option;
  timeout_option.handshake_timeout =
      std::chrono::milliseconds(std::max(1000, state.config.connect_timeout_ms));
  timeout_option.idle_timeout =
      std::chrono::milliseconds(std::max(1000, state.config.idle_timeout_ms));
  timeout_option.keep_alive_pings = true;
  ws.set_option(timeout_option);
  ws.read_message_max(8 * 1024 * 1024);
  ws.binary(false);

  beast::get_lowest_layer(ws).expires_after(
      std::chrono::milliseconds(std::max(1000, state.config.connect_timeout_ms)));

  http::response<http::string_body> response;
  try {
    co_await ws.async_handshake(response, state.endpoint.host, state.endpoint.target,
                                asio::use_awaitable);
  } catch (const boost::system::system_error& error) {
    SessionOutcome outcome;
    outcome.connected = false;
    outcome.error = error.what();
    outcome.http_status = response.result_int();
    if (outcome.http_status == 451) {
      state.summary.http_status = 451;
      state.summary.http_blocked = true;
      state.summary.failure = "Binance WebSocket HTTP 451: geographic access restriction";
      outcome.end = SessionEnd::Blocked;
      state.logger.error("http_451_access_restricted",
                         fields({f_uint("generation", generation),
                                 f_string("host", state.endpoint.host),
                                 f_string("target", state.endpoint.target),
                                 f_string("body", std::string(response.body().substr(0, 512)))}));
    } else if (outcome.http_status >= 400) {
      state.summary.http_status = outcome.http_status;
      state.summary.failure = "WebSocket handshake rejected with HTTP " +
                              std::to_string(outcome.http_status);
      outcome.end = SessionEnd::Fatal;
      state.logger.error("ws_handshake_rejected",
                         fields({f_uint("generation", generation),
                                 f_int("http_status", outcome.http_status),
                                 f_string("body", std::string(response.body().substr(0, 512)))}));
    } else {
      outcome.end = classify_end(state);
    }
    co_return outcome;
  }

  state.any_connected = true;
  // The handshake expiry must be cleared; otherwise Beast's tcp_stream timeout
  // remains armed and tears the session down after connect_timeout even though
  // WebSocket frames are flowing. Idle detection is handled by the WebSocket
  // timeout/keep-alive options above.
  beast::get_lowest_layer(ws).expires_never();
  state.summary.connection_generations = std::max(state.summary.connection_generations, generation);
  state.consecutive_connect_failures = 0;
  if constexpr (std::is_same_v<NextLayer, beast::ssl_stream<beast::tcp_stream>>) {
    state.tls_verified = true;
    const char* version = SSL_get_version(ws.next_layer().native_handle());
    const char* cipher = SSL_get_cipher_name(ws.next_layer().native_handle());
    state.logger.info(
        "ws_connected",
        fields({f_uint("generation", generation), f_string("connection_id", connection_id_value),
                f_string("host", state.endpoint.host), f_string("target", state.endpoint.target),
                f_bool("tls", true), f_string("tls_version", version ? version : "unknown"),
                f_string("tls_cipher", cipher ? cipher : "unknown")}));
  } else {
    state.logger.info("ws_connected",
                      fields({f_uint("generation", generation),
                              f_string("connection_id", connection_id_value),
                              f_string("host", state.endpoint.host),
                              f_string("target", state.endpoint.target), f_bool("tls", false)}));
  }

  if (state.endpoint.needs_subscribe) {
    const std::string subscribe = "{\"method\":\"SUBSCRIBE\",\"params\":[\"" +
                                  state.config.symbol + "@trade\"],\"id\":1}";
    try {
      co_await ws.async_write(asio::buffer(subscribe), asio::use_awaitable);
      state.logger.info("subscribe_sent",
                        fields({f_uint("generation", generation),
                                f_string("symbol", state.config.symbol)}));
    } catch (const boost::system::system_error& error) {
      SessionOutcome outcome;
      outcome.connected = true;
      outcome.error = error.what();
      outcome.end = classify_end(state);
      co_return outcome;
    }
  }

  for (;;) {
    if (stop_requested() || state.fatal.load(std::memory_order_relaxed) || deadline_reached(state)) {
      SessionOutcome outcome;
      outcome.connected = true;
      outcome.end = classify_end(state);
      co_return outcome;
    }
    beast::flat_buffer buffer;
    try {
      co_await ws.async_read(buffer, asio::use_awaitable);
    } catch (const boost::system::system_error& error) {
      SessionOutcome outcome;
      outcome.connected = true;
      outcome.error = error.what();
      const auto code = error.code();
      if (code == asio::error::operation_aborted || code == beast::error::timeout ||
          code == websocket::error::closed || code == asio::error::eof) {
        outcome.end = classify_end(state);
        if (outcome.end == SessionEnd::Disconnected) {
          outcome.error = std::string("WebSocket read failed: ") + error.what();
        }
      } else {
        outcome.end = classify_end(state);
      }
      co_return outcome;
    }

    const std::int64_t received_steady = steady_ns();
    const std::string received_utc = utc_now_iso();
    std::string payload = beast::buffers_to_string(buffer.data());
    RawFrame frame;
    frame.seq = ++state.frame_seq;
    frame.generation = generation;
    frame.connection_id = connection_id_value;
    frame.received_utc = received_utc;
    frame.received_steady_ns = received_steady;
    frame.payload = std::move(payload);
    state.last_frame_time = std::chrono::steady_clock::now();
    state.last_frame_utc = received_utc;
    ++state.summary.frames_received;
    if (!state.raw_queue.try_push(std::move(frame))) {
      state.summary.queue_overflow = true;
      ++state.summary.queue_overflow_events;
      ++state.summary.dropped_events;
      state.fatal.store(true, std::memory_order_relaxed);
      state.logger.error("raw_queue_overflow",
                         fields({f_uint("generation", generation),
                                 f_uint("capacity", state.raw_queue.capacity()),
                                 f_uint("dropped_frames", state.summary.dropped_events)}));
      SessionOutcome outcome;
      outcome.connected = true;
      outcome.end = SessionEnd::Fatal;
      outcome.error = "raw queue overflow";
      co_return outcome;
    }
    if (stop_requested() || state.fatal.load(std::memory_order_relaxed) || deadline_reached(state)) {
      SessionOutcome outcome;
      outcome.connected = true;
      outcome.end = classify_end(state);
      co_return outcome;
    }
  }
}

asio::awaitable<SessionOutcome> tls_session(NetworkState& state, std::uint64_t generation,
                                            const std::string& id, asio::ssl::context& ssl_context) {
  auto executor = co_await asio::this_coro::executor;
  tcp::resolver resolver(executor);
  auto results = co_await resolver.async_resolve(state.endpoint.host, state.endpoint.port,
                                                 asio::use_awaitable);
  beast::ssl_stream<beast::tcp_stream> stream(executor, ssl_context);
  if (!SSL_set_tlsext_host_name(stream.native_handle(), state.endpoint.host.c_str())) {
    throw boost::system::system_error(
        boost::system::error_code(static_cast<int>(::ERR_get_error()),
                                  asio::error::get_ssl_category()),
        "failed to set TLS SNI hostname");
  }
  stream.set_verify_mode(asio::ssl::verify_peer);
  stream.set_verify_callback(asio::ssl::host_name_verification(state.endpoint.host));
  auto& lowest = beast::get_lowest_layer(stream);
  lowest.expires_after(std::chrono::milliseconds(std::max(1000, state.config.connect_timeout_ms)));
  co_await lowest.async_connect(results, asio::use_awaitable);
  lowest.expires_after(std::chrono::milliseconds(std::max(1000, state.config.connect_timeout_ms)));
  co_await stream.async_handshake(asio::ssl::stream_base::client, asio::use_awaitable);
  lowest.expires_never();

  websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(std::move(stream));
  co_return co_await websocket_session(ws, state, generation, id);
}

asio::awaitable<SessionOutcome> plain_session(NetworkState& state, std::uint64_t generation,
                                              const std::string& id) {
  auto executor = co_await asio::this_coro::executor;
  tcp::resolver resolver(executor);
  auto results = co_await resolver.async_resolve(state.endpoint.host, state.endpoint.port,
                                                 asio::use_awaitable);
  beast::tcp_stream stream(executor);
  stream.expires_after(std::chrono::milliseconds(std::max(1000, state.config.connect_timeout_ms)));
  co_await stream.async_connect(results, asio::use_awaitable);
  stream.expires_never();
  websocket::stream<beast::tcp_stream> ws(std::move(stream));
  co_return co_await websocket_session(ws, state, generation, id);
}

void log_connect_failure(NetworkState& state, const boost::system::system_error& error,
                         std::uint64_t generation) {
  const std::string message = error.what();
  const std::string code_message = error.code().message();
  const bool certificate_error =
      message.find("certificate") != std::string::npos ||
      message.find("Certificate") != std::string::npos ||
      message.find("verify failed") != std::string::npos ||
      code_message.find("certificate") != std::string::npos;
  state.logger.error("ws_connect_failed",
                     fields({f_uint("generation", generation), f_string("error", message),
                             f_int("error_code", error.code().value()),
                             f_bool("tls_certificate_error", certificate_error)}));
  if (certificate_error) {
    state.summary.tls_failure = true;
    state.summary.failure = "TLS certificate verification failed: " + message;
    state.fatal.store(true, std::memory_order_relaxed);
  }
}

asio::awaitable<void> interruptible_sleep(std::chrono::milliseconds duration, NetworkState& state) {
  auto executor = co_await asio::this_coro::executor;
  asio::steady_timer timer(executor);
  const auto end = std::chrono::steady_clock::now() + duration;
  while (!stop_requested() && !state.fatal.load(std::memory_order_relaxed) &&
         !deadline_reached(state)) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= end) break;
    const auto slice = std::min<std::chrono::milliseconds>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - now), 100ms);
    timer.expires_after(slice);
    try {
      co_await timer.async_wait(asio::use_awaitable);
    } catch (const boost::system::system_error&) {
      break;
    }
  }
}

asio::awaitable<void> network_loop(NetworkState& state, asio::ssl::context* ssl_context) {
  std::uint64_t generation = 0;
  while (!stop_requested() && !state.fatal.load(std::memory_order_relaxed) &&
         !deadline_reached(state)) {
    ++generation;
    state.summary.connection_generations = generation;
    if (generation > 1) {
      ++state.summary.reconnects;
      if (state.last_frame_time) {
        GapWindow gap;
        gap.generation = generation;
        gap.gap_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                        *state.last_frame_time)
                              .count();
        gap.disconnected_utc = state.last_disconnect_utc;
        gap.reconnected_utc = utc_now_iso();
        state.summary.gap_windows.push_back(gap);
        state.logger.warn("potential_data_gap_after_reconnect",
                          fields({f_uint("generation", generation),
                                  f_double("gap_seconds", gap.gap_seconds),
                                  f_string("last_frame_utc", state.last_frame_utc),
                                  f_string("note",
                                           "reconnect cannot guarantee the exchange feed "
                                           "had no missing trades")}));
      }
    }
    const std::string id = connection_id(state.summary.run_id, generation);
    state.logger.info("ws_connecting",
                      fields({f_uint("generation", generation), f_string("connection_id", id),
                              f_string("url", state.config.ws_url)}));
    SessionOutcome outcome;
    try {
      if (state.endpoint.tls) {
        if (!ssl_context) throw std::runtime_error("TLS context is not initialized");
        outcome = co_await tls_session(state, generation, id, *ssl_context);
      } else {
        outcome = co_await plain_session(state, generation, id);
      }
    } catch (const boost::system::system_error& error) {
      log_connect_failure(state, error, generation);
      outcome.end = state.summary.tls_failure ? SessionEnd::Fatal : SessionEnd::Disconnected;
      outcome.connected = false;
      outcome.error = error.what();
    } catch (const std::exception& error) {
      state.logger.error("ws_session_exception",
                         fields({f_uint("generation", generation), f_string("error", error.what())}));
      outcome.end = SessionEnd::Disconnected;
      outcome.connected = false;
      outcome.error = error.what();
    }

    if (outcome.end == SessionEnd::Blocked) {
      state.logger.error("capture_blocked_by_exchange",
                         fields({f_int("http_status", state.summary.http_status)}));
      break;
    }
    if (outcome.end == SessionEnd::Stopped) {
      state.logger.info("capture_stop_requested",
                        fields({f_uint("generation", generation),
                                f_uint("frames_received", state.summary.frames_received)}));
      break;
    }
    if (outcome.end == SessionEnd::Fatal || state.fatal.load(std::memory_order_relaxed)) break;

    if (outcome.connected) {
      state.logger.warn("ws_session_ended",
                        fields({f_uint("generation", generation),
                                f_string("reason", outcome.error),
                                f_string("end", outcome.end == SessionEnd::Deadline ? "deadline"
                                                                                    : "disconnected")}));
      if (outcome.end == SessionEnd::Disconnected) {
        ++state.summary.disconnects;
        state.last_disconnect_utc = utc_now_iso();
      }
    } else {
      ++state.consecutive_connect_failures;
      state.logger.warn("ws_connect_attempt_failed",
                        fields({f_uint("generation", generation),
                                f_int("consecutive_failures", state.consecutive_connect_failures),
                                f_string("error", outcome.error)}));
    }

    if (outcome.end == SessionEnd::Deadline || deadline_reached(state) || stop_requested()) break;
    if (!outcome.connected &&
        state.consecutive_connect_failures >= state.config.max_consecutive_connect_failures) {
      state.summary.failure = "maximum consecutive WebSocket connection failures reached";
      state.logger.error("ws_max_connect_failures",
                         fields({f_int("failures", state.consecutive_connect_failures)}));
      break;
    }

    const double exponent = std::max(0, state.consecutive_connect_failures - 1);
    const double backoff =
        std::min(state.config.reconnect_max_seconds,
                 state.config.reconnect_initial_seconds * std::pow(2.0, exponent));
    state.logger.info("ws_reconnect_backoff",
                      fields({f_uint("next_generation", generation + 1),
                              f_double("sleep_seconds", backoff)}));
    co_await interruptible_sleep(std::chrono::milliseconds(static_cast<int>(backoff * 1000)),
                                 state);
  }
}

void run_network(NetworkState& state) {
  asio::io_context ioc;
  state.ioc = &ioc;
  std::optional<asio::ssl::context> ssl_context;
  if (state.endpoint.tls) {
    ssl_context.emplace(asio::ssl::context::tls_client);
    if (!configure_ca(*ssl_context, state.logger, state.config.ca_file)) {
      state.summary.tls_failure = true;
      state.summary.failure = "TLS CA bundle could not be loaded";
      state.fatal.store(true, std::memory_order_relaxed);
      return;
    }
  }
  bool finished = false;
  std::thread stopper([&] {
    while (!finished) {
      if (stop_requested() || state.fatal.load(std::memory_order_relaxed) ||
          deadline_reached(state)) {
        state.canceller.cancel();
      }
      std::this_thread::sleep_for(50ms);
    }
  });
  asio::co_spawn(ioc,
                 network_loop(state, ssl_context ? &*ssl_context : nullptr),
                 asio::detached);
  ioc.run();
  finished = true;
  stopper.join();
}

std::string summary_json(const CaptureSummary& summary) {
  std::ostringstream json;
  json << "{\n";
  json << "  \"run_id\":\"" << json::escape(summary.run_id) << "\",\n";
  json << "  \"symbol\":\"" << json::escape(summary.symbol) << "\",\n";
  json << "  \"websocket_url\":\"" << json::escape(summary.ws_url) << "\",\n";
  json << "  \"run_dir\":\"" << json::escape(summary.run_dir) << "\",\n";
  json << "  \"result\":\"" << to_string(summary.result) << "\",\n";
  json << "  \"stopped_by_signal\":" << (summary.stopped_by_signal ? "true" : "false") << ",\n";
  json << "  \"failure\":";
  if (summary.failure.empty()) {
    json << "null,\n";
  } else {
    json << "\"" << json::escape(summary.failure) << "\",\n";
  }
  json << "  \"error_detail\":\"" << json::escape(summary.error_detail) << "\",\n";
  json << "  \"http_status\":" << summary.http_status << ",\n";
  json << "  \"http_blocked\":" << (summary.http_blocked ? "true" : "false") << ",\n";
  json << "  \"tls_failure\":" << (summary.tls_failure ? "true" : "false") << ",\n";
  json << "  \"started_utc\":\"" << summary.started_utc << "\",\n";
  json << "  \"finished_utc\":\"" << summary.finished_utc << "\",\n";
  json << "  \"started_wall_ns\":" << summary.started_wall_ns << ",\n";
  json << "  \"finished_wall_ns\":" << summary.finished_wall_ns << ",\n";
  json << "  \"duration_target_seconds\":" << format_double(summary.duration_target_seconds)
       << ",\n";
  json << "  \"duration_actual_seconds\":" << format_double(summary.duration_actual_seconds)
       << ",\n";
  json << "  \"processing_elapsed_seconds\":"
       << format_double(summary.processing_elapsed_seconds) << ",\n";
  json << "  \"frames_received\":" << summary.frames_received << ",\n";
  json << "  \"raw_frames_records\":" << summary.raw_frames_records << ",\n";
  json << "  \"raw_frames_bytes\":" << summary.raw_frames_bytes << ",\n";
  json << "  \"trade_events_seen\":" << summary.trade_events_seen << ",\n";
  json << "  \"trades_processed\":" << summary.trades_processed << ",\n";
  json << "  \"duplicates\":" << summary.duplicates << ",\n";
  json << "  \"out_of_order\":" << summary.out_of_order << ",\n";
  json << "  \"malformed_frames\":" << summary.malformed_frames << ",\n";
  json << "  \"subscription_acks\":" << summary.subscription_acks << ",\n";
  json << "  \"server_errors\":" << summary.server_errors << ",\n";
  json << "  \"signals_emitted\":" << summary.signals_emitted << ",\n";
  json << "  \"reconnects\":" << summary.reconnects << ",\n";
  json << "  \"disconnects\":" << summary.disconnects << ",\n";
  json << "  \"connection_generations\":" << summary.connection_generations << ",\n";
  json << "  \"queue_capacity\":" << summary.queue_capacity << ",\n";
  json << "  \"queue_high_water\":" << summary.queue_high_water << ",\n";
  json << "  \"ring_capacity\":" << summary.ring_capacity << ",\n";
  json << "  \"queue_overflow\":" << (summary.queue_overflow ? "true" : "false") << ",\n";
  json << "  \"queue_overflow_events\":" << summary.queue_overflow_events << ",\n";
  json << "  \"dropped_events\":" << summary.dropped_events << ",\n";
  json << "  \"persistence_error\":" << (summary.persistence_error ? "true" : "false") << ",\n";
  json << "  \"processing_failures\":" << summary.processing_failures << ",\n";
  json << "  \"raw_frames_sha256\":\"" << summary.raw_frames_sha256 << "\",\n";
  json << "  \"raw_integrity_verified\":"
       << (summary.raw_integrity_verified ? "true" : "false") << ",\n";
  json << "  \"throughput_trades_per_second\":"
       << format_double(summary.throughput_trades_per_second) << ",\n";
  json << "  \"ingress_trades_per_second\":"
       << format_double(summary.ingress_trades_per_second) << ",\n";
  json << "  \"raw_write_bytes_per_second\":"
       << format_double(summary.raw_write_bytes_per_second) << ",\n";
  json << "  \"average_trades_per_minute\":"
       << format_double(summary.average_trades_per_minute) << ",\n";
  json << "  \"max_trades_per_minute\":" << summary.max_trades_per_minute << ",\n";
  json << "  \"max_trades_per_second\":" << summary.max_trades_per_second << ",\n";
  auto latency_json = [&](const char* name, const LatencySummary& latency) {
    json << "  \"" << name << "\":{\"samples\":" << latency.samples
         << ",\"p50_us\":" << format_double(latency.p50_us)
         << ",\"p95_us\":" << format_double(latency.p95_us)
         << ",\"p99_us\":" << format_double(latency.p99_us)
         << ",\"max_us\":" << format_double(latency.max_us) << "},\n";
  };
  latency_json("queue_wait_latency", summary.queue_wait_latency);
  latency_json("receive_to_process_latency", summary.receive_to_process_latency);
  latency_json("receive_to_signal_latency", summary.receive_to_signal_latency);
  json << "  \"cpu_percent_avg\":" << format_double(summary.cpu_percent_avg) << ",\n";
  json << "  \"rss_bytes_peak\":" << summary.rss_bytes_peak << ",\n";
  json << "  \"raw_fsync_policy\":\"" << json::escape(summary.raw_fsync_policy) << "\",\n";
  json << "  \"derived_fsync_policy\":\"" << json::escape(summary.derived_fsync_policy)
       << "\",\n";
  json << "  \"activity_by_minute_utc\":{";
  bool first = true;
  for (const auto& [minute, count] : summary.activity_by_minute_utc) {
    if (!first) json << ',';
    json << "\"" << json::escape(minute) << "\":" << count;
    first = false;
  }
  json << "},\n";
  json << "  \"gap_windows\":[";
  for (std::size_t i = 0; i < summary.gap_windows.size(); ++i) {
    if (i) json << ',';
    const auto& gap = summary.gap_windows[i];
    json << "{\"generation\":" << gap.generation
         << ",\"gap_seconds\":" << format_double(gap.gap_seconds) << ",\"disconnected_utc\":\""
         << json::escape(gap.disconnected_utc) << "\",\"reconnected_utc\":\""
         << json::escape(gap.reconnected_utc) << "\"}";
  }
  json << "],\n";
  json << "  \"signals_by_action\":{";
  first = true;
  for (const auto& [action, count] : summary.signals_by_action) {
    if (!first) json << ',';
    json << "\"" << json::escape(action) << "\":" << count;
    first = false;
  }
  json << "},\n";
  json << "  \"quality_checks\":{";
  first = true;
  for (const auto& [name, value] : summary.quality_checks) {
    if (!first) json << ',';
    json << "\"" << json::escape(name) << "\":" << (value ? "true" : "false");
    first = false;
  }
  json << "},\n";
  json << "  \"artifact_sha256\":{";
  first = true;
  for (const auto& [name, hash] : summary.artifact_sha256) {
    if (!first) json << ',';
    json << "\"" << json::escape(name) << "\":\"" << hash << "\"";
    first = false;
  }
  json << "},\n";
  json << "  \"execution_enabled\":false\n";
  json << "}\n";
  return json.str();
}

struct RawVerification {
  bool ok{false};
  std::uint64_t records{};
  std::uint64_t bytes{};
  std::string sha256;
  std::string error;
};

RawVerification verify_raw_file(const std::filesystem::path& path) {
  RawVerification result;
  std::string hash_error;
  if (!sha256_file(path, result.sha256, hash_error)) {
    result.error = hash_error;
    return result;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    result.error = "cannot reopen raw_frames.jsonl for verification";
    return result;
  }
  std::uint64_t expected_seq = 0;
  std::uint64_t last_generation = 0;
  std::string line;
  std::uint64_t line_no = 0;
  while (std::getline(input, line)) {
    ++line_no;
    if (line.empty()) {
      result.error = "blank line at raw record " + std::to_string(line_no);
      return result;
    }
    json::Value record;
    json::ParseError parse_error;
    if (!json::parse(line, record, parse_error) || !record.is_object()) {
      result.error = "invalid raw JSONL at line " + std::to_string(line_no);
      return result;
    }
    const json::Value* payload = record.find("payload");
    const json::Value* payload_hash = record.find("payload_sha256");
    const json::Value* payload_bytes = record.find("payload_bytes");
    const json::Value* seq = record.find("frame_seq");
    const json::Value* generation = record.find("connection_generation");
    if (!payload || !payload->is_string() || !payload_hash || !payload_hash->is_string() ||
        !payload_bytes || !seq || !generation) {
      result.error = "raw record missing fields at line " + std::to_string(line_no);
      return result;
    }
    if (sha256_hex(payload->text) != payload_hash->text) {
      result.error = "payload SHA-256 mismatch at line " + std::to_string(line_no);
      return result;
    }
    unsigned long long payload_size = 0;
    if (!json::to_uint64(*payload_bytes, payload_size) || payload_size != payload->text.size()) {
      result.error = "payload_bytes mismatch at line " + std::to_string(line_no);
      return result;
    }
    unsigned long long seq_value = 0;
    unsigned long long generation_value = 0;
    if (!json::to_uint64(*seq, seq_value) || !json::to_uint64(*generation, generation_value)) {
      result.error = "invalid sequence/generation at line " + std::to_string(line_no);
      return result;
    }
    if (seq_value <= expected_seq) {
      result.error = "frame_seq is not strictly increasing at line " + std::to_string(line_no);
      return result;
    }
    if (generation_value < last_generation) {
      result.error = "connection generation went backwards at line " + std::to_string(line_no);
      return result;
    }
    expected_seq = seq_value;
    last_generation = generation_value;
    result.bytes += line.size() + 1;
    ++result.records;
  }
  result.ok = true;
  return result;
}

void compute_artifact_hashes(const std::filesystem::path& run_dir,
                             std::map<std::string, std::string>& hashes, Logger& logger) {
  for (const char* name : {"raw_frames.jsonl", "normalized_events.jsonl", "factors.jsonl",
                           "signals.jsonl", "system.log"}) {
    const auto path = run_dir / name;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) continue;
    std::string hash;
    std::string error;
    if (sha256_file(path, hash, error)) {
      hashes[name] = hash;
    } else {
      logger.error("artifact_hash_failed", fields({f_string("file", name), f_string("error", error)}));
    }
  }
}

std::string unique_run_directory(const std::filesystem::path& root, const std::string& run_id) {
  std::filesystem::path candidate = root / run_id;
  std::error_code ec;
  if (!std::filesystem::exists(candidate, ec)) return candidate.string();
  for (int suffix = 1; suffix < 1000; ++suffix) {
    candidate = root / (run_id + "_" + std::to_string(suffix));
    if (!std::filesystem::exists(candidate, ec)) return candidate.string();
  }
  return candidate.string();
}

int write_failure_summary(const std::filesystem::path& run_dir, const CaptureConfig& config,
                          const std::string& run_id, const std::string& failure) {
  CaptureSummary summary;
  summary.run_id = run_id;
  summary.symbol = config.symbol;
  summary.ws_url = config.ws_url;
  summary.run_dir = run_dir.string();
  summary.result = RunResult::Fail;
  summary.failure = failure;
  summary.started_utc = utc_now_iso();
  summary.finished_utc = summary.started_utc;
  summary.started_wall_ns = wall_ns();
  summary.finished_wall_ns = summary.started_wall_ns;
  summary.duration_target_seconds = config.duration_seconds;
  summary.queue_capacity = config.queue_capacity;
  summary.ring_capacity = config.ring_capacity;
  summary.persistence_error = true;
  summary.quality_checks["duration_at_least_99_percent"] = false;
  summary.quality_checks["at_least_one_trade_processed"] = false;
  std::string error;
  atomic_write_file(run_dir / "summary.json", summary_json(summary), error);
  return 2;
}

void shutdown_writer(JsonlFileWriter& writer, CaptureSummary& summary, Logger& logger,
                     const char* name) {
  if (!writer.finalize()) {
    summary.persistence_error = true;
    logger.error("writer_finalize_failed",
                 fields({f_string("file", name), f_string("error", writer.error())}));
  }
}

}  // namespace

int run_capture(const CaptureConfig& config, CaptureSummary& summary) {
  summary = CaptureSummary{};
  summary.symbol = config.symbol;
  summary.ws_url = config.ws_url;
  summary.duration_target_seconds = config.duration_seconds;
  summary.queue_capacity = config.queue_capacity;
  summary.ring_capacity = config.ring_capacity;

  if (config.duration_seconds <= 0) {
    std::cerr << "--duration must be positive\n";
    return 2;
  }
  if (config.queue_capacity == 0 || config.ring_capacity == 0) {
    std::cerr << "queue and ring capacities must be positive\n";
    return 2;
  }

  Endpoint endpoint;
  std::string endpoint_error;
  if (!parse_endpoint(config.ws_url, endpoint, endpoint_error)) {
    std::cerr << endpoint_error << '\n';
    return 2;
  }
  if (!endpoint.tls && !is_loopback_host(endpoint.host)) {
    std::cerr << "refusing plaintext ws:// for non-loopback host; use wss://\n";
    return 2;
  }

  std::string run_id = config.run_id;
  if (run_id.empty()) {
    const std::string stamp = utc_now_iso();
    std::string compact;
    for (char c : stamp) {
      if (c >= '0' && c <= '9') compact.push_back(c);
    }
    // YYYYMMDDTHHMMSSZ
    run_id = compact.substr(0, 14) + "Z_" + config.symbol;
  }
  std::string dir_error;
  if (!ensure_directory(config.output_dir, dir_error)) {
    std::cerr << dir_error << '\n';
    return 2;
  }
  const std::string run_dir_string = unique_run_directory(config.output_dir, run_id);
  const std::filesystem::path run_dir(run_dir_string);
  if (!ensure_directory(run_dir, dir_error)) {
    std::cerr << dir_error << '\n';
    return 2;
  }
  summary.run_id = run_id;
  summary.run_dir = run_dir.string();

  Logger logger(run_dir / "system.log", config.console_logs);
  std::string log_error;
  if (!logger.open(log_error)) {
    std::cerr << log_error << '\n';
    return write_failure_summary(run_dir, config, run_id, log_error);
  }
  logger.info("capture_start",
              fields({f_string("run_id", run_id), f_string("symbol", config.symbol),
                      f_string("url", config.ws_url), f_double("duration_seconds", config.duration_seconds),
                      f_uint("queue_capacity", config.queue_capacity),
                      f_uint("ring_capacity", config.ring_capacity),
                      f_string("tls_mode", endpoint.tls ? "verified" : "loopback_plaintext"),
                      f_int("connect_timeout_ms", config.connect_timeout_ms),
                      f_int("idle_timeout_ms", config.idle_timeout_ms)}));

  summary.started_wall_ns = wall_ns();
  summary.started_utc = utc_iso_from_ns(summary.started_wall_ns);
  const auto start_steady = std::chrono::steady_clock::now();
  const auto deadline =
      start_steady +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(config.duration_seconds));

  WriterFault raw_fault = WriterFault::None;
  if (config.test_fail_raw_after_bytes > 0) {
    raw_fault = WriterFault::SimulatedDiskFull;
  } else if (config.test_fail_final_sync) {
    raw_fault = WriterFault::FinalSyncFailure;
  }
  JsonlFileWriter raw_writer(run_dir / "raw_frames.jsonl", config.fsync_every, raw_fault,
                             config.test_fail_raw_after_bytes);
  JsonlFileWriter events_writer(run_dir / "normalized_events.jsonl", config.fsync_every);
  JsonlFileWriter factors_writer(run_dir / "factors.jsonl", config.fsync_every);
  JsonlFileWriter signals_writer(run_dir / "signals.jsonl", config.fsync_every);
  if (!raw_writer.open(log_error)) {
    logger.error("raw_writer_open_failed", fields({f_string("error", log_error)}));
    logger.close();
    return write_failure_summary(run_dir, config, run_id, log_error);
  }
  if (!events_writer.open(log_error) || !factors_writer.open(log_error) ||
      !signals_writer.open(log_error)) {
    logger.error("derived_writer_open_failed", fields({f_string("error", log_error)}));
    logger.close();
    return write_failure_summary(run_dir, config, run_id, log_error);
  }

  BoundedQueue<RawFrame> raw_queue(config.queue_capacity);
  BoundedQueue<ProcessItem> process_queue(config.queue_capacity);
  std::atomic<bool> fatal{false};
  std::atomic<bool> processing_done{false};
  summary.raw_fsync_policy =
      "buffered_batch_flush_fsync_every_" + std::to_string(config.fsync_every) + "_records_plus_final";
  summary.derived_fsync_policy = summary.raw_fsync_policy;

  ResourceSampler sampler;
  sampler.start();

  std::thread writer_thread([&] {
    try {
      RawFrame frame;
      auto last_flush = std::chrono::steady_clock::now();
      while (raw_queue.pop(frame)) {
        if (!raw_writer.append(raw_line(frame, run_id))) {
          summary.persistence_error = true;
          fatal.store(true, std::memory_order_relaxed);
          logger.error("raw_write_failed",
                       fields({f_string("error", raw_writer.error()), f_uint("frame_seq", frame.seq)}));
        }
        if (!process_queue.try_push(ProcessItem{std::move(frame), steady_ns()})) {
          summary.queue_overflow = true;
          ++summary.queue_overflow_events;
          ++summary.dropped_events;
          fatal.store(true, std::memory_order_relaxed);
          logger.error("process_queue_overflow",
                       fields({f_uint("capacity", process_queue.capacity()),
                               f_uint("dropped_events", summary.dropped_events)}));
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_flush >= 250ms) {
          raw_writer.flush_batch();
          last_flush = now;
        }
      }
      raw_writer.flush_batch();
      // The writer owns final synchronization and the first integrity check:
      // compare the incremental digest with a fresh read of the closed file.
      if (!raw_writer.finalize()) {
        summary.persistence_error = true;
        fatal.store(true, std::memory_order_relaxed);
        logger.error("raw_finalize_failed_in_writer",
                     fields({f_string("error", raw_writer.error())}));
      } else {
        std::string file_hash;
        std::string hash_error;
        if (!sha256_file(run_dir / "raw_frames.jsonl", file_hash, hash_error)) {
          summary.persistence_error = true;
          logger.error("raw_hash_readback_failed", fields({f_string("error", hash_error)}));
        } else if (file_hash != raw_writer.file_sha256()) {
          summary.persistence_error = true;
          summary.raw_integrity_verified = false;
          logger.error("raw_hash_readback_mismatch",
                       fields({f_string("file", file_hash),
                               f_string("incremental", raw_writer.file_sha256())}));
        }
      }
    } catch (const std::exception& error) {
      summary.persistence_error = true;
      fatal.store(true, std::memory_order_relaxed);
      logger.error("writer_thread_exception", fields({f_string("error", error.what())}));
    }
    process_queue.close();
  });

  std::thread processor_thread([&] {
    try {
      TradeDeduplicator deduplicator(config.dedup_capacity);
      FactorEngine factors_engine;
      std::deque<Trade> ring;
      LatencySampler queue_wait;
      LatencySampler process_latency;
      LatencySampler signal_latency;
      std::uint64_t log_budget = 50;
      ProcessItem item;
      while (process_queue.pop(item)) {
        const std::int64_t process_start = steady_ns();
        ParseContext context;
        context.run_id = run_id;
        context.connection_id = item.frame.connection_id;
        context.expected_symbol = config.symbol;
        context.connection_generation = item.frame.generation;
        context.frame_seq = item.frame.seq;
        context.received_utc = item.frame.received_utc;
        context.received_steady_ns = item.frame.received_steady_ns;
        ParsedMessage parsed = parse_binance_message(item.frame.payload, context);
        if (parsed.status == ParseStatus::Malformed) {
          ++summary.malformed_frames;
          if (log_budget > 0) {
            logger.warn("malformed_frame",
                        fields({f_uint("frame_seq", item.frame.seq),
                                f_string("detail", parsed.detail)}));
            --log_budget;
          }
          continue;
        }
        if (parsed.status == ParseStatus::SubscriptionAck) {
          ++summary.subscription_acks;
          logger.info("subscription_ack", fields({f_uint("frame_seq", item.frame.seq)}));
          continue;
        }
        if (parsed.status == ParseStatus::ServerError) {
          ++summary.server_errors;
          logger.error("server_error_frame",
                       fields({f_uint("frame_seq", item.frame.seq), f_string("detail", parsed.detail)}));
          continue;
        }
        if (parsed.status == ParseStatus::SymbolMismatch || parsed.status == ParseStatus::NotTrade) {
          if (log_budget > 0) {
            logger.warn("non_trade_frame",
                        fields({f_uint("frame_seq", item.frame.seq),
                                f_string("detail", parsed.detail)}));
            --log_budget;
          }
          continue;
        }

        for (Trade& trade : parsed.trades) {
          ++summary.trade_events_seen;
          const DedupResult dedup_result = deduplicator.classify(trade);
          if (dedup_result == DedupResult::Duplicate) {
            ++summary.duplicates;
            continue;
          }
          if (dedup_result == DedupResult::OutOfOrder) {
            ++summary.out_of_order;
            continue;
          }
          trade.out_of_order = false;
          ++summary.trades_processed;
          ring.push_back(trade);
          while (ring.size() > config.ring_capacity) ring.pop_front();

          const Factors factors = factors_engine.update(trade);
          const Signal signal = evaluate(factors);
          ++summary.signals_emitted;
          ++summary.signals_by_action[signal.action];

          const std::int64_t now = steady_ns();
          queue_wait.add(now - item.enqueued_ns);
          process_latency.add(now - trade.received_steady_ns);
          signal_latency.add(signal.created_steady_ns - trade.received_steady_ns);

          const std::string minute_key = trade.exchange_time.substr(0, 16);
          const std::string second_key = trade.exchange_time.substr(0, 19);
          ++summary.activity_by_minute_utc[minute_key];
          ++summary.activity_by_second_utc[second_key];

          std::ostringstream event_line;
          event_line << "{\"stage\":\"normalized_event\",\"event\":{\"symbol\":\""
                     << json::escape(trade.symbol) << "\",\"side\":\"" << trade.side
                     << "\",\"price_text\":\"" << json::escape(trade.price_text)
                     << "\",\"qty_text\":\"" << json::escape(trade.qty_text)
                     << "\",\"price\":" << format_double(trade.price)
                     << ",\"qty\":" << format_double(trade.qty) << ",\"trade_id\":\""
                     << json::escape(trade.id) << "\",\"exchange_timestamp\":\""
                     << trade.exchange_time << "\",\"exchange_ts_ns\":" << trade.exchange_ts_ns
                     << ",\"received_utc\":\"" << trade.received_utc
                     << "\",\"received_ns\":" << trade.received_steady_ns
                     << ",\"frame_seq\":" << trade.frame_seq
                     << ",\"connection_generation\":" << trade.connection_generation
                     << ",\"connection_id\":\"" << json::escape(trade.connection_id)
                     << "\",\"event_key\":\"" << json::escape(trade.event_key())
                     << "\",\"source_type\":\"trade\"},\"processed_ns\":" << now
                     << ",\"ring_size\":" << ring.size() << '}';

          std::ostringstream factor_line;
          factor_line << "{\"stage\":\"factor_snapshot\",\"symbol\":\""
                      << json::escape(factors.symbol) << "\",\"trade_id\":\""
                      << json::escape(factors.trade_id)
                      << "\",\"price\":" << format_double(factors.price)
                      << ",\"qty\":" << format_double(factors.qty)
                      << ",\"trade_imbalance\":" << format_double(factors.imbalance)
                      << ",\"return_n\":" << format_double(factors.return_n)
                      << ",\"trade_rate_10s\":" << format_double(factors.rate_10s)
                      << ",\"avg_trade_size\":" << format_double(factors.average_size)
                      << ",\"window_trades\":" << factors.window_trades
                      << ",\"exchange_ts_ns\":" << factors.exchange_ts_ns
                      << ",\"received_ns\":" << factors.received_ns
                      << ",\"computed_ns\":" << now << '}';

          std::ostringstream signal_line;
          signal_line << "{\"symbol\":\"" << json::escape(signal.symbol) << "\",\"trade_id\":\""
                      << json::escape(signal.trade_id) << "\",\"action\":\"" << signal.action
                      << "\",\"reason\":\"" << signal.reason
                      << "\",\"score\":" << format_double(signal.score)
                      << ",\"price\":" << format_double(signal.price)
                      << ",\"created_ns\":" << signal.created_steady_ns
                      << ",\"execution\":\"DISABLED_DEMO_ONLY\"}";

          if (!events_writer.append(event_line.str()) || !factors_writer.append(factor_line.str()) ||
              !signals_writer.append(signal_line.str())) {
            summary.persistence_error = true;
            ++summary.processing_failures;
            fatal.store(true, std::memory_order_relaxed);
            logger.error("derived_write_failed",
                         fields({f_string("events_error", events_writer.error()),
                                 f_string("factors_error", factors_writer.error()),
                                 f_string("signals_error", signals_writer.error())}));
          }
          if (config.test_processing_delay_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(config.test_processing_delay_us));
          }
        }
      }
      summary.queue_wait_latency = queue_wait.summary();
      summary.receive_to_process_latency = process_latency.summary();
      summary.receive_to_signal_latency = signal_latency.summary();
    } catch (const std::exception& error) {
      summary.persistence_error = true;
      ++summary.processing_failures;
      fatal.store(true, std::memory_order_relaxed);
      logger.error("processor_thread_exception", fields({f_string("error", error.what())}));
    }
    processing_done.store(true, std::memory_order_relaxed);
  });

  CancelController canceller;
  NetworkState state{config,      endpoint,       raw_queue, logger, summary,
                     canceller,   fatal,          deadline,  nullptr};
  run_network(state);

  raw_queue.close();
  writer_thread.join();
  process_queue.close();
  processor_thread.join();

  const double capture_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start_steady).count();
  summary.duration_actual_seconds = capture_seconds;
  summary.stopped_by_signal = stop_requested() && capture_seconds < config.duration_seconds * 0.99;

  if (!raw_writer.finalize()) {
    summary.persistence_error = true;
    logger.error("raw_finalize_failed", fields({f_string("error", raw_writer.error())}));
  }
  shutdown_writer(events_writer, summary, logger, "normalized_events.jsonl");
  shutdown_writer(factors_writer, summary, logger, "factors.jsonl");
  shutdown_writer(signals_writer, summary, logger, "signals.jsonl");

  RawVerification raw_verification = verify_raw_file(run_dir / "raw_frames.jsonl");
  summary.raw_frames_records = raw_verification.records;
  summary.raw_frames_bytes = raw_verification.bytes;
  summary.raw_frames_sha256 = raw_verification.sha256;
  summary.raw_integrity_verified = raw_verification.ok;
  if (!raw_verification.ok) {
    summary.persistence_error = true;
    logger.error("raw_integrity_failed", fields({f_string("error", raw_verification.error)}));
  }
  if (raw_verification.ok && raw_writer.file_sha256() != raw_verification.sha256) {
    summary.persistence_error = true;
    summary.raw_integrity_verified = false;
    logger.error("raw_incremental_hash_mismatch",
                 fields({f_string("incremental", raw_writer.file_sha256()),
                         f_string("file", raw_verification.sha256)}));
  }

  summary.queue_high_water = std::max(raw_queue.high_water(), process_queue.high_water());
  summary.queue_overflow = summary.queue_overflow || raw_queue.overflowed() ||
                           process_queue.overflowed();
  if (summary.queue_overflow && summary.queue_overflow_events == 0) {
    summary.queue_overflow_events = 1;
  }
  summary.cpu_percent_avg = sampler.average_cpu_percent();
  summary.rss_bytes_peak = sampler.peak_rss_bytes();
  sampler.stop();

  summary.throughput_trades_per_second =
      capture_seconds > 0 ? static_cast<double>(summary.trades_processed) / capture_seconds : 0.0;
  summary.ingress_trades_per_second =
      capture_seconds > 0 ? static_cast<double>(summary.trade_events_seen) / capture_seconds : 0.0;
  summary.raw_write_bytes_per_second =
      capture_seconds > 0 ? static_cast<double>(summary.raw_frames_bytes) / capture_seconds : 0.0;
  summary.average_trades_per_minute =
      capture_seconds > 0 ? static_cast<double>(summary.trades_processed) / (capture_seconds / 60.0)
                          : 0.0;
  for (const auto& [minute, count] : summary.activity_by_minute_utc) {
    summary.max_trades_per_minute = std::max(summary.max_trades_per_minute, count);
  }
  for (const auto& [second, count] : summary.activity_by_second_utc) {
    summary.max_trades_per_second = std::max(summary.max_trades_per_second, count);
  }

  const bool duration_ok = capture_seconds >= config.duration_seconds * 0.99;
  const bool data_ok = summary.frames_received > 0 && summary.trades_processed > 0;
  const bool records_ok = summary.raw_frames_records == summary.frames_received;
  const bool no_overflow = !summary.queue_overflow && summary.dropped_events == 0;
  const bool no_persistence_error = !summary.persistence_error;
  const bool no_processing_failures = summary.processing_failures == 0;
  const bool no_disconnects = summary.disconnects == 0;
  const bool no_malformed = summary.malformed_frames == 0;
  const bool no_duplicates = summary.duplicates == 0 && summary.out_of_order == 0;
  const bool no_reconnects = summary.reconnects == 0;
  const bool tls_ok = endpoint.tls ? state.tls_verified : is_loopback_host(endpoint.host);

  summary.quality_checks["duration_at_least_99_percent"] = duration_ok;
  summary.quality_checks["at_least_one_trade_processed"] = data_ok;
  summary.quality_checks["raw_record_count_matches_frames"] = records_ok;
  summary.quality_checks["zero_queue_overflow"] = no_overflow;
  summary.quality_checks["zero_persistence_errors"] = no_persistence_error;
  summary.quality_checks["raw_sha256_verified"] = summary.raw_integrity_verified;
  summary.quality_checks["zero_processing_failures"] = no_processing_failures;
  summary.quality_checks["no_disconnects"] = no_disconnects;
  summary.quality_checks["no_malformed_frames"] = no_malformed;
  summary.quality_checks["no_duplicate_or_out_of_order_trades"] = no_duplicates;
  summary.quality_checks["tls_verified_or_loopback_plaintext"] = tls_ok;

  if (summary.http_blocked) {
    summary.result = RunResult::Blocked;
    summary.failure = summary.failure.empty()
                          ? "exchange returned HTTP 451 / access restriction"
                          : summary.failure;
  } else if (summary.stopped_by_signal) {
    summary.result = RunResult::Fail;
    summary.failure = summary.failure.empty() ? "stopped by signal before duration target"
                                              : summary.failure;
  } else if (!duration_ok || !data_ok || !records_ok || !no_overflow || !no_persistence_error ||
             !no_processing_failures || !summary.raw_integrity_verified || !tls_ok) {
    summary.result = RunResult::Fail;
    if (summary.failure.empty()) {
      if (!duration_ok) summary.failure = "capture duration below 99% of target";
      else if (!data_ok) summary.failure = "no trade data captured/processed";
      else if (!records_ok) summary.failure = "raw record count does not match frames received";
      else if (!no_overflow) summary.failure = "bounded queue overflow: sample invalid";
      else if (!no_persistence_error) summary.failure = "raw/derived persistence failure";
      else if (!no_processing_failures) summary.failure = "processing failures";
      else if (!summary.raw_integrity_verified) summary.failure = "raw integrity verification failed";
      else if (!tls_ok) summary.failure = "TLS verification was not established";
    }
  } else if (!no_disconnects || !no_malformed || !no_duplicates || !no_reconnects) {
    summary.result = RunResult::Partial;
    summary.failure = "run completed with disconnects, duplicates, out-of-order events or malformed frames";
  } else {
    summary.result = RunResult::Pass;
  }

  summary.finished_wall_ns = wall_ns();
  summary.finished_utc = utc_iso_from_ns(summary.finished_wall_ns);
  summary.processing_elapsed_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start_steady).count();

  logger.info("capture_finished",
              fields({f_string("result", to_string(summary.result)),
                      f_uint("frames_received", summary.frames_received),
                      f_uint("trades_processed", summary.trades_processed),
                      f_uint("signals_emitted", summary.signals_emitted),
                      f_double("duration_actual_seconds", summary.duration_actual_seconds),
                      f_uint("reconnects", summary.reconnects),
                      f_uint("malformed_frames", summary.malformed_frames),
                      f_uint("duplicates", summary.duplicates),
                      f_uint("out_of_order", summary.out_of_order),
                      f_bool("queue_overflow", summary.queue_overflow),
                      f_bool("persistence_error", summary.persistence_error),
                      f_bool("raw_integrity_verified", summary.raw_integrity_verified),
                      f_string("failure", summary.failure)}));
  logger.close();
  compute_artifact_hashes(run_dir, summary.artifact_sha256, logger);

  std::string summary_error;
  if (!atomic_write_file(run_dir / "summary.json", summary_json(summary), summary_error)) {
    std::cerr << "cannot write summary.json: " << summary_error << '\n';
    return 2;
  }
  std::string verification_json;
  const int report_exit = run_report(run_dir, true, verification_json);
  if (report_exit == 2) {
    std::cerr << "warning: post-run verification did not pass\n";
  }

  std::cout << "capture result=" << to_string(summary.result)
            << " run_dir=" << summary.run_dir << " frames=" << summary.frames_received
            << " processed=" << summary.trades_processed
            << " signals=" << summary.signals_emitted << '\n';

  if (summary.stopped_by_signal) return 130;
  return (summary.result == RunResult::Pass || summary.result == RunResult::Partial) ? 0 : 2;
}

}  // namespace marketpipe
