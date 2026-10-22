#include "marketpipe.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "marketpipe/json.hpp"
#include "marketpipe/runtime.hpp"

namespace marketpipe {
namespace {

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int) { g_stop = 1; }

bool positive_decimal(std::string_view text, double& value) {
  if (text.empty() || text.size() > 64) return false;
  double parsed = 0.0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) return false;
  if (!std::isfinite(parsed) || parsed <= 0.0) return false;
  value = parsed;
  return true;
}

bool numeric_text(const json::Value* node, std::string& text) {
  if (!node) return false;
  if (node->is_string() || node->is_number()) {
    text = node->text;
    return !text.empty();
  }
  return false;
}

bool timestamp_ms(const json::Value* node, std::int64_t& epoch_ms) {
  if (!node) return false;
  long long value = 0;
  if (node->is_number()) {
    if (!json::to_int64(*node, value)) return false;
  } else if (node->is_string()) {
    const char* begin = node->text.data();
    const char* end = begin + node->text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) return false;
  } else {
    return false;
  }
  if (value <= 0) return false;
  epoch_ms = value;
  return true;
}

bool parse_trade_node(const json::Value& node, const ParseContext& context, Trade& trade,
                      std::string& detail) {
  if (!node.is_object()) {
    detail = "trade payload is not an object";
    return false;
  }
  const json::Value* type = node.find("e");
  if (!type || !type->is_string() || type->text != "trade") {
    detail = "unsupported event type";
    return false;
  }
  const json::Value* symbol = node.find("s");
  const json::Value* id = node.find("t");
  const json::Value* price = node.find("p");
  const json::Value* qty = node.find("q");
  const json::Value* event_time = node.find("T");
  const json::Value* maker = node.find("m");
  if (!symbol || !symbol->is_string() || !id || !price || !qty || !event_time || !maker) {
    detail = "missing required Binance trade fields";
    return false;
  }
  if (!context.expected_symbol.empty() && symbol->text != context.expected_symbol) {
    detail = "trade symbol mismatch: " + symbol->text;
    return false;
  }
  if (!maker->is_bool()) {
    detail = "Binance 'm' field must be boolean";
    return false;
  }
  if (!numeric_text(id, trade.id) || !numeric_text(price, trade.price_text) ||
      !numeric_text(qty, trade.qty_text)) {
    detail = "trade id/price/qty must be strings or numbers";
    return false;
  }
  double price_value = 0.0;
  double qty_value = 0.0;
  if (!positive_decimal(trade.price_text, price_value)) {
    detail = "invalid positive price text";
    return false;
  }
  if (!positive_decimal(trade.qty_text, qty_value)) {
    detail = "invalid positive quantity text";
    return false;
  }
  std::int64_t epoch_ms = 0;
  if (!timestamp_ms(event_time, epoch_ms)) {
    detail = "invalid exchange event time";
    return false;
  }
  trade.symbol = symbol->text;
  trade.side = maker->boolean ? "sell" : "buy";
  trade.price = price_value;
  trade.qty = qty_value;
  trade.exchange_ts_ns = epoch_ms * 1000000LL;
  trade.exchange_time = utc_iso_from_ms(epoch_ms);
  trade.frame_seq = context.frame_seq;
  trade.received_utc = context.received_utc;
  trade.received_steady_ns = context.received_steady_ns;
  trade.connection_generation = context.connection_generation;
  trade.connection_id = context.connection_id;
  trade.source_type = "trade";
  return true;
}

bool looks_like_trade(const json::Value& node) {
  if (!node.is_object()) return false;
  const json::Value* type = node.find("e");
  return type && type->is_string() && type->text == "trade";
}

}  // namespace

void install_signal_handlers() {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
}

void request_stop() { g_stop = 1; }
void reset_stop() { g_stop = 0; }
bool stop_requested() { return g_stop != 0; }

