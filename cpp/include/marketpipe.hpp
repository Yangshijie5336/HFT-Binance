#pragma once

// Public interface for the MarketPipe C++20 market-data runtime.
//
// The core (trade parsing, rolling factors, deterministic demo strategy,
// durable JSONL replay) predates the live collector and is intentionally kept
// small and dependency-free.  The live collector in capture.cpp owns network,
// queue and persistence concerns.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace marketpipe {

inline constexpr const char* kVersion = "0.3.0";

struct Trade {
  std::string symbol;
  std::string side;       // "buy" or "sell" (aggressor side from Binance `m`)
  std::string id;         // exchange trade id, exact text
  std::string price_text; // exact decimal text
  std::string qty_text;   // exact decimal text
  std::string exchange_time;  // ISO-8601 UTC derived from `T`
  double price{};
  double qty{};
  std::uint64_t frame_seq{};
  // Added for the live collector; core replay leaves these defaulted.
  std::int64_t exchange_ts_ns{};
  std::string received_utc;
  std::int64_t received_steady_ns{};
  std::uint64_t connection_generation{};
  std::string connection_id;
  std::string source_type;
  bool out_of_order{};

  std::string event_key() const { return symbol + ":" + id; }
};

struct Factors {
  double return_n{};
  double imbalance{};
  double rate_10s{};
  double average_size{};
  std::size_t window_trades{};
  // Extended snapshot fields.
  std::string symbol;
  std::string trade_id;
  double price{};
  double qty{};
  std::int64_t exchange_ts_ns{};
  std::int64_t received_ns{};
  bool out_of_order{};
};

struct Signal {
  std::string action;
  std::string reason;
  double score{};
  std::string symbol;
  std::string trade_id;
  double price{};
  std::int64_t created_steady_ns{};
  bool out_of_order{};
};

// Context stamped onto a trade while it travels through the pipeline.
struct ParseContext {
  std::string run_id;
  std::string connection_id;
  std::string expected_symbol;
  std::uint64_t connection_generation{};
  std::uint64_t frame_seq{};
  std::string received_utc;
  std::int64_t received_steady_ns{};
};

enum class ParseStatus {
  Ok,
  SubscriptionAck,
  NotTrade,
  SymbolMismatch,
  ServerError,
  Malformed,
};

struct ParsedMessage {
  ParseStatus status{ParseStatus::Malformed};
  std::vector<Trade> trades;
  std::string detail;
};

// Parses a single WebSocket text frame. Supports both the raw
// `/ws/<symbol>@trade` payload and the combined-stream `{"stream":..,"data":..}`
// envelope. Numbers keep their wire text; malformed input yields Malformed.
ParsedMessage parse_binance_message(std::string_view json_text, const ParseContext& context);

// Compatibility helper retained for replay and the existing core tests.
std::optional<Trade> parse_binance_trade(std::string_view json_text, std::uint64_t frame_seq);

// Rolling trade-flow factors. The 30 s window and 10 s rate use receive time
// (monotonic) so out-of-order exchange timestamps cannot trap stale entries;
// exchange timestamps remain attached to every snapshot.
class FactorEngine {
 public:
  explicit FactorEngine(double window_seconds = 30.0, std::size_t return_lookback = 20);
  Factors update(const Trade& trade);

 private:
  struct WindowEntry {
    std::int64_t ts_ns;
    Trade trade;
  };
  std::int64_t window_ns_;
  std::size_t return_lookback_;
  std::deque<WindowEntry> window_;
  std::deque<std::int64_t> ten_second_times_;
  std::deque<double> prices_;
  std::int64_t last_event_ts_ns_{};
  double buy_qty_{};
  double sell_qty_{};
};

Signal evaluate(const Factors& factors);

// Durable append-only journal used by the original core/replay path.
class Journal {
 public:
  explicit Journal(const std::filesystem::path& path);
  ~Journal();
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;
  void append(const std::string& payload, std::uint64_t seq, const std::string& run_id,
              std::uint64_t connection_generation = 1, const std::string& received_utc = "");
  void close();
  std::uint64_t records() const { return records_; }
  const std::string& error() const { return error_; }

 private:
  std::FILE* file_{nullptr};
  std::uint64_t records_{};
  std::string error_;
};

// ---------------------------------------------------------------------------
// Live capture
// ---------------------------------------------------------------------------

enum class RunResult { Pass, Partial, Fail, Blocked };
const char* to_string(RunResult result);

