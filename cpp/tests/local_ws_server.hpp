#pragma once

// Controllable loopback WebSocket test server. It implements only what the
// integration tests need: scripted text frames, abrupt disconnects, a burst
// mode and an HTTP 451 handshake response. It never reaches the network.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

namespace test_support {

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

enum class ServerMode { Basic, Reconnect, Burst, Script, Http451 };

inline std::string trade_frame(std::uint64_t id, std::uint64_t timestamp_ms, const char* price,
                               const char* qty, bool maker) {
  std::ostringstream out;
  out << "{\"e\":\"trade\",\"E\":" << timestamp_ms << ",\"s\":\"BTCUSDT\",\"t\":" << id
      << ",\"p\":\"" << price << "\",\"q\":\"" << qty << "\",\"T\":" << timestamp_ms
      << ",\"m\":" << (maker ? "true" : "false") << ",\"M\":true}";
  return out.str();
}

class LocalWsServer {
 public:
  LocalWsServer(ServerMode mode, std::size_t count = 6,
                std::vector<std::string> scripted_frames = {})
      : mode_(mode), count_(count), scripted_frames_(std::move(scripted_frames)) {
    asio::ip::tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), 0);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
    port_ = acceptor_.local_endpoint().port();
    thread_ = std::thread([this] { run(); });
  }

  ~LocalWsServer() { stop(); }

  LocalWsServer(const LocalWsServer&) = delete;
  LocalWsServer& operator=(const LocalWsServer&) = delete;

  std::uint16_t port() const { return port_; }
  int connections() const { return connections_.load(); }
  std::string url() const {
    return "ws://127.0.0.1:" + std::to_string(port_) + "/ws/btcusdt@trade";
  }

  void stop() {
    if (stopped_.exchange(true)) return;
    boost::system::error_code ec;
    acceptor_.close(ec);
    if (thread_.joinable()) thread_.join();
  }

 private:
  void run() {
    while (!stopped_.load()) {
      tcp::socket socket(acceptor_.get_executor());
      boost::system::error_code ec;
      acceptor_.accept(socket, ec);
      if (ec) break;
      connections_.fetch_add(1);
      try {
        handle(std::move(socket));
      } catch (...) {
        // Test server deliberately ignores per-connection failures.
      }
    }
  }

  std::vector<std::string> frames_for(int connection) {
    std::vector<std::string> frames;
    if (mode_ == ServerMode::Script) return scripted_frames_;
    std::size_t count = count_;
    if (mode_ == ServerMode::Reconnect) count = connection == 1 ? 2 : 3;
    for (std::size_t i = 0; i < count; ++i) {
      const std::uint64_t id = next_id_.fetch_add(1);
      const std::uint64_t timestamp = 1700000000000ULL + id * 100ULL;
      std::ostringstream price;
      price.precision(2);
      price << std::fixed << (67000.0 + static_cast<double>(id % 50));
      frames.push_back(trade_frame(id, timestamp, price.str().c_str(), "0.01000000", false));
    }
    return frames;
  }

  void handle(tcp::socket socket) {
    const int connection = connections_.load();
    if (mode_ == ServerMode::Http451) {
      char request[4096];
      boost::system::error_code ignored;
      socket.read_some(asio::buffer(request), ignored);
      const std::string response =
          "HTTP/1.1 451 Unavailable For Legal Reasons\r\n"
          "Content-Length: 0\r\nConnection: close\r\n\r\n";
      asio::write(socket, asio::buffer(response), ignored);
      socket.shutdown(tcp::socket::shutdown_both, ignored);
      socket.close(ignored);
      return;
    }

    beast::websocket::stream<beast::tcp_stream> ws(std::move(socket));
    ws.accept();
    beast::get_lowest_layer(ws).expires_never();
    const auto frames = frames_for(connection);
    for (const auto& frame : frames) {
      if (stopped_.load()) break;
      ws.write(asio::buffer(frame));
      if (mode_ == ServerMode::Basic) std::this_thread::sleep_for(5ms);
    }
    if (mode_ == ServerMode::Reconnect && connection == 1) {
      boost::system::error_code ignored;
      beast::get_lowest_layer(ws).socket().close(ignored);
      return;
    }
    while (!stopped_.load()) std::this_thread::sleep_for(20ms);
    boost::system::error_code ignored;
    ws.close(beast::websocket::close_code::normal, ignored);
  }

  ServerMode mode_;
  std::size_t count_;
  std::vector<std::string> scripted_frames_;
  tcp::acceptor acceptor_{asio::system_executor()};
  std::uint16_t port_{};
  std::thread thread_;
  std::atomic<bool> stopped_{false};
  std::atomic<int> connections_{0};
  std::atomic<std::uint64_t> next_id_{1};
};

}  // namespace test_support