ParsedMessage parse_binance_message(std::string_view json_text, const ParseContext& context) {
  ParsedMessage result;
  if (json_text.empty()) {
    result.status = ParseStatus::Malformed;
    result.detail = "empty frame";
    return result;
  }
  json::Value root;
  json::ParseError error;
  if (!json::parse(json_text, root, error)) {
    result.status = ParseStatus::Malformed;
    result.detail = "JSON parse error at offset " + std::to_string(error.offset) + ": " +
                    error.message;
    return result;
  }

  const json::Value* candidate = &root;
  if (root.is_object()) {
    const json::Value* data = root.find("data");
    if (data && (data->is_object() || data->is_array())) candidate = data;
    const json::Value* id = root.find("id");
    const json::Value* status = root.find("result");
    if (id && status && status->is_null() && !looks_like_trade(root)) {
      result.status = ParseStatus::SubscriptionAck;
      result.detail = "subscription acknowledged";
      return result;
    }
    const json::Value* event = root.find("e");
    if (event && event->is_string() && event->text == "error") {
      result.status = ParseStatus::ServerError;
      result.detail = "server error frame";
      return result;
    }
    const json::Value* success = root.find("success");
    if (success && success->is_bool() && !success->boolean) {
      result.status = ParseStatus::ServerError;
      result.detail = "server reported success=false";
      return result;
    }
  }

  auto parse_one = [&](const json::Value& node) {
    if (!node.is_object()) {
      result.detail = "frame element is not an object";
      return;
    }
    if (!looks_like_trade(node)) {
      const json::Value* type = node.find("e");
      if (type && type->is_string()) {
        result.detail = "non-trade event: " + type->text;
        if (result.status != ParseStatus::SymbolMismatch) {
          result.status = ParseStatus::NotTrade;
        }
      }
      return;
    }
    const json::Value* symbol = node.find("s");
    if (symbol && symbol->is_string() && !context.expected_symbol.empty() &&
        symbol->text != context.expected_symbol) {
      result.status = ParseStatus::SymbolMismatch;
      result.detail = "trade symbol mismatch: " + symbol->text;
      return;
    }
    Trade trade;
    std::string detail;
    if (!parse_trade_node(node, context, trade, detail)) {
      // A malformed trade-level payload is a malformed frame, not a silent skip.
      result.status = ParseStatus::Malformed;
      result.detail = detail;
      return;
    }
    result.trades.push_back(std::move(trade));
  };

  if (candidate->is_array()) {
    for (const auto& element : candidate->array) parse_one(element);
  } else {
    parse_one(*candidate);
  }

  if (!result.trades.empty()) {
    result.status = ParseStatus::Ok;
    return result;
  }
  if (result.status == ParseStatus::Malformed || result.status == ParseStatus::SymbolMismatch) {
    return result;
  }
  if (result.status == ParseStatus::NotTrade) return result;
  const json::Value* type = root.find("e");
  if (type && type->is_string() && type->text != "trade") {
    result.status = ParseStatus::NotTrade;
    result.detail = "non-trade event: " + type->text;
  } else {
    result.status = ParseStatus::Malformed;
    if (result.detail.empty()) result.detail = "frame did not contain a trade";
  }
  return result;
}

std::optional<Trade> parse_binance_trade(std::string_view json_text, std::uint64_t frame_seq) {
  ParseContext context;
  context.frame_seq = frame_seq;
  context.received_utc = utc_now_iso();
  context.received_steady_ns = steady_ns();
  ParsedMessage parsed = parse_binance_message(json_text, context);
  if (parsed.status != ParseStatus::Ok || parsed.trades.size() != 1) return std::nullopt;
  return parsed.trades.front();
}

FactorEngine::FactorEngine(double window_seconds, std::size_t return_lookback)
    : window_ns_(static_cast<std::int64_t>(window_seconds * 1e9)),
      return_lookback_(return_lookback) {}

Factors FactorEngine::update(const Trade& trade) {
  std::int64_t event_ts = trade.received_steady_ns;
  if (event_ts <= 0) event_ts = trade.exchange_ts_ns;
  if (event_ts <= last_event_ts_ns_) event_ts = last_event_ts_ns_ + 1;
  last_event_ts_ns_ = event_ts;

  window_.push_back(WindowEntry{event_ts, trade});
  if (trade.side == "buy") {
    buy_qty_ += trade.qty;
  } else {
    sell_qty_ += trade.qty;
  }
  const std::int64_t cutoff = event_ts - window_ns_;
  while (!window_.empty() && window_.front().ts_ns < cutoff) {
    const Trade& expired = window_.front().trade;
    if (expired.side == "buy") {
      buy_qty_ -= expired.qty;
    } else {
      sell_qty_ -= expired.qty;
    }
    window_.pop_front();
  }
  if (buy_qty_ < 0 && buy_qty_ > -1e-9) buy_qty_ = 0;
  if (sell_qty_ < 0 && sell_qty_ > -1e-9) sell_qty_ = 0;

  ten_second_times_.push_back(event_ts);
  const std::int64_t cutoff_10s = event_ts - 10000000000LL;
  while (!ten_second_times_.empty() && ten_second_times_.front() < cutoff_10s) {
    ten_second_times_.pop_front();
  }

  prices_.push_back(trade.price);
  while (prices_.size() > return_lookback_ + 1) prices_.pop_front();

  const double total_qty = buy_qty_ + sell_qty_;
  Factors factors;
  factors.symbol = trade.symbol;
  factors.trade_id = trade.id;
  factors.price = trade.price;
  factors.qty = trade.qty;
  factors.exchange_ts_ns = trade.exchange_ts_ns;
  factors.received_ns = trade.received_steady_ns;
  factors.out_of_order = trade.out_of_order;
  factors.window_trades = window_.size();
  factors.imbalance = total_qty > 0 ? (buy_qty_ - sell_qty_) / total_qty : 0.0;
  factors.return_n =
      prices_.size() >= 2 && prices_.front() != 0 ? prices_.back() / prices_.front() - 1.0 : 0.0;
  factors.rate_10s = static_cast<double>(ten_second_times_.size()) / 10.0;
  factors.average_size = window_.empty() ? 0.0 : total_qty / static_cast<double>(window_.size());
  return factors;
}

