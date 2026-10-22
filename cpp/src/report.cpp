#include "marketpipe.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "marketpipe/json.hpp"
#include "marketpipe/runtime.hpp"

namespace marketpipe {
namespace {

const json::Value* member(const json::Value& object, const char* key) {
  return object.is_object() ? object.find(key) : nullptr;
}

std::string string_member(const json::Value& object, const char* key, const std::string& fallback = "") {
  const json::Value* value = member(object, key);
  return value && value->is_string() ? value->text : fallback;
}

bool bool_member(const json::Value& object, const char* key, bool fallback = false) {
  const json::Value* value = member(object, key);
  return value && value->is_bool() ? value->boolean : fallback;
}

std::uint64_t uint_member(const json::Value& object, const char* key, std::uint64_t fallback = 0) {
  const json::Value* value = member(object, key);
  unsigned long long parsed = 0;
  if (value && json::to_uint64(*value, parsed)) return parsed;
  return fallback;
}

double double_member(const json::Value& object, const char* key, double fallback = 0.0) {
  const json::Value* value = member(object, key);
  double parsed = 0.0;
  if (value && json::to_double(*value, parsed)) return parsed;
  return fallback;
}

struct Check {
  std::string name;
  bool ok{false};
  std::string detail;
};

struct ArtifactCheck {
  std::string name;
  std::string expected;
  std::string actual;
  bool ok{false};
};

struct RawCheck {
  bool ok{true};
  std::uint64_t records{};
  std::uint64_t bytes{};
  std::string sha256;
  std::vector<std::string> errors;
};

RawCheck inspect_raw_frames(const std::filesystem::path& path) {
  RawCheck result;
  std::string error;
  if (!sha256_file(path, result.sha256, error)) {
    result.ok = false;
    result.errors.push_back(error);
    return result;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    result.ok = false;
    result.errors.push_back("cannot reopen raw_frames.jsonl");
    return result;
  }
  std::string line;
  std::uint64_t last_seq = 0;
  std::uint64_t last_generation = 0;
  std::uint64_t line_no = 0;
  while (std::getline(input, line)) {
    ++line_no;
    json::Value record;
    json::ParseError parse_error;
    if (line.empty() || !json::parse(line, record, parse_error) || !record.is_object()) {
      result.errors.push_back("invalid raw JSONL at line " + std::to_string(line_no));
      result.ok = false;
      continue;
    }
    const json::Value* payload = record.find("payload");
    const json::Value* payload_hash = record.find("payload_sha256");
    const json::Value* payload_bytes = record.find("payload_bytes");
    const json::Value* seq = record.find("frame_seq");
    const json::Value* generation = record.find("connection_generation");
    if (!payload || !payload->is_string() || !payload_hash || !payload_hash->is_string() ||
        !payload_bytes || !seq || !generation) {
      result.errors.push_back("raw record fields missing at line " + std::to_string(line_no));
      result.ok = false;
      continue;
    }
    if (sha256_hex(payload->text) != payload_hash->text) {
      result.errors.push_back("payload SHA-256 mismatch at line " + std::to_string(line_no));
      result.ok = false;
    }
    unsigned long long bytes = 0;
    if (!json::to_uint64(*payload_bytes, bytes) || bytes != payload->text.size()) {
      result.errors.push_back("payload_bytes mismatch at line " + std::to_string(line_no));
      result.ok = false;
    }
    unsigned long long seq_value = 0;
    unsigned long long generation_value = 0;
    if (!json::to_uint64(*seq, seq_value) || !json::to_uint64(*generation, generation_value)) {
      result.errors.push_back("invalid seq/generation at line " + std::to_string(line_no));
      result.ok = false;
      continue;
    }
    if (seq_value <= last_seq) {
      result.errors.push_back("frame_seq not strictly increasing at line " + std::to_string(line_no));
      result.ok = false;
    }
    if (generation_value < last_generation) {
      result.errors.push_back("generation went backwards at line " + std::to_string(line_no));
      result.ok = false;
    }
    last_seq = seq_value;
    last_generation = generation_value;
    result.bytes += line.size() + 1;
    ++result.records;
  }
  return result;
}

std::string latency_cell(const json::Value& summary, const char* key) {
  const json::Value* latency = member(summary, key);
  if (!latency || !latency->is_object()) return "| N/A | N/A | N/A | 0 |";
  const std::uint64_t samples = uint_member(*latency, "samples");
  if (samples == 0) return "| N/A | N/A | N/A | 0 |";
  std::ostringstream out;
  out << "| " << format_double(double_member(*latency, "p50_us")) << " | "
      << format_double(double_member(*latency, "p95_us")) << " | "
      << format_double(double_member(*latency, "p99_us")) << " | " << samples << " |";
  return out.str();
}

std::string render_report(const json::Value& summary, const std::map<std::string, std::string>& hashes,
                          const std::map<std::string, bool>& checks) {
  const std::string result = string_member(summary, "result", "UNKNOWN");
  std::ostringstream report;
  report << "# MarketPipe live-feed performance and acceptance report\n\n";
  report << "## Result\n\n";
  report << "- **Run status:** `" << result << "`\n";
  report << "- **Symbol:** `" << string_member(summary, "symbol", "unknown") << "`\n";
  report << "- **Source:** `" << string_member(summary, "websocket_url", "unknown") << "`\n";
  report << "- **Run id:** `" << string_member(summary, "run_id", "unknown") << "`\n";
  report << "- **Start UTC:** `" << string_member(summary, "started_utc", "unknown") << "`\n";
  report << "- **Finish UTC:** `" << string_member(summary, "finished_utc", "unknown") << "`\n";
  report << "- **Target duration:** " << format_double(double_member(summary, "duration_target_seconds"))
         << " s\n";
  report << "- **Observed capture duration:** "
         << format_double(double_member(summary, "duration_actual_seconds")) << " s\n";
  report << "- **Processing/drain duration:** "
         << format_double(double_member(summary, "processing_elapsed_seconds")) << " s\n";
  report << "- **Failure:** `" << string_member(summary, "failure", "none") << "`\n";
  report << "- **Stopped by signal:** "
         << (bool_member(summary, "stopped_by_signal") ? "yes" : "no") << "\n\n";

  report << "## Event, connection and storage counters\n\n";
  report << "| Metric | Value |\n|---|---:|\n";
  auto row = [&](const char* label, const std::string& value) {
    report << "| " << label << " | " << value << " |\n";
  };
  row("WebSocket frames received", std::to_string(uint_member(summary, "frames_received")));
  row("Raw frames journaled", std::to_string(uint_member(summary, "raw_frames_records")));
  row("Raw frame bytes", std::to_string(uint_member(summary, "raw_frames_bytes")));
  row("Trade events seen", std::to_string(uint_member(summary, "trade_events_seen")));
  row("Unique trades processed", std::to_string(uint_member(summary, "trades_processed")));
  row("Signals emitted", std::to_string(uint_member(summary, "signals_emitted")));
  row("Duplicate trades", std::to_string(uint_member(summary, "duplicates")));
  row("Out-of-order trades", std::to_string(uint_member(summary, "out_of_order")));
  row("Malformed frames", std::to_string(uint_member(summary, "malformed_frames")));
  row("Subscription acknowledgements", std::to_string(uint_member(summary, "subscription_acks")));
  row("Server error frames", std::to_string(uint_member(summary, "server_errors")));
  row("Disconnects", std::to_string(uint_member(summary, "disconnects")));
  row("Reconnect attempts", std::to_string(uint_member(summary, "reconnects")));
  row("Connection generations", std::to_string(uint_member(summary, "connection_generations")));
  row("Queue high-water / capacity",
      std::to_string(uint_member(summary, "queue_high_water")) + " / " +
          std::to_string(uint_member(summary, "queue_capacity")));
  row("Ring capacity", std::to_string(uint_member(summary, "ring_capacity")));
  row("Queue overflow", bool_member(summary, "queue_overflow") ? "yes" : "no");
  row("Dropped events", std::to_string(uint_member(summary, "dropped_events")));
  row("Persistence error", bool_member(summary, "persistence_error") ? "yes" : "no");
  row("Processing failures", std::to_string(uint_member(summary, "processing_failures")));
  row("Throughput", format_double(double_member(summary, "throughput_trades_per_second")) +
                        " trades/s");
  row("Observed ingress", format_double(double_member(summary, "ingress_trades_per_second")) +
                              " trade events/s");
  row("Raw disk write rate",
      format_double(double_member(summary, "raw_write_bytes_per_second")) + " bytes/s");
  row("Average unique trades/minute",
      format_double(double_member(summary, "average_trades_per_minute")));
  row("Peak unique trades/minute", std::to_string(uint_member(summary, "max_trades_per_minute")));
  row("Peak unique trades/second", std::to_string(uint_member(summary, "max_trades_per_second")));
  row("Average sampled CPU", format_double(double_member(summary, "cpu_percent_avg")) + " %");
  row("Peak sampled RSS", std::to_string(uint_member(summary, "rss_bytes_peak")) + " bytes");
  row("HTTP status", std::to_string(uint_member(summary, "http_status")));
  report << "\n## Latency (sampled; microseconds)\n\n";
  report << "Samples are taken every fifth processed event, capped at 200,000. They measure local "
            "receive-to-process and receive-to-signal latency, not exchange-to-exchange latency.\n\n";
  report << "| Stage | p50 | p95 | p99 | Samples |\n|---|---:|---:|---:|---:|\n";
  report << "| Queue wait " << latency_cell(summary, "queue_wait_latency") << "\n";
  report << "| Receive to process " << latency_cell(summary, "receive_to_process_latency") << "\n";
  report << "| Receive to signal " << latency_cell(summary, "receive_to_signal_latency") << "\n\n";

  const json::Value* gaps = member(summary, "gap_windows");
  report << "## Reconnect gap record\n\n";
  if (gaps && gaps->is_array() && !gaps->array.empty()) {
    report << "| Generation | Approx. gap (s) | Last frame UTC | Reconnected UTC |\n|---:|---:|---|---|\n";
    for (const auto& gap : gaps->array) {
      report << "| " << uint_member(gap, "generation") << " | "
             << format_double(double_member(gap, "gap_seconds")) << " | "
             << string_member(gap, "disconnected_utc") << " | "
             << string_member(gap, "reconnected_utc") << " |\n";
    }
    report << "\nA reconnect cannot guarantee that the exchange feed had no missing trades; the "
              "window above is recorded as a potential gap, not evidence of completeness.\n";
  } else {
    report << "No reconnect gap was observed.\n";
  }

  report << "\n## Quality gates\n\n";
  if (checks.empty()) {
    report << "- No quality checks available\n";
  } else {
    for (const auto& [name, value] : checks) {
      report << "- `" << name << "`: **" << (value ? "PASS" : "FAIL") << "**\n";
    }
  }
  report << "\n## Artifact integrity (SHA-256)\n\n";
  if (hashes.empty()) {
    report << "- Not available\n";
  } else {
    for (const auto& [name, hash] : hashes) {
      report << "- `" << name << "`: `" << hash << "`\n";
    }
  }
  report << "\n## Persistence and honest interpretation\n\n";
  report << "- Raw WebSocket frames are stored as exact text (base JSON escaping only) with "
            "run_id, connection generation, receive UTC/monotonic timestamps, sequence, payload "
            "bytes and payload SHA-256.\n";
  report << "- Raw and derived files are flushed in batches and fsynced every "
         << string_member(summary, "raw_fsync_policy", "configured interval") <<
                " and on finalization; finalization errors are reported, never ignored.\n";
  report << "- A `PASS` requires a real WebSocket connection, at least 99% of the requested "
            "duration, persisted raw frames, matching SHA-256, zero queue overflow, zero "
            "persistence/processing failures, and no disconnects, malformed or duplicate trades.\n";
  report << "- A synthetic benchmark is never evidence of live network throughput. The strategy "
            "emits labels only; execution is disabled.\n";
  return report.str();
}

}  // namespace

int run_report(const std::filesystem::path& run_dir, bool write_files, std::string& output_json) {
  const auto summary_path = run_dir / "summary.json";
  std::ifstream summary_stream(summary_path, std::ios::binary);
  if (!summary_stream) {
    output_json = "{\"verified\":false,\"error\":\"cannot open summary.json\"}\n";
    return 2;
  }
  std::string summary_text((std::istreambuf_iterator<char>(summary_stream)),
                           std::istreambuf_iterator<char>());
  json::Value summary;
  json::ParseError parse_error;
  if (!json::parse(summary_text, summary, parse_error) || !summary.is_object()) {
    output_json = "{\"verified\":false,\"error\":\"invalid summary.json\"}\n";
    return 2;
  }

  std::vector<Check> checks;
  std::vector<ArtifactCheck> artifacts;
  std::vector<std::string> errors;

  const std::string summary_result = string_member(summary, "result", "UNKNOWN");
  const bool pass_result = summary_result == "PASS";
  const bool partial_result = summary_result == "PARTIAL";

  // Artifact hashes recorded by the collector.
  const json::Value* recorded_hashes = member(summary, "artifact_sha256");
  if (recorded_hashes && recorded_hashes->is_object()) {
    for (const auto& [name, value] : recorded_hashes->object) {
      if (!value.is_string()) continue;
      ArtifactCheck check;
      check.name = name;
      check.expected = value.text;
      std::string hash_error;
      if (sha256_file(run_dir / name, check.actual, hash_error)) {
        check.ok = check.actual == check.expected;
      } else {
        check.ok = false;
        check.actual = "ERROR: " + hash_error;
      }
      if (!check.ok) errors.push_back("artifact SHA-256 mismatch: " + name);
      artifacts.push_back(std::move(check));
    }
  } else {
    errors.push_back("summary.json has no artifact_sha256 map");
  }

  const auto raw_path = run_dir / "raw_frames.jsonl";
  const RawCheck raw = inspect_raw_frames(raw_path);
  const std::string raw_sha = string_member(summary, "raw_frames_sha256");
  const bool raw_sha_ok = !raw.sha256.empty() && raw.sha256 == raw_sha;
  const bool raw_records_ok =
      raw.records == uint_member(summary, "raw_frames_records") &&
      raw.records == uint_member(summary, "frames_received");
  const bool raw_verified = bool_member(summary, "raw_integrity_verified");
  if (!raw.ok) {
    for (const auto& error : raw.errors) errors.push_back("raw_frames.jsonl: " + error);
  }
  if (!raw_sha_ok) errors.push_back("raw_frames.jsonl SHA-256 does not match summary");
  if (!raw_records_ok) errors.push_back("raw record count does not match summary counters");
  if (!raw_verified) errors.push_back("collector did not verify raw integrity");

  for (const char* name : {"raw_frames.jsonl", "normalized_events.jsonl", "factors.jsonl",
                           "signals.jsonl", "system.log"}) {
    std::error_code ec;
    if (!std::filesystem::exists(run_dir / name, ec)) {
      errors.push_back(std::string("missing artifact: ") + name);
    }
  }

  checks.push_back({"summary_result_pass", pass_result, summary_result});
  checks.push_back({"raw_frames_exist", std::filesystem::exists(raw_path), {}});
  checks.push_back({"raw_sha256_matches_summary", raw_sha_ok, raw.sha256});
  checks.push_back({"raw_records_match_summary", raw_records_ok,
                    std::to_string(raw.records)});
  checks.push_back({"raw_payload_hashes_valid", raw.ok, {}});
  checks.push_back({"collector_raw_integrity_verified", raw_verified, {}});
  bool all_artifacts_ok = !artifacts.empty();
  for (const auto& artifact : artifacts) {
    all_artifacts_ok = all_artifacts_ok && artifact.ok;
  }
  checks.push_back({"artifact_hashes_match", all_artifacts_ok, {}});

  bool verified = pass_result;
  for (const auto& check : checks) verified = verified && check.ok;
  verified = verified && errors.empty();

  std::ostringstream json;
  json << "{\n  \"run_dir\":\"" << json::escape(run_dir.string()) << "\",\n";
  json << "  \"summary_result\":\"" << summary_result << "\",\n";
  json << "  \"verified\":" << (verified ? "true" : "false") << ",\n";
  json << "  \"raw_records_computed\":" << raw.records << ",\n";
  json << "  \"raw_bytes_computed\":" << raw.bytes << ",\n";
  json << "  \"raw_sha256_computed\":\"" << raw.sha256 << "\",\n";
  json << "  \"checks\":{";
  for (std::size_t i = 0; i < checks.size(); ++i) {
    if (i) json << ',';
    json << "\"" << json::escape(checks[i].name) << "\":" << (checks[i].ok ? "true" : "false");
  }
  json << "},\n  \"artifacts\":[";
  for (std::size_t i = 0; i < artifacts.size(); ++i) {
    if (i) json << ',';
    json << "{\"name\":\"" << json::escape(artifacts[i].name) << "\",\"ok\":"
         << (artifacts[i].ok ? "true" : "false") << ",\"expected\":\""
         << artifacts[i].expected << "\",\"actual\":\"" << artifacts[i].actual << "\"}";
  }
  json << "],\n  \"errors\":[";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i) json << ',';
    json << "\"" << json::escape(errors[i]) << "\"";
  }
  json << "]\n}\n";
  output_json = json.str();

  if (write_files) {
    std::string write_error;
    if (!atomic_write_file(run_dir / "verification.json", output_json, write_error)) {
      std::cerr << "cannot write verification.json: " << write_error << '\n';
    }
    std::map<std::string, std::string> hashes;
    for (const auto& artifact : artifacts) hashes[artifact.name] = artifact.actual;
    std::map<std::string, bool> check_map;
    for (const auto& check : checks) check_map[check.name] = check.ok;
    const std::string report = render_report(summary, hashes, check_map);
    if (!atomic_write_file(run_dir / "performance_report.md", report, write_error)) {
      std::cerr << "cannot write performance_report.md: " << write_error << '\n';
    }
  }

  if (verified) return 0;
  if (partial_result && errors.empty()) return 3;
  return 2;
}

}  // namespace marketpipe
