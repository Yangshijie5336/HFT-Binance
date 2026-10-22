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

std::string check_label(const std::string& name) {
  static const std::map<std::string, std::string> labels = {
      {"summary_result_pass", "摘要结果为 PASS"},
      {"raw_frames_exist", "raw 文件存在"},
      {"raw_sha256_matches_summary", "raw SHA-256 与摘要一致"},
      {"raw_records_match_summary", "raw 记录数与摘要一致"},
      {"raw_payload_hashes_valid", "逐帧 payload 哈希有效"},
      {"collector_raw_integrity_verified", "采集器 raw 完整性已校验"},
      {"artifact_hashes_match", "产物哈希一致"},
  };
  const auto found = labels.find(name);
  return found == labels.end() ? name : found->second;
}

std::string render_report(const json::Value& summary, const std::map<std::string, std::string>& hashes,
                          const std::map<std::string, bool>& checks) {
  const std::string result = string_member(summary, "result", "UNKNOWN");
  std::ostringstream report;
  report << "# MarketPipe 实时行情性能与验收报告\n\n";
  report << "## 结果\n\n";
  report << "- **运行状态：** `" << result << "`\n";
  report << "- **交易对：** `" << string_member(summary, "symbol", "unknown") << "`\n";
  report << "- **数据源：** `" << string_member(summary, "websocket_url", "unknown") << "`\n";
  report << "- **运行 ID：** `" << string_member(summary, "run_id", "unknown") << "`\n";
  report << "- **开始 UTC：** `" << string_member(summary, "started_utc", "unknown") << "`\n";
  report << "- **结束 UTC：** `" << string_member(summary, "finished_utc", "unknown") << "`\n";
  report << "- **目标时长：** "
         << format_double(double_member(summary, "duration_target_seconds")) << " 秒\n";
  report << "- **实际采集时长：** "
         << format_double(double_member(summary, "duration_actual_seconds")) << " 秒\n";
  report << "- **处理/排空时长：** "
         << format_double(double_member(summary, "processing_elapsed_seconds")) << " 秒\n";
  report << "- **排空/最终确定时长：** "
         << format_double(double_member(summary, "drain_elapsed_seconds")) << " 秒\n";
  report << "- **失败原因：** `" << string_member(summary, "failure", "无") << "`\n";
  report << "- **是否被信号停止：** "
         << (bool_member(summary, "stopped_by_signal") ? "是" : "否") << "\n\n";

  report << "## 事件、连接与存储计数\n\n";
  report << "| 指标 | 数值 |\n|---|---:|\n";
  auto row = [&](const char* label, const std::string& value) {
    report << "| " << label << " | " << value << " |\n";
  };
  row("接收 WebSocket 帧", std::to_string(uint_member(summary, "frames_received")));
  row("落盘 raw 帧", std::to_string(uint_member(summary, "raw_frames_records")));
  row("raw 帧字节", std::to_string(uint_member(summary, "raw_frames_bytes")));
  row("看到成交事件", std::to_string(uint_member(summary, "trade_events_seen")));
  row("已处理唯一成交", std::to_string(uint_member(summary, "trades_processed")));
  row("发出信号", std::to_string(uint_member(summary, "signals_emitted")));
  row("重复成交", std::to_string(uint_member(summary, "duplicates")));
  row("乱序成交", std::to_string(uint_member(summary, "out_of_order")));
  row("异常帧", std::to_string(uint_member(summary, "malformed_frames")));
  row("订阅确认", std::to_string(uint_member(summary, "subscription_acks")));
  row("服务端错误帧", std::to_string(uint_member(summary, "server_errors")));
  row("断开次数", std::to_string(uint_member(summary, "disconnects")));
  row("重连尝试", std::to_string(uint_member(summary, "reconnects")));
  row("连接代次", std::to_string(uint_member(summary, "connection_generations")));
  row("队列峰值 / 容量",
      std::to_string(uint_member(summary, "queue_high_water")) + " / " +
          std::to_string(uint_member(summary, "queue_capacity")));
  row("环容量", std::to_string(uint_member(summary, "ring_capacity")));
  row("队列溢出", bool_member(summary, "queue_overflow") ? "是" : "否");
  row("丢弃事件", std::to_string(uint_member(summary, "dropped_events")));
  row("持久化错误", bool_member(summary, "persistence_error") ? "是" : "否");
  row("处理失败", std::to_string(uint_member(summary, "processing_failures")));
  row("吞吐", format_double(double_member(summary, "throughput_trades_per_second")) +
                   " 笔/秒");
  row("观测入口速率", format_double(double_member(summary, "ingress_trades_per_second")) +
                          " 笔事件/秒");
  row("raw 磁盘写入速率",
      format_double(double_member(summary, "raw_write_bytes_per_second")) + " 字节/秒");
  row("每分钟平均唯一成交",
      format_double(double_member(summary, "average_trades_per_minute")));
  row("每分钟峰值唯一成交", std::to_string(uint_member(summary, "max_trades_per_minute")));
  row("每秒峰值唯一成交", std::to_string(uint_member(summary, "max_trades_per_second")));
  row("进程 CPU 均值", format_double(double_member(summary, "cpu_percent_avg")) + " %");
  row("RSS 峰值", std::to_string(uint_member(summary, "rss_bytes_peak")) + " 字节");
  row("HTTP 状态", std::to_string(uint_member(summary, "http_status")));
  report << "\n## 延迟（采样；微秒）\n\n";
  report << "每 5 笔已处理事件采样一次，最多 200,000 条。测量的是本地接收→处理和"
            "接收→信号延迟，不是交易所到交易所延迟。\n\n";
  report << "| 阶段 | P50 | P95 | P99 | 样本数 |\n|---|---:|---:|---:|---:|\n";
  report << "| 队列等待 " << latency_cell(summary, "queue_wait_latency") << "\n";
  report << "| 接收→处理 " << latency_cell(summary, "receive_to_process_latency") << "\n";
  report << "| 接收→信号 " << latency_cell(summary, "receive_to_signal_latency") << "\n\n";

  const json::Value* gaps = member(summary, "gap_windows");
  report << "## 重连缺口记录\n\n";
  if (gaps && gaps->is_array() && !gaps->array.empty()) {
    report << "| 代次 | 近似缺口（秒） | 最后帧 UTC | 重连 UTC |\n|---:|---:|---|---|\n";
    for (const auto& gap : gaps->array) {
      report << "| " << uint_member(gap, "generation") << " | "
             << format_double(double_member(gap, "gap_seconds")) << " | "
             << string_member(gap, "disconnected_utc") << " | "
             << string_member(gap, "reconnected_utc") << " |\n";
    }
    report << "\n重连不能保证交易所行情没有缺失成交；上表记录的是潜在缺口，不能作为完整性证明。\n";
  } else {
    report << "未观察到重连缺口。\n";
  }

  report << "\n## 质量门禁\n\n";
  if (checks.empty()) {
    report << "- 无可用质量检查\n";
  } else {
    for (const auto& [name, value] : checks) {
      report << "- `" << name << "`（" << check_label(name) << "）：**"
             << (value ? "通过" : "失败") << "**\n";
    }
  }
  report << "\n## 产物完整性（SHA-256）\n\n";
  if (hashes.empty()) {
    report << "- 不可用\n";
  } else {
    for (const auto& [name, hash] : hashes) {
      report << "- `" << name << "`：`" << hash << "`\n";
    }
  }
  report << "\n## 持久化与真实性说明\n\n";
  report << "- 原始 WebSocket 帧以精确文本保存（仅做 JSON 转义），并记录 run_id、连接代次、"
            "接收 UTC/单调时间、序号、payload 字节数和 payload SHA-256。\n";
  report << "- raw 与派生文件批量 flush，并按 "
         << string_member(summary, "raw_fsync_policy", "配置的间隔") << " 及最终确定时 fsync；"
            "最终确定错误会如实上报，绝不忽略。\n";
  report << "- `PASS` 要求真实 WebSocket 连接、实际采样时长严格达到目标（排空时间不计入）、"
            "raw 帧已落盘、SHA-256 一致、零队列溢出、零持久化/处理失败，且无断开、异常帧、"
            "重复或乱序成交。\n";
  report << "- 合成基准永远不能作为实时网络吞吐证据。策略只输出标签，执行禁用。\n";
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