Signal evaluate(const Factors& factors) {
  Signal signal;
  signal.symbol = factors.symbol;
  signal.trade_id = factors.trade_id;
  signal.price = factors.price;
  signal.created_steady_ns = steady_ns();
  signal.out_of_order = factors.out_of_order;
  const double score = std::clamp(factors.imbalance * 0.7 + factors.return_n * 1000.0 * 0.3, -1.0,
                                  1.0);
  if (factors.imbalance >= 0.15 && factors.return_n >= 0.0001) {
    signal.action = "BUY_SIGNAL";
    signal.reason = "positive_trade_imbalance_and_momentum";
  } else if (factors.imbalance <= -0.15 && factors.return_n <= -0.0001) {
    signal.action = "SELL_SIGNAL";
    signal.reason = "negative_trade_imbalance_and_momentum";
  } else {
    signal.action = "HOLD";
    signal.reason = "thresholds_not_aligned";
  }
  signal.score = score;
  return signal;
}

Journal::Journal(const std::filesystem::path& path) {
  std::string error;
  if (!ensure_directory(path.parent_path(), error)) {
    error_ = error;
    return;
  }
  file_ = std::fopen(path.string().c_str(), "ab");
  if (!file_) error_ = "cannot open journal: " + path.string();
}

Journal::~Journal() { close(); }

void Journal::append(const std::string& payload, std::uint64_t seq, const std::string& run_id,
                     std::uint64_t generation, const std::string& received_utc) {
  if (!file_) {
    if (error_.empty()) error_ = "journal is not open";
    return;
  }
  const std::int64_t now = steady_ns();
  std::ostringstream line;
  line << "{\"run_id\":\"" << json::escape(run_id) << "\",\"frame_seq\":" << seq
       << ",\"connection_generation\":" << generation << ",\"received_at_utc\":\""
       << json::escape(received_utc) << "\",\"received_monotonic_ns\":" << now
       << ",\"payload_sha256\":\"" << sha256_hex(payload) << "\",\"payload_bytes\":"
       << payload.size() << ",\"payload\":\"" << json::escape(payload) << "\"}\n";
  const std::string text = line.str();
  if (std::fwrite(text.data(), 1, text.size(), file_) != text.size()) {
    error_ = "journal write failed";
    return;
  }
  if (std::fflush(file_) != 0) {
    error_ = "journal flush failed";
    return;
  }
  ++records_;
}

void Journal::close() {
  if (!file_) return;
  std::fflush(file_);
#ifdef _WIN32
  _commit(_fileno(file_));
#else
  ::fsync(::fileno(file_));
#endif
  std::fclose(file_);
  file_ = nullptr;
}

