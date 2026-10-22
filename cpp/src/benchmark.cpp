#include "marketpipe.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "marketpipe/json.hpp"
#include "marketpipe/runtime.hpp"

namespace marketpipe {
namespace {

struct BenchResult {
  std::string mode;
  std::uint64_t events{};
  double seconds{};
  LatencySummary latency;
  std::uint64_t signals{};
  std::uint64_t raw_records{};
  std::string raw_sha256;
  std::string output_file;
  double cpu_percent{};
  std::uint64_t rss_bytes{};
  std::uint64_t errors{};
  std::uint64_t dropped{};
};

std::vector<std::string> make_payloads(std::size_t count) {
  std::vector<std::string> payloads;
  payloads.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const long long id = static_cast<long long>(i + 1);
    const long long timestamp = 1700000000000LL + static_cast<long long>(i);
    const bool maker = (i % 7) == 0;
    std::ostringstream price;
    price << std::fixed << std::setprecision(2)
          << (42000.0 + static_cast<double>(i) * 0.5);
    const char* qty = (i % 2) == 0 ? "0.00120000" : "0.00080000";
    std::ostringstream payload;
    payload << "{\"e\":\"trade\",\"E\":" << timestamp << ",\"s\":\"BTCUSDT\",\"t\":" << id
            << ",\"p\":\"" << price.str() << "\",\"q\":\"" << qty << "\",\"T\":" << timestamp
            << ",\"m\":" << (maker ? "true" : "false") << ",\"M\":true}";
    payloads.push_back(payload.str());
  }
  return payloads;
}

BenchResult bench_parse(const std::vector<std::string>& payloads) {
  BenchResult result;
  result.mode = "parse_only";
  result.events = payloads.size();
  LatencySampler latency;
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < payloads.size(); ++i) {
    ParseContext context;
    context.frame_seq = i + 1;
    context.expected_symbol = "BTCUSDT";
    context.received_utc = utc_now_iso();
    context.received_steady_ns = steady_ns();
    ParsedMessage parsed = parse_binance_message(payloads[i], context);
    if (parsed.status != ParseStatus::Ok || parsed.trades.size() != 1) ++result.errors;
    latency.add(steady_ns() - context.received_steady_ns);
  }
  result.seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  result.latency = latency.summary();
  return result;
}

BenchResult bench_pipeline(const std::vector<std::string>& payloads) {
  BenchResult result;
  result.mode = "full_pipeline";
  result.events = payloads.size();
  LatencySampler latency;
  FactorEngine engine;
  TradeDeduplicator dedup;
  const auto start_all = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < payloads.size(); ++i) {
    const auto start = std::chrono::steady_clock::now();
    ParseContext context;
    context.frame_seq = i + 1;
    context.expected_symbol = "BTCUSDT";
    context.received_utc = utc_now_iso();
    context.received_steady_ns = steady_ns();
    ParsedMessage parsed = parse_binance_message(payloads[i], context);
    if (parsed.status != ParseStatus::Ok || parsed.trades.size() != 1) {
      ++result.errors;
      continue;
    }
    Trade trade = parsed.trades.front();
    DedupResult dedup_result = dedup.classify(trade);
    if (dedup_result != DedupResult::New) {
      ++result.dropped;
      continue;
    }
    const Factors factors = engine.update(trade);
    const Signal signal = evaluate(factors);
    if (signal.action != "HOLD") ++result.signals;
    latency.add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
  }
  result.seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start_all).count();
  result.latency = latency.summary();
  return result;
}

BenchResult bench_persist(const std::vector<std::string>& payloads,
                          const std::filesystem::path& output_dir, std::uint64_t index) {
  BenchResult result;
  result.mode = "pipeline_plus_raw_persistence";
  result.events = payloads.size();
  const std::string run_id =
      "benchmark_" + std::to_string(index) + "_" + std::to_string(wall_ns() % 1000000);
  const auto directory = output_dir / run_id;
  std::string error;
  if (!ensure_directory(directory, error)) {
    result.errors = 1;
    result.output_file = error;
    return result;
  }
  JsonlFileWriter writer(directory / "raw_frames.jsonl", 100);
  if (!writer.open(error)) {
    result.errors = 1;
    result.output_file = error;
    return result;
  }
  LatencySampler latency;
  FactorEngine engine;
  TradeDeduplicator dedup;
  const std::string run = run_id;
  const auto start_all = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < payloads.size(); ++i) {
    const auto start = std::chrono::steady_clock::now();
    ParseContext context;
    context.run_id = run;
    context.connection_id = run + "-conn-1";
    context.connection_generation = 1;
    context.frame_seq = i + 1;
    context.expected_symbol = "BTCUSDT";
    context.received_utc = utc_now_iso();
    context.received_steady_ns = steady_ns();
    ParsedMessage parsed = parse_binance_message(payloads[i], context);
    if (parsed.status != ParseStatus::Ok || parsed.trades.size() != 1) {
      ++result.errors;
      continue;
    }
    Trade trade = parsed.trades.front();
    if (dedup.classify(trade) != DedupResult::New) {
      ++result.dropped;
      continue;
    }
    const Factors factors = engine.update(trade);
    const Signal signal = evaluate(factors);
    if (signal.action != "HOLD") ++result.signals;

    std::ostringstream record;
    record << "{\"record_type\":\"raw_websocket_frame\",\"run_id\":\"" << json::escape(run)
           << "\",\"connection_id\":\"" << json::escape(context.connection_id)
           << "\",\"connection_generation\":1,\"frame_seq\":" << context.frame_seq
           << ",\"received_at_utc\":\"" << context.received_utc
           << "\",\"received_monotonic_ns\":" << context.received_steady_ns
           << ",\"payload\":\"" << json::escape(payloads[i]) << "\",\"payload_sha256\":\""
           << sha256_hex(payloads[i]) << "\",\"payload_bytes\":" << payloads[i].size() << '}';
    if (!writer.append(record.str())) {
      ++result.errors;
      break;
    }
    latency.add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
  }
  if (!writer.finalize()) ++result.errors;
  result.seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start_all).count();
  result.latency = latency.summary();
  result.raw_records = writer.records();
  result.raw_sha256 = writer.file_sha256();
  result.output_file = (directory / "raw_frames.jsonl").string();
  return result;
}