struct CaptureConfig {
  // Empty run_id means "UTC timestamp + symbol".
  std::string run_id;
  std::string symbol{"BTCUSDT"};
  std::string ws_url{"wss://stream.binance.com:9443/ws/btcusdt@trade"};
  std::string ca_file;
  double duration_seconds{900.0};
  std::filesystem::path output_dir{"data/runs"};
  std::size_t queue_capacity{65536};
  std::size_t ring_capacity{20000};
  std::size_t dedup_capacity{200000};
  std::size_t fsync_every{100};
  int connect_timeout_ms{10000};
  int idle_timeout_ms{90000};
  double reconnect_initial_seconds{1.0};
  double reconnect_max_seconds{15.0};
  int max_consecutive_connect_failures{10};
  bool console_logs{true};

  // Internal fault-injection hooks used only by automated tests. They are not
  // exposed on the command line.
  std::uint64_t test_processing_delay_us{0};
  std::uint64_t test_fail_raw_after_bytes{0};
  bool test_fail_final_sync{false};
};

struct LatencySummary {
  std::uint64_t samples{};
  double p50_us{};
  double p95_us{};
  double p99_us{};
  double max_us{};
};

struct GapWindow {
  std::uint64_t generation{};
  double gap_seconds{};
  std::string disconnected_utc;
  std::string reconnected_utc;
};

struct CaptureSummary {
  std::string run_id;
  std::string symbol;
  std::string ws_url;
  std::string run_dir;
  RunResult result{RunResult::Fail};
  bool stopped_by_signal{false};
  std::string failure;
  std::string error_detail;
  int http_status{0};
  bool http_blocked{false};
  bool tls_failure{false};

  std::string started_utc;
  std::string finished_utc;
  std::int64_t started_wall_ns{};
  std::int64_t finished_wall_ns{};
  double duration_target_seconds{};
  double duration_actual_seconds{};
  double processing_elapsed_seconds{};
  double drain_elapsed_seconds{};

  std::uint64_t frames_received{};
  std::uint64_t raw_frames_records{};
  std::uint64_t raw_frames_bytes{};
  std::uint64_t trade_events_seen{};
  std::uint64_t trades_processed{};
  std::uint64_t duplicates{};
  std::uint64_t out_of_order{};
  std::uint64_t malformed_frames{};
  std::uint64_t subscription_acks{};
  std::uint64_t server_errors{};
  std::uint64_t signals_emitted{};
  std::uint64_t reconnects{};
  std::uint64_t disconnects{};
  std::uint64_t connection_generations{};
  std::uint64_t queue_overflow_events{};
  std::uint64_t dropped_events{};
  std::uint64_t processing_failures{};
  std::size_t queue_capacity{};
  std::size_t queue_high_water{};
  std::size_t ring_capacity{};
  bool queue_overflow{false};
  bool persistence_error{false};
  bool raw_integrity_verified{false};
  std::string raw_frames_sha256;
  std::string raw_fsync_policy;
  std::string derived_fsync_policy;

  double throughput_trades_per_second{};
  double ingress_trades_per_second{};
  double raw_write_bytes_per_second{};
  double average_trades_per_minute{};
  std::uint64_t max_trades_per_minute{};
  std::uint64_t max_trades_per_second{};
  LatencySummary queue_wait_latency;
  LatencySummary receive_to_process_latency;
  LatencySummary receive_to_signal_latency;
  double cpu_percent_avg{};
  std::uint64_t rss_bytes_peak{};

  std::map<std::string, std::uint64_t> activity_by_minute_utc;
  std::map<std::string, std::uint64_t> activity_by_second_utc;
  std::map<std::string, std::uint64_t> signals_by_action;
  std::vector<GapWindow> gap_windows;
  std::map<std::string, bool> quality_checks;
  std::map<std::string, std::string> artifact_sha256;
};

// Runs the live collector synchronously. Returns a process exit code:
// 0 PASS/PARTIAL, 2 FAIL, 2 BLOCKED, 130 interrupted by SIGINT/SIGTERM.
int run_capture(const CaptureConfig& config, CaptureSummary& summary);

// Recomputes artifact hashes and validates a run directory. Writes
// verification.json and performance_report.md when write_files is true.
// Returns 0 when the run verifies as PASS, 3 for PARTIAL, 2 otherwise.
int run_report(const std::filesystem::path& run_dir, bool write_files, std::string& output_json);

// Synthetic processing benchmarks (never represented as live throughput).
int run_benchmark(std::size_t events, std::string_view mode,
                  const std::filesystem::path& output_dir);

int replay(const std::filesystem::path& input, const std::filesystem::path& output);
int synthetic(std::size_t count);

// Shared helpers used by the CLI, collector and tests.
std::string sha256_hex(std::string_view data);
std::string utc_now_iso();

// Cooperative process-wide stop flag, safe to set from a signal handler.
void install_signal_handlers();
void request_stop();
void reset_stop();
bool stop_requested();

}  // namespace marketpipe