namespace {

struct ReplayEnvelope {
  std::string payload;
  std::string received_utc;
  std::string connection_id;
  std::uint64_t connection_generation{1};
  std::uint64_t frame_seq{};
  std::int64_t received_ns{};
};

std::string lowercase_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

// Validates the collector's raw JSONL envelope before any business replay.
// A modified payload, bad hash, missing timestamp or reordered metadata is a
// hard integrity failure, not a frame to skip.
bool parse_replay_envelope(const std::string& line, std::uint64_t line_no,
                           ReplayEnvelope& envelope, std::string& error) {
  json::Value record;
  json::ParseError parse_error;
  if (!json::parse(line, record, parse_error) || !record.is_object()) {
    error = "line " + std::to_string(line_no) + ": invalid raw JSON object";
    return false;
  }
  const json::Value* record_type = record.find("record_type");
  if (record_type && (!record_type->is_string() ||
                      record_type->text != "raw_websocket_frame")) {
    error = "line " + std::to_string(line_no) + ": unsupported record_type";
    return false;
  }
  const json::Value* payload = record.find("payload");
  const json::Value* payload_hash = record.find("payload_sha256");
  const json::Value* payload_bytes = record.find("payload_bytes");
  const json::Value* seq = record.find("frame_seq");
  const json::Value* generation = record.find("connection_generation");
  const json::Value* received = record.find("received_monotonic_ns");
  if (!received) received = record.find("received_ns");
  const json::Value* received_utc = record.find("received_utc");
  if (!payload || !payload->is_string() || !payload_hash || !payload_hash->is_string() ||
      !seq || !generation || !received || !received_utc || !received_utc->is_string()) {
    error = "line " + std::to_string(line_no) + ": missing raw replay metadata";
    return false;
  }
  if (payload->text.empty()) {
    error = "line " + std::to_string(line_no) + ": empty payload";
    return false;
  }
  const std::string actual_hash = sha256_hex(payload->text);
  if (lowercase_ascii(payload_hash->text) != lowercase_ascii(actual_hash)) {
    error = "line " + std::to_string(line_no) + ": payload SHA-256 mismatch";
    return false;
  }
  if (payload_bytes) {
    unsigned long long declared_bytes = 0;
    if (!json::to_uint64(*payload_bytes, declared_bytes) ||
        declared_bytes != payload->text.size()) {
      error = "line " + std::to_string(line_no) + ": payload_bytes mismatch";
      return false;
    }
  }
  unsigned long long seq_value = 0;
  unsigned long long generation_value = 0;
  long long received_value = 0;
  if (!json::to_uint64(*seq, seq_value) || seq_value == 0) {
    error = "line " + std::to_string(line_no) + ": invalid frame_seq";
    return false;
  }
  if (!json::to_uint64(*generation, generation_value) || generation_value == 0) {
    error = "line " + std::to_string(line_no) + ": invalid connection_generation";
    return false;
  }
  if (!json::to_int64(*received, received_value) || received_value <= 0) {
    error = "line " + std::to_string(line_no) + ": invalid received_monotonic_ns";
    return false;
  }
  const json::Value* connection_id = record.find("connection_id");
  envelope.payload = payload->text;
  envelope.received_utc = received_utc->text;
  envelope.connection_id =
      connection_id && connection_id->is_string() ? connection_id->text : "";
  envelope.connection_generation = generation_value;
  envelope.frame_seq = seq_value;
  envelope.received_ns = received_value;
  return true;
}

}  // namespace