std::string result_json(const BenchResult& result) {
  std::ostringstream json;
  json << "{\"benchmark_type\":\"SYNTHETIC_BENCHMARK\",\"mode\":\"" << result.mode
       << "\",\"live_network\":false,\"events\":" << result.events
       << ",\"events_per_second\":"
       << (result.seconds > 0 ? static_cast<double>(result.events) / result.seconds : 0.0)
       << ",\"duration_seconds\":" << format_double(result.seconds)
       << ",\"p50_us\":" << format_double(result.latency.p50_us)
       << ",\"p95_us\":" << format_double(result.latency.p95_us)
       << ",\"p99_us\":" << format_double(result.latency.p99_us)
       << ",\"latency_samples\":" << result.latency.samples
       << ",\"signals_emitted\":" << result.signals
       << ",\"raw_frames_written\":" << result.raw_records
       << ",\"raw_frames_sha256\":\"" << result.raw_sha256 << "\""
       << ",\"output_file\":\"" << json::escape(result.output_file) << "\""
       << ",\"cpu_percent\":" << format_double(result.cpu_percent)
       << ",\"rss_bytes\":" << result.rss_bytes << ",\"dropped_events\":" << result.dropped
       << ",\"errors\":" << result.errors << ",\"execution_enabled\":false}";
  return json.str();
}

}  // namespace

int run_benchmark(std::size_t events, std::string_view mode,
                  const std::filesystem::path& output_dir) {
  if (events == 0) {
    std::cerr << "--events must be positive\n";
    return 2;
  }
  const std::vector<std::string> payloads = make_payloads(events);
  std::vector<BenchResult> results;

  auto run_one = [&](const std::string& requested) {
    const std::uint64_t cpu_start = process_cpu_ns();
    const std::int64_t wall_start = steady_ns();
    BenchResult result;
    if (requested == "parse") {
      result = bench_parse(payloads);
    } else if (requested == "pipeline") {
      result = bench_pipeline(payloads);
    } else if (requested == "persist") {
      result = bench_persist(payloads, output_dir, static_cast<std::uint64_t>(results.size() + 1));
    } else {
      return false;
    }
    const std::uint64_t cpu_end = process_cpu_ns();
    const std::int64_t wall_end = steady_ns();
    const double wall_seconds = static_cast<double>(wall_end - wall_start) / 1e9;
    result.cpu_percent =
        wall_seconds > 0 ? 100.0 * static_cast<double>(cpu_end - cpu_start) / 1e9 / wall_seconds
                         : 0.0;
    result.rss_bytes = process_rss_bytes();
    results.push_back(std::move(result));
    return true;
  };

  bool ok = true;
  if (mode == "all") {
    ok = run_one("parse") && run_one("pipeline") && run_one("persist");
  } else {
    ok = run_one(std::string(mode));
  }
  if (!ok) {
    std::cerr << "unknown benchmark mode; use parse, pipeline, persist or all\n";
    return 2;
  }
  std::ostringstream output;
  output << "{\"benchmark_type\":\"SYNTHETIC_BENCHMARK\",\"live_network\":false,"
            "\"interpretation\":\"not live network throughput\",\"events_per_mode\":"
         << events << ",\"modes\":[";
  for (std::size_t i = 0; i < results.size(); ++i) {
    if (i) output << ',';
    output << result_json(results[i]);
  }
  output << "]}\n";
  std::cout << output.str();
  if (mode == "all" || mode == "persist") {
    std::string error;
    const std::filesystem::path summary_path = output_dir / "synthetic_benchmark_summary.json";
    atomic_write_file(summary_path, output.str(), error);
  }
  for (const auto& result : results) {
    if (result.errors != 0) return 2;
  }
  return 0;
}

}  // namespace marketpipe
