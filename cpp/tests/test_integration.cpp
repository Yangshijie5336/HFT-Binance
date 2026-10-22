#include "test_framework.hpp"
#include "local_ws_server.hpp"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "marketpipe.hpp"
#include "marketpipe/runtime.hpp"

using namespace marketpipe;
using test_support::LocalWsServer;
using test_support::ServerMode;
using test_support::trade_frame;

namespace {

std::filesystem::path temp_root(const std::string& name) {
  const auto base = std::filesystem::current_path() / ".marketpipe-test-tmp";
  std::error_code ec;
  std::filesystem::create_directories(base, ec);
  const auto path = base / ("integration_" + name + "_" + std::to_string(wall_ns()));
  std::filesystem::remove_all(path, ec);
  std::filesystem::create_directories(path, ec);
  MP_CHECK(!ec);
  return path;
}

CaptureConfig config_for(const LocalWsServer& server, const std::filesystem::path& root,
                         const std::string& run_id, double duration) {
  CaptureConfig config;
  config.symbol = "BTCUSDT";
  config.ws_url = server.url();
  config.duration_seconds = duration;
  config.output_dir = root;
  config.run_id = run_id;
  config.queue_capacity = 1024;
  config.ring_capacity = 100;
  config.fsync_every = 1;
  config.connect_timeout_ms = 2000;
  config.idle_timeout_ms = 5000;
  config.reconnect_initial_seconds = 0.05;
  config.reconnect_max_seconds = 0.2;
  config.max_consecutive_connect_failures = 5;
  config.console_logs = false;
  return config;
}

std::size_t line_count(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::string line;
  std::size_t count = 0;
  while (std::getline(input, line)) ++count;
  return count;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

CaptureSummary read_summary(const std::filesystem::path& root, const std::string& run_id) {
  const auto path = root / run_id / "summary.json";
  MP_CHECK(std::filesystem::exists(path));
  const std::string text = read_text(path);
  MP_CHECK(text.find("\"result\"") != std::string::npos);
  CaptureSummary summary;
  // Return the in-memory summary from run_capture in tests; this helper only
  // checks the file is readable and is used for diagnostics.
  return summary;
}

}  // namespace

MP_TEST(capture_end_to_end_basic_pass) {
  reset_stop();
  const auto root = temp_root("basic");
  LocalWsServer server(ServerMode::Basic, 8);
  CaptureConfig config = config_for(server, root, "basic", 1.5);
  CaptureSummary summary;
  const int exit_code = run_capture(config, summary);
  server.stop();

  MP_CHECK_EQ(exit_code, 0);
  MP_CHECK(summary.result == RunResult::Pass);
  MP_CHECK_EQ(summary.frames_received, 8u);
  MP_CHECK_EQ(summary.raw_frames_records, 8u);
  MP_CHECK_EQ(summary.trades_processed, 8u);
  MP_CHECK_EQ(summary.signals_emitted, 8u);
  MP_CHECK_EQ(summary.duplicates, 0u);
  MP_CHECK_EQ(summary.malformed_frames, 0u);
  MP_CHECK_EQ(summary.disconnects, 0u);
  MP_CHECK(summary.raw_integrity_verified);
  MP_CHECK(!summary.raw_frames_sha256.empty());
  MP_CHECK(summary.connection_generations == 1);
  MP_CHECK(summary.receive_to_process_latency.samples > 0);
  MP_CHECK(summary.queue_wait_latency.samples > 0);

  const auto run_dir = std::filesystem::path(summary.run_dir);
  MP_CHECK_EQ(line_count(run_dir / "raw_frames.jsonl"), 8u);
  MP_CHECK_EQ(line_count(run_dir / "normalized_events.jsonl"), 8u);
  MP_CHECK_EQ(line_count(run_dir / "factors.jsonl"), 8u);
  MP_CHECK_EQ(line_count(run_dir / "signals.jsonl"), 8u);
  const std::string signals = read_text(run_dir / "signals.jsonl");
  MP_CHECK(signals.find("BUY_SIGNAL") != std::string::npos ||
           signals.find("SELL_SIGNAL") != std::string::npos);
  MP_CHECK(signals.find("DISABLED_DEMO_ONLY") != std::string::npos);

  std::string verification;
  MP_CHECK_EQ(run_report(run_dir, true, verification), 0);
  MP_CHECK(verification.find("\"verified\":true") != std::string::npos);
  MP_CHECK(std::filesystem::exists(run_dir / "verification.json"));
  MP_CHECK(std::filesystem::exists(run_dir / "performance_report.md"));

  std::filesystem::remove_all(root);
}

MP_TEST(reconnect_isolates_generations_and_records_gap) {
  reset_stop();
  const auto root = temp_root("reconnect");
  LocalWsServer server(ServerMode::Reconnect);
  CaptureConfig config = config_for(server, root, "reconnect", 2.2);
  CaptureSummary summary;
  const int exit_code = run_capture(config, summary);
  server.stop();

  MP_CHECK_EQ(exit_code, 0);
  MP_CHECK(summary.result == RunResult::Partial);
  MP_CHECK(summary.connection_generations >= 2);
  MP_CHECK(summary.reconnects >= 1);
  MP_CHECK(summary.disconnects >= 1);
  MP_CHECK_EQ(summary.frames_received, 5u);
  MP_CHECK_EQ(summary.trades_processed, 5u);
  MP_CHECK(!summary.gap_windows.empty());
  MP_CHECK(summary.gap_windows.front().generation >= 2);
  MP_CHECK(summary.gap_windows.front().gap_seconds >= 0.0);

  const std::string raw = read_text(std::filesystem::path(summary.run_dir) / "raw_frames.jsonl");
  MP_CHECK(raw.find("\"connection_generation\":1") != std::string::npos);
  MP_CHECK(raw.find("\"connection_generation\":2") != std::string::npos);
  std::filesystem::remove_all(root);
}

MP_TEST(duplicate_and_out_of_order_trades_are_counted) {
  reset_stop();
  const auto root = temp_root("dedup");
  std::vector<std::string> frames = {
      trade_frame(1, 1000, "67000.00", "0.01000000", false),
      trade_frame(3, 1200, "67002.00", "0.01000000", false),
      trade_frame(2, 1100, "67001.00", "0.01000000", false),
      trade_frame(2, 1100, "67001.00", "0.01000000", false),
      trade_frame(4, 1300, "67003.00", "0.01000000", false),
  };
  LocalWsServer server(ServerMode::Script, 0, frames);
  CaptureConfig config = config_for(server, root, "dedup", 1.4);
  CaptureSummary summary;
  const int exit_code = run_capture(config, summary);
  server.stop();

  MP_CHECK_EQ(exit_code, 0);
  MP_CHECK(summary.result == RunResult::Partial);
  MP_CHECK_EQ(summary.frames_received, 5u);
  MP_CHECK_EQ(summary.trades_processed, 3u);
  MP_CHECK_EQ(summary.duplicates, 1u);
  MP_CHECK_EQ(summary.out_of_order, 1u);
  MP_CHECK(summary.raw_integrity_verified);
  std::filesystem::remove_all(root);
}

MP_TEST(malformed_frames_do_not_stop_capture) {
  reset_stop();
  const auto root = temp_root("malformed");
  std::vector<std::string> frames = {
      trade_frame(1, 1000, "67000.00", "0.01000000", false),
      "not-json-at-all",
      R"({"e":"trade"})",
      R"({"e":"ping"})",
      trade_frame(2, 1200, "67001.00", "0.01000000", false),
  };
  LocalWsServer server(ServerMode::Script, 0, frames);
  CaptureConfig config = config_for(server, root, "malformed", 1.4);
  CaptureSummary summary;
  const int exit_code = run_capture(config, summary);
  server.stop();

  MP_CHECK_EQ(exit_code, 0);
  MP_CHECK(summary.result == RunResult::Partial);
  MP_CHECK_EQ(summary.frames_received, 5u);
  MP_CHECK_EQ(summary.malformed_frames, 2u);
  MP_CHECK_EQ(summary.trades_processed, 2u);
  MP_CHECK(summary.raw_integrity_verified);
  std::filesystem::remove_all(root);
}

MP_TEST(queue_overflow_invalidates_the_sample) {
  reset_stop();
  const auto root = temp_root("overflow");
  LocalWsServer server(ServerMode::Burst, 300);
  CaptureConfig config = config_for(server, root, "overflow", 3.0);
  config.queue_capacity = 2;
  config.test_processing_delay_us = 20000;
  CaptureSummary summary;
  const int exit_code = run_capture(config, summary);
  server.stop();

  MP_CHECK(exit_code != 0);
  MP_CHECK(summary.result == RunResult::Fail);
  MP_CHECK(summary.queue_overflow);
  MP_CHECK(summary.dropped_events > 0);
  MP_CHECK(summary.queue_overflow_events > 0);
  std::filesystem::remove_all(root);
}

MP_TEST(sigint_stops_drains_and_finalizes) {
  reset_stop();
  const auto root = temp_root("sigint");
  LocalWsServer server(ServerMode::Basic, 100);
  CaptureConfig config = config_for(server, root, "sigint", 30.0);
  CaptureSummary summary;
  int exit_code = -1;
  std::thread runner([&] { exit_code = run_capture(config, summary); });
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  std::raise(SIGINT);
  runner.join();
  server.stop();

  MP_CHECK_EQ(exit_code, 130);
  MP_CHECK(summary.stopped_by_signal);
  MP_CHECK(summary.result == RunResult::Fail);
  MP_CHECK(summary.frames_received > 0);
  MP_CHECK_EQ(summary.raw_frames_records, summary.frames_received);
  MP_CHECK_EQ(summary.trades_processed, summary.frames_received);
  MP_CHECK(summary.raw_integrity_verified);
  MP_CHECK(!summary.queue_overflow);
  MP_CHECK(std::filesystem::exists(std::filesystem::path(summary.run_dir) / "summary.json"));
  MP_CHECK(std::filesystem::exists(std::filesystem::path(summary.run_dir) / "verification.json"));
  reset_stop();
  std::filesystem::remove_all(root);
}

MP_TEST(sigterm_stops_drains_and_finalizes) {
  reset_stop();
  const auto root = temp_root("sigterm");
  LocalWsServer server(ServerMode::Burst, 200);
  CaptureConfig config = config_for(server, root, "sigterm", 20.0);
  CaptureSummary summary;
  int exit_code = -1;
  std::thread runner([&] { exit_code = run_capture(config, summary); });
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  std::raise(SIGTERM);
  runner.join();
  server.stop();

  MP_CHECK_EQ(exit_code, 130);
  MP_CHECK(summary.stopped_by_signal);
  MP_CHECK(summary.frames_received > 0);
  MP_CHECK_EQ(summary.raw_frames_records, summary.frames_received);
  MP_CHECK_EQ(summary.trades_processed, summary.frames_received);
  MP_CHECK(summary.raw_integrity_verified);
  reset_stop();
  std::filesystem::remove_all(root);
}

MP_TEST(http_451_is_blocked_and_not_retried_forever) {
  reset_stop();
  const auto root = temp_root("http451");
  LocalWsServer server(ServerMode::Http451);
  CaptureConfig config = config_for(server, root, "http451", 5.0);
  const auto start = std::chrono::steady_clock::now();
  CaptureSummary summary;
  const int exit_code = run_capture(config, summary);
  server.stop();
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  MP_CHECK_EQ(exit_code, 2);
  MP_CHECK(summary.result == RunResult::Blocked);
  MP_CHECK(summary.http_blocked);
  MP_CHECK_EQ(summary.http_status, 451);
  MP_CHECK_EQ(summary.frames_received, 0u);
  MP_CHECK(summary.failure.find("451") != std::string::npos);
  MP_CHECK(elapsed < 4.0);
  std::filesystem::remove_all(root);
}

MP_TEST(raw_disk_full_marks_persistence_failure) {
  reset_stop();
  const auto root = temp_root("diskfull");
  LocalWsServer server(ServerMode::Basic, 80);
  CaptureConfig config = config_for(server, root, "diskfull", 2.0);
  config.test_fail_raw_after_bytes = 500;
  CaptureSummary summary;
  const int exit_code = run_capture(config, summary);
  server.stop();

  MP_CHECK(exit_code != 0);
  MP_CHECK(summary.result == RunResult::Fail);
  MP_CHECK(summary.persistence_error);
  MP_CHECK(!summary.quality_checks["zero_persistence_errors"]);
  std::filesystem::remove_all(root);
}

MP_TEST(final_flush_failure_marks_failure) {
  reset_stop();
  const auto root = temp_root("finalsync");
  LocalWsServer server(ServerMode::Basic, 10);
  CaptureConfig config = config_for(server, root, "finalsync", 1.2);
  config.test_fail_final_sync = true;
  CaptureSummary summary;
  const int exit_code = run_capture(config, summary);
  server.stop();

  MP_CHECK(exit_code != 0);
  MP_CHECK(summary.result == RunResult::Fail);
  MP_CHECK(summary.persistence_error);
  std::filesystem::remove_all(root);
}

MP_TEST(report_detects_tampered_raw_file) {
  reset_stop();
  const auto root = temp_root("tamper");
  LocalWsServer server(ServerMode::Basic, 6);
  CaptureConfig config = config_for(server, root, "tamper", 1.1);
  CaptureSummary summary;
  MP_CHECK_EQ(run_capture(config, summary), 0);
  server.stop();
  const auto run_dir = std::filesystem::path(summary.run_dir);
  std::string verification;
  MP_CHECK_EQ(run_report(run_dir, true, verification), 0);

  {
    std::ofstream raw(run_dir / "raw_frames.jsonl", std::ios::app);
    raw << "{\"tampered\":true}\n";
  }
  MP_CHECK_EQ(run_report(run_dir, true, verification), 2);
  MP_CHECK(verification.find("\"verified\":false") != std::string::npos);
  std::filesystem::remove_all(root);
}

MP_TEST(replay_reproduces_normalized_pipeline) {
  reset_stop();
  const auto root = temp_root("replay");
  LocalWsServer server(ServerMode::Basic, 7);
  CaptureConfig config = config_for(server, root, "capture", 1.1);
  CaptureSummary summary;
  MP_CHECK_EQ(run_capture(config, summary), 0);
  server.stop();
  const auto replay_dir = root / "replay";
  MP_CHECK_EQ(replay(std::filesystem::path(summary.run_dir) / "raw_frames.jsonl", replay_dir), 0);
  MP_CHECK_EQ(line_count(replay_dir / "normalized_events.jsonl"), summary.trades_processed);
  MP_CHECK_EQ(line_count(replay_dir / "factors.jsonl"), summary.trades_processed);
  MP_CHECK_EQ(line_count(replay_dir / "signals.jsonl"), summary.trades_processed);
  std::filesystem::remove_all(root);
}

MP_TEST(plaintext_websocket_is_rejected_for_non_loopback) {
  reset_stop();
  CaptureConfig config;
  config.ws_url = "ws://example.com:80/ws/btcusdt@trade";
  config.duration_seconds = 0.2;
  config.console_logs = false;
  CaptureSummary summary;
  MP_CHECK_EQ(run_capture(config, summary), 2);
}

MP_TEST(connection_refused_returns_nonzero_with_diagnostics) {
  reset_stop();
  const auto root = temp_root("refused");
  const auto probe = std::filesystem::temp_directory_path();  // only used for a unique name
  LocalWsServer placeholder(ServerMode::Basic, 1);
  const std::uint16_t port = placeholder.port();
  placeholder.stop();
  CaptureConfig config;
  config.ws_url = "ws://127.0.0.1:" + std::to_string(port) + "/ws/btcusdt@trade";
  config.duration_seconds = 1.0;
  config.output_dir = root;
  config.run_id = "refused";
  config.max_consecutive_connect_failures = 2;
  config.reconnect_initial_seconds = 0.05;
  config.reconnect_max_seconds = 0.1;
  config.console_logs = false;
  CaptureSummary summary;
  const int exit_code = run_capture(config, summary);
  MP_CHECK(exit_code != 0);
  MP_CHECK(summary.result == RunResult::Fail);
  MP_CHECK(summary.frames_received == 0);
  MP_CHECK(!summary.failure.empty());
  MP_CHECK(std::filesystem::exists(std::filesystem::path(summary.run_dir) / "system.log"));
  (void)probe;
  std::filesystem::remove_all(root);
}

int main() {
  install_signal_handlers();
  reset_stop();
  return testing::run_all("integration");
}