int replay(const std::filesystem::path& input, const std::filesystem::path& output) {
  std::ifstream input_check(input, std::ios::binary);
  if (!input_check) {
    std::cerr << "cannot open input: " << input.string() << '\n';
    return 2;
  }
  input_check.close();
  std::string error;
  if (!ensure_directory(output, error)) {
    std::cerr << error << '\n';
    return 2;
  }
  for (const char* name : {"normalized_events.jsonl", "factors.jsonl", "signals.jsonl",
                           "replay_summary.json"}) {
    std::error_code remove_error;
    std::filesystem::remove(output / name, remove_error);
  }
  std::string input_sha;
  std::string hash_error;
  if (!sha256_file(input, input_sha, hash_error)) {
    std::cerr << hash_error << '\n';
    return 2;
  }

  // Pass 1: validate envelope, SHA-256, sequence and saved receive timing.
  std::ifstream validate_input(input, std::ios::binary);
  std::uint64_t records_read = 0;
  std::uint64_t integrity_errors = 0;
  std::uint64_t frames_validated = 0;
  std::uint64_t last_seq = 0;
  std::uint64_t last_generation = 0;
  std::int64_t last_received_ns = 0;
  std::string first_error;
  std::string line;
  while (std::getline(validate_input, line)) {
    ++records_read;
    if (line.empty()) {
      ++integrity_errors;
      if (first_error.empty()) {
        first_error = "line " + std::to_string(records_read) + ": blank line";
      }
      continue;
    }
    ReplayEnvelope envelope;
    std::string record_error;
    if (!parse_replay_envelope(line, records_read, envelope, record_error)) {
      ++integrity_errors;
      if (first_error.empty()) first_error = record_error;
      continue;
    }
    if (envelope.frame_seq <= last_seq) {
      ++integrity_errors;
      if (first_error.empty()) {
        first_error = "line " + std::to_string(records_read) +
                      ": frame_seq is not strictly increasing";
      }
      continue;
    }
    if (envelope.connection_generation < last_generation) {
      ++integrity_errors;
      if (first_error.empty()) {
        first_error = "line " + std::to_string(records_read) +
                      ": connection_generation went backwards";
      }
      continue;
    }
    if (envelope.received_ns < last_received_ns) {
      ++integrity_errors;
      if (first_error.empty()) {
        first_error = "line " + std::to_string(records_read) +
                      ": received_monotonic_ns went backwards";
      }
      continue;
    }
    last_seq = envelope.frame_seq;
    last_generation = envelope.connection_generation;
    last_received_ns = envelope.received_ns;
    ++frames_validated;
  }

  if (integrity_errors > 0) {
    std::ostringstream summary;
    summary << "{\"result\":\"FAIL\",\"replay\":true,\"input\":\""
            << json::escape(input.string()) << "\",\"input_sha256\":\"" << input_sha
            << "\",\"records_read\":" << records_read
            << ",\"frames_validated\":" << frames_validated
            << ",\"integrity_errors\":" << integrity_errors
            << ",\"trades_processed\":0,\"duplicates\":0,\"out_of_order\":0"
            << ",\"malformed_frames\":0,\"first_error\":\"" << json::escape(first_error)
            << "\",\"exit_code\":2,\"execution_enabled\":false}\n";
    if (!atomic_write_file(output / "replay_summary.json", summary.str(), error)) {
      std::cerr << error << '\n';
      return 2;
    }
    std::cerr << "replay rejected corrupt raw input: " << first_error << '\n';
    std::cout << summary.str();
    return 2;
  }

  // Pass 2: replay with the exact saved receive clock and frame metadata.
  std::ifstream in(input, std::ios::binary);
  JsonlFileWriter events(output / "normalized_events.jsonl", 100);
  JsonlFileWriter factors_out(output / "factors.jsonl", 100);
  JsonlFileWriter signals_out(output / "signals.jsonl", 100);
  if (!events.open(error) || !factors_out.open(error) || !signals_out.open(error)) {
    std::cerr << error << '\n';
    return 2;
  }

  FactorEngine engine;
  TradeDeduplicator dedup;
  std::uint64_t valid = 0;
  std::uint64_t malformed = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t out_of_order = 0;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    ReplayEnvelope envelope;
    std::string record_error;
    // Pass 1 already validated this record; a failure here is an I/O anomaly.
    if (!parse_replay_envelope(line, records_read, envelope, record_error)) {
      std::cerr << "replay input changed between validation and replay: " << record_error
                << '\n';
      return 2;
    }
    ParseContext context;
    context.expected_symbol = "";
    context.connection_generation = envelope.connection_generation;
    context.connection_id = envelope.connection_id;
    context.frame_seq = envelope.frame_seq;
    context.received_utc = envelope.received_utc;
    context.received_steady_ns = envelope.received_ns;
    ParsedMessage parsed = parse_binance_message(envelope.payload, context);
    if (parsed.status == ParseStatus::Malformed) {
      ++malformed;
      continue;
    }
    for (Trade trade : parsed.trades) {
      DedupResult dedup_result = dedup.classify(trade);
      if (dedup_result == DedupResult::Duplicate) {
        ++duplicates;
        continue;
      }
      if (dedup_result == DedupResult::OutOfOrder) {
        ++out_of_order;
        trade.out_of_order = true;
      }
      ++valid;
      const Factors factors = engine.update(trade);
      const Signal signal = evaluate(factors);
      std::ostringstream event_line;
      event_line << "{\"stage\":\"normalized_event\",\"symbol\":\""
                 << json::escape(trade.symbol) << "\",\"trade_id\":\""
                 << json::escape(trade.id) << "\",\"price_text\":\""
                 << json::escape(trade.price_text) << "\",\"qty_text\":\""
                 << json::escape(trade.qty_text) << "\",\"price\":" << format_double(trade.price)
                 << ",\"qty\":" << format_double(trade.qty) << ",\"exchange_ts_ns\":"
                 << trade.exchange_ts_ns << ",\"frame_seq\":" << trade.frame_seq
                 << ",\"connection_generation\":" << trade.connection_generation
                 << ",\"received_ns\":" << trade.received_steady_ns
                 << ",\"out_of_order\":" << (trade.out_of_order ? "true" : "false") << '}';
      std::ostringstream factor_line;
      factor_line << "{\"stage\":\"factor_snapshot\",\"symbol\":\""
                  << json::escape(factors.symbol) << "\",\"trade_id\":\""
                  << json::escape(factors.trade_id) << "\",\"trade_imbalance\":"
                  << format_double(factors.imbalance)
                  << ",\"return_n\":" << format_double(factors.return_n)
                  << ",\"trade_rate_10s\":" << format_double(factors.rate_10s)
                  << ",\"avg_trade_size\":" << format_double(factors.average_size)
                  << ",\"window_trades\":" << factors.window_trades
                  << ",\"received_ns\":" << factors.received_ns
                  << ",\"out_of_order\":" << (factors.out_of_order ? "true" : "false")
                  << '}';
      std::ostringstream signal_line;
      signal_line << "{\"symbol\":\"" << json::escape(signal.symbol) << "\",\"trade_id\":\""
                  << json::escape(signal.trade_id) << "\",\"action\":\"" << signal.action
                  << "\",\"reason\":\"" << signal.reason
                  << "\",\"score\":" << format_double(signal.score)
                  << ",\"price\":" << format_double(signal.price)
                  << ",\"out_of_order\":" << (signal.out_of_order ? "true" : "false")
                  << ",\"execution\":\"DISABLED_DEMO_ONLY\"}";
      if (!events.append(event_line.str()) || !factors_out.append(factor_line.str()) ||
          !signals_out.append(signal_line.str())) {
        std::cerr << "replay write failed: " << events.error() << factors_out.error()
                  << signals_out.error() << '\n';
        return 2;
      }
    }
  }
  if (!events.finalize() || !factors_out.finalize() || !signals_out.finalize()) {
    std::cerr << "replay finalization failed\n";
    return 2;
  }

  std::string result;
  int exit_code = 2;
  if (valid == 0) {
    result = "FAIL";
    first_error = "no valid trades replayed";
  } else if (malformed > 0) {
    result = "FAIL";
    first_error = "malformed trade payloads in raw input";
  } else if (duplicates > 0 || out_of_order > 0) {
    result = "PARTIAL";
    exit_code = 3;
  } else {
    result = "PASS";
    exit_code = 0;
  }
  std::ostringstream summary;
  summary << "{\"result\":\"" << result << "\",\"replay\":true,\"input\":\""
          << json::escape(input.string()) << "\",\"input_sha256\":\"" << input_sha
          << "\",\"records_read\":" << records_read
          << ",\"frames_validated\":" << frames_validated
          << ",\"integrity_errors\":0"
          << ",\"trades_processed\":" << valid
          << ",\"duplicates\":" << duplicates
          << ",\"out_of_order\":" << out_of_order
          << ",\"malformed_frames\":" << malformed
          << ",\"exit_code\":" << exit_code
          << ",\"execution_enabled\":false}\n";
  if (!atomic_write_file(output / "replay_summary.json", summary.str(), error)) {
    std::cerr << error << '\n';
    return 2;
  }
  std::cout << summary.str();
  return exit_code;
}

int synthetic(std::size_t count) {
  FactorEngine engine;
  const auto start = std::chrono::steady_clock::now();
  std::uint64_t signals = 0;
  for (std::size_t i = 0; i < count; ++i) {
    Trade trade{"BTCUSDT",   i % 2 ? "sell" : "buy", std::to_string(i), "67000.0",
                "0.001",     "0",                    67000.0,         0.001,
                i};
    trade.received_steady_ns = steady_ns();
    trade.exchange_ts_ns = trade.received_steady_ns;
    const Factors factors = engine.update(trade);
    const Signal signal = evaluate(factors);
    if (signal.action != "HOLD") ++signals;
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::cout << "{\"benchmark_type\":\"SYNTHETIC_BENCHMARK\",\"mode\":\"factors_strategy\","
               "\"live_network\":false,\"events\":"
            << count << ",\"events_per_second\":"
            << (seconds > 0 ? static_cast<double>(count) / seconds : 0.0)
            << ",\"signals_emitted\":" << signals << "}\n";
  return 0;
}

const char* to_string(RunResult result) {
  switch (result) {
    case RunResult::Pass: return "PASS";
    case RunResult::Partial: return "PARTIAL";
    case RunResult::Blocked: return "BLOCKED";
    case RunResult::Fail: return "FAIL";
  }
  return "FAIL";
}

}  // namespace marketpipe
