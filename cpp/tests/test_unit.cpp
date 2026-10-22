#include "test_framework.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "marketpipe/json.hpp"
#include "marketpipe/runtime.hpp"

using namespace marketpipe;

namespace {

std::filesystem::path temp_directory(const std::string& name) {
  const auto base = std::filesystem::current_path() / ".marketpipe-test-tmp";
  std::error_code ec;
  std::filesystem::create_directories(base, ec);
  const auto path = base / ("unit_" + name + "_" + std::to_string(wall_ns()));
  std::filesystem::remove_all(path, ec);
  std::filesystem::create_directories(path, ec);
  MP_CHECK(!ec);
  return path;
}

Trade make_trade(std::uint64_t id, const std::string& side, double price, double qty,
                 std::int64_t received_ns) {
  Trade trade;
  trade.symbol = "BTCUSDT";
  trade.id = std::to_string(id);
  trade.side = side;
  trade.price = price;
  trade.qty = qty;
  trade.price_text = format_double(price);
  trade.qty_text = format_double(qty);
  trade.exchange_ts_ns = 1700000000000000000LL + static_cast<std::int64_t>(id) * 1000000LL;
  trade.exchange_time = utc_iso_from_ns(trade.exchange_ts_ns);
  trade.received_steady_ns = received_ns;
  return trade;
}

std::string replay_trade_payload(std::uint64_t id) {
  const long long timestamp = 1700000000000LL + static_cast<long long>(id) * 1000LL;
  std::ostringstream payload;
  payload << "{\"e\":\"trade\",\"E\":" << timestamp << ",\"s\":\"BTCUSDT\",\"t\":" << id
          << ",\"p\":\"100.0\",\"q\":\"1.0\",\"T\":" << timestamp << ",\"m\":false}";
  return payload.str();
}

std::string replay_raw_record(const std::string& payload, std::uint64_t seq,
                              std::int64_t received_ns,
                              const std::string& hash_override = "") {
  const std::string hash = hash_override.empty() ? sha256_hex(payload) : hash_override;
  std::ostringstream record;
  record << "{\"record_type\":\"raw_websocket_frame\",\"run_id\":\"unit-replay\","
            "\"connection_id\":\"unit-replay-conn-1\",\"connection_generation\":1,"
            "\"frame_seq\":"
         << seq << ",\"received_at_utc\":\"2026-10-22T00:00:00.000000000Z\","
         << "\"received_monotonic_ns\":" << received_ns
         << ",\"received_utc\":\"2026-10-22T00:00:00.000000000Z\",\"payload\":\""
         << json::escape(payload) << "\",\"payload_sha256\":\"" << hash
         << "\",\"payload_bytes\":" << payload.size() << '}';
  return record.str();
}

std::vector<json::Value> read_json_lines(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::vector<json::Value> values;
  std::string line;
  while (std::getline(input, line)) {
    json::Value value;
    json::ParseError error;
    MP_REQUIRE(json::parse(line, value, error));
    values.push_back(std::move(value));
  }
  return values;
}

}  // namespace

MP_TEST(json_parses_and_preserves_number_text) {
  json::Value value;
  json::ParseError error;
  MP_REQUIRE(json::parse(
      R"({"e":"trade","t":123456789012345,"p":"67000.12345678","q":0.0012,"m":false,"nested":{"a":[1,2,3]}})",
      value, error));
  MP_CHECK(value.type == json::Type::Object);
  const json::Value* price = value.find("p");
  MP_REQUIRE(price != nullptr);
  MP_CHECK(price->is_string());
  MP_CHECK_EQ(price->text, "67000.12345678");
  const json::Value* qty = value.find("q");
  MP_REQUIRE(qty != nullptr);
  MP_CHECK_EQ(qty->text, "0.0012");
  const json::Value* id = value.find("t");
  MP_REQUIRE(id != nullptr);
  MP_CHECK_EQ(id->text, "123456789012345");
}

MP_TEST(json_rejects_malformed_input) {
  json::Value value;
  json::ParseError error;
  MP_CHECK(!json::parse("{", value, error));
  MP_CHECK(!json::parse("{\"a\":}", value, error));
  MP_CHECK(!json::parse("{\"a\":1} trailing", value, error));
  MP_CHECK(!json::parse("{\"a\":\"\\q\"}", value, error));
  MP_CHECK(!json::parse("{\"a\":\"line\nbreak\"}", value, error));
  MP_CHECK(!json::parse("[1,2,3", value, error));
  std::string deep;
  for (int i = 0; i < 70; ++i) deep.push_back('[');
  deep += "0";
  for (int i = 0; i < 70; ++i) deep.push_back(']');
  MP_CHECK(!json::parse(deep, value, error));
}

MP_TEST(json_decodes_escapes) {
  json::Value value;
  json::ParseError error;
  MP_REQUIRE(json::parse(R"({"s":"a\"b\\c\u00e9\u0041"})", value, error));
  const json::Value* text = value.find("s");
  MP_REQUIRE(text != nullptr);
  MP_CHECK_EQ(text->text, std::string("a\"b\\c\xc3\xa9") + "A");
}

MP_TEST(trade_parser_preserves_decimal_text_and_side) {
  ParseContext context;
  context.expected_symbol = "BTCUSDT";
  context.frame_seq = 7;
  context.connection_generation = 2;
  context.connection_id = "run-conn-2";
  context.received_utc = "2026-10-22T00:00:00.000000000Z";
  context.received_steady_ns = 42;
  const std::string frame =
      R"({"e":"trade","E":1700000000000,"s":"BTCUSDT","t":99,"p":"67000.12345678","q":"0.00120000","T":1700000000123,"m":true,"M":true})";
  ParsedMessage parsed = parse_binance_message(frame, context);
  MP_CHECK(parsed.status == ParseStatus::Ok);
  MP_REQUIRE(parsed.trades.size() == 1);
  const Trade& trade = parsed.trades.front();
  MP_CHECK_EQ(trade.symbol, "BTCUSDT");
  MP_CHECK_EQ(trade.id, "99");
  MP_CHECK_EQ(trade.price_text, "67000.12345678");
  MP_CHECK_EQ(trade.qty_text, "0.00120000");
  MP_CHECK_EQ(trade.side, "sell");
  MP_CHECK_EQ(trade.frame_seq, 7u);
  MP_CHECK_EQ(trade.connection_generation, 2u);
  MP_CHECK_EQ(trade.received_steady_ns, 42);
  MP_CHECK_EQ(trade.exchange_ts_ns, 1700000000123000000LL);
}

MP_TEST(trade_parser_handles_combined_stream_envelope) {
  ParseContext context;
  context.expected_symbol = "BTCUSDT";
  const std::string envelope =
      R"({"stream":"btcusdt@trade","data":{"e":"trade","E":1700000000000,"s":"BTCUSDT","t":5,"p":"1.5","q":"2.5","T":1700000000001,"m":false}})";
  ParsedMessage parsed = parse_binance_message(envelope, context);
  MP_CHECK(parsed.status == ParseStatus::Ok);
  MP_REQUIRE(parsed.trades.size() == 1);
  MP_CHECK_EQ(parsed.trades.front().side, "buy");
  MP_CHECK_EQ(parsed.trades.front().price_text, "1.5");
}

MP_TEST(trade_parser_rejects_bad_frames) {
  ParseContext context;
  context.expected_symbol = "BTCUSDT";
  auto status_of = [&](const std::string& frame) {
    return parse_binance_message(frame, context).status;
  };
  MP_CHECK(status_of("not json") == ParseStatus::Malformed);
  MP_CHECK(status_of(R"({"e":"trade"})") == ParseStatus::Malformed);
  MP_CHECK(status_of(
               R"({"e":"trade","s":"BTCUSDT","t":1,"p":"NaN","q":"1","T":1,"m":false})") ==
           ParseStatus::Malformed);
  MP_CHECK(status_of(
               R"({"e":"trade","s":"BTCUSDT","t":1,"p":"-1","q":"1","T":1,"m":false})") ==
           ParseStatus::Malformed);
  MP_CHECK(status_of(
               R"({"e":"trade","s":"ETHUSDT","t":1,"p":"1","q":"1","T":1,"m":false})") ==
           ParseStatus::SymbolMismatch);
  MP_CHECK(status_of(R"({"e":"ping"})") == ParseStatus::NotTrade);
  MP_CHECK(status_of(R"({"result":null,"id":1})") == ParseStatus::SubscriptionAck);
}

MP_TEST(factors_and_strategy_emit_buy_sell_hold) {
  FactorEngine buy_engine;
  Factors buy_factors;
  for (int i = 0; i < 12; ++i) {
    buy_factors = buy_engine.update(
        make_trade(static_cast<std::uint64_t>(i + 1), "buy", 100.0 + i * 0.5, 1.0,
                   1000000000LL + i * 1000000LL));
  }
  MP_CHECK(buy_factors.imbalance > 0.15);
  MP_CHECK(buy_factors.return_n > 0.0);
  MP_CHECK_EQ(evaluate(buy_factors).action, "BUY_SIGNAL");

  FactorEngine sell_engine;
  Factors sell_factors;
  for (int i = 0; i < 12; ++i) {
    sell_factors = sell_engine.update(
        make_trade(static_cast<std::uint64_t>(i + 1), "sell", 100.0 - i * 0.5, 1.0,
                   2000000000LL + i * 1000000LL));
  }
  MP_CHECK(sell_factors.imbalance < -0.15);
  MP_CHECK(sell_factors.return_n < 0.0);
  MP_CHECK_EQ(evaluate(sell_factors).action, "SELL_SIGNAL");

  FactorEngine hold_engine;
  Factors hold_factors;
  for (int i = 0; i < 10; ++i) {
    hold_factors = hold_engine.update(
        make_trade(static_cast<std::uint64_t>(i + 1), i % 2 == 0 ? "buy" : "sell", 100.0, 1.0,
                   3000000000LL + i * 1000000LL));
  }
  MP_CHECK_EQ(evaluate(hold_factors).action, "HOLD");
}

MP_TEST(factor_window_expires_stale_trades) {
  FactorEngine engine;
  Factors factors;
  for (int i = 0; i < 45; ++i) {
    factors = engine.update(make_trade(static_cast<std::uint64_t>(i + 1), "buy", 100.0, 1.0,
                                       4000000000LL + i * 1000000000LL));
  }
  MP_CHECK(factors.window_trades >= 29);
  MP_CHECK(factors.window_trades <= 31);
}

MP_TEST(deduplicator_detects_duplicates_and_out_of_order) {
  TradeDeduplicator dedup(10);
  MP_CHECK(dedup.classify(make_trade(1, "buy", 1.0, 1.0, 1)) == DedupResult::New);
  MP_CHECK(dedup.classify(make_trade(3, "buy", 1.0, 1.0, 3)) == DedupResult::New);
  MP_CHECK(dedup.classify(make_trade(2, "buy", 1.0, 1.0, 2)) == DedupResult::OutOfOrder);
  MP_CHECK(dedup.classify(make_trade(3, "buy", 1.0, 1.0, 3)) == DedupResult::Duplicate);
  MP_CHECK(dedup.classify(make_trade(4, "buy", 1.0, 1.0, 4)) == DedupResult::New);
}

MP_TEST(bounded_queue_reports_overflow_and_drains) {
  BoundedQueue<int> queue(2);
  MP_CHECK(queue.try_push(1));
  MP_CHECK(queue.try_push(2));
  MP_CHECK(!queue.try_push(3));
  MP_CHECK(queue.overflowed());
  MP_CHECK_EQ(queue.high_water(), 2u);
  queue.close();
  int value = 0;
  MP_CHECK(queue.pop(value));
  MP_CHECK_EQ(value, 1);
  MP_CHECK(queue.pop(value));
  MP_CHECK_EQ(value, 2);
  MP_CHECK(!queue.pop(value));
  MP_CHECK(queue.closed());
}

MP_TEST(jsonl_writer_flushes_hashes_and_finalizes) {
  const auto dir = temp_directory("writer");
  const auto path = dir / "events.jsonl";
  JsonlFileWriter writer(path, 2);
  std::string error;
  MP_REQUIRE(writer.open(error));
  MP_CHECK(writer.append("{\"a\":1}"));
  MP_CHECK(writer.append("{\"a\":2}"));
  MP_CHECK(writer.append("{\"a\":3}"));
  MP_CHECK(writer.finalize());
  std::string file_hash;
  MP_REQUIRE(sha256_file(path, file_hash, error));
  MP_CHECK_EQ(writer.file_sha256(), file_hash);
  MP_CHECK_EQ(writer.records(), 3u);
  std::ifstream input(path);
  std::string line;
  int lines = 0;
  while (std::getline(input, line)) {
    ++lines;
    MP_CHECK(line.rfind("{\"a\":", 0) == 0);
  }
  MP_CHECK_EQ(lines, 3);
  input.close();
  std::filesystem::remove_all(dir);
}

MP_TEST(jsonl_writer_surfaces_simulated_disk_full) {
  const auto dir = temp_directory("diskfull");
  JsonlFileWriter writer(dir / "raw.jsonl", 1, WriterFault::SimulatedDiskFull, 32);
  std::string error;
  MP_REQUIRE(writer.open(error));
  MP_CHECK(!writer.append("{\"payload\":\"012345678901234567890123456789\"}"));
  MP_CHECK(writer.failed());
  MP_CHECK(writer.error().find("ENOSPC") != std::string::npos);
  MP_CHECK(!writer.finalize());
  std::filesystem::remove_all(dir);
}

MP_TEST(jsonl_writer_surfaces_final_sync_failure) {
  const auto dir = temp_directory("finalsync");
  JsonlFileWriter writer(dir / "raw.jsonl", 1, WriterFault::FinalSyncFailure);
  std::string error;
  MP_REQUIRE(writer.open(error));
  MP_CHECK(writer.append("{\"a\":1}"));
  MP_CHECK(!writer.finalize());
  MP_CHECK(writer.error().find("final fsync") != std::string::npos);
  std::filesystem::remove_all(dir);
}

MP_TEST(jsonl_writer_reports_open_failure) {
  const auto dir = temp_directory("openfail");
  const auto blocking_file = dir / "blocked";
  std::ofstream(blocking_file) << "not a directory";
  JsonlFileWriter writer(blocking_file / "nested" / "raw.jsonl", 1);
  std::string error;
  MP_CHECK(!writer.open(error));
  MP_CHECK(!error.empty());
  std::filesystem::remove_all(dir);
}

MP_TEST(sha256_known_vector) {
  MP_CHECK_EQ(sha256_hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  MP_CHECK_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

MP_TEST(atomic_file_write_round_trip) {
  const auto dir = temp_directory("atomic");
  const auto path = dir / "summary.json";
  std::string error;
  MP_CHECK(atomic_write_file(path, "{\"ok\":true}\n", error));
  std::ifstream input(path);
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  MP_CHECK_EQ(text, std::string("{\"ok\":true}\n"));
  input.close();
  std::filesystem::remove_all(dir);
}

MP_TEST(logger_writes_valid_structured_json) {
  const auto dir = temp_directory("logger");
  const auto path = dir / "system.log";
  {
    Logger logger(path, false);
    std::string error;
    MP_REQUIRE(logger.open(error));
    logger.info("unit_event", "\"answer\":42,\"text\":\"ok\"");
    logger.close();
  }
  std::ifstream input(path);
  std::string line;
  MP_REQUIRE(static_cast<bool>(std::getline(input, line)));
  input.close();
  json::Value record;
  json::ParseError parse_error;
  MP_REQUIRE(json::parse(line, record, parse_error));
  const json::Value* fields = record.find("fields");
  MP_REQUIRE(fields != nullptr);
  MP_CHECK(fields->is_object());
  const json::Value* answer = fields->find("answer");
  MP_REQUIRE(answer != nullptr);
  MP_CHECK_EQ(answer->text, "42");
  const json::Value* text = fields->find("text");
  MP_REQUIRE(text != nullptr);
  MP_CHECK_EQ(text->text, "ok");
  std::filesystem::remove_all(dir);
}

MP_TEST(replay_uses_saved_receive_clock_and_metadata) {
  const auto dir = temp_directory("replay-clock");
  const auto input = dir / "raw.jsonl";
  const auto output = dir / "out";
  {
    std::ofstream raw(input, std::ios::binary);
    raw << replay_raw_record(replay_trade_payload(1), 1, 1000000000LL) << '\n';
    raw << replay_raw_record(replay_trade_payload(2), 2, 32000000000LL) << '\n';
  }
  MP_CHECK_EQ(replay(input, output), 0);
  const auto factors = read_json_lines(output / "factors.jsonl");
  MP_CHECK_EQ(factors.size(), 2u);
  MP_CHECK_EQ(factors[0].find("received_ns")->text, "1000000000");
  MP_CHECK_EQ(factors[1].find("received_ns")->text, "32000000000");
  MP_CHECK_EQ(factors[1].find("window_trades")->text, "1");
  MP_CHECK_EQ(factors[1].find("trade_rate_10s")->text, "0.10000000000000001");
  const auto events = read_json_lines(output / "normalized_events.jsonl");
  MP_CHECK_EQ(events.size(), 2u);
  MP_CHECK_EQ(events[1].find("frame_seq")->text, "2");
  MP_CHECK_EQ(events[1].find("received_ns")->text, "32000000000");
  const auto summary = read_json_lines(output / "replay_summary.json");
  MP_REQUIRE(summary.size() == 1);
  MP_CHECK_EQ(summary[0].find("result")->text, "PASS");
  MP_CHECK_EQ(summary[0].find("frames_validated")->text, "2");
  std::filesystem::remove_all(dir);
}

MP_TEST(replay_rejects_tampered_hash_and_broken_input) {
  const auto dir = temp_directory("replay-corrupt");
  const auto tampered = dir / "tampered.jsonl";
  const auto tampered_out = dir / "tampered-out";
  {
    std::ofstream raw(tampered, std::ios::binary);
    raw << replay_raw_record(replay_trade_payload(1), 1, 1000000000LL) << '\n';
    raw << replay_raw_record(replay_trade_payload(2), 2, 2000000000LL, "deadbeef") << '\n';
  }
  MP_CHECK_EQ(replay(tampered, tampered_out), 2);
  {
    const auto summary = read_json_lines(tampered_out / "replay_summary.json");
    MP_REQUIRE(summary.size() == 1);
    MP_CHECK_EQ(summary[0].find("result")->text, "FAIL");
    MP_CHECK_EQ(summary[0].find("integrity_errors")->text, "1");
    MP_CHECK_EQ(summary[0].find("frames_validated")->text, "1");
    MP_CHECK_EQ(summary[0].find("trades_processed")->text, "0");
  }

  const auto broken = dir / "broken.jsonl";
  const auto broken_out = dir / "broken-out";
  {
    std::ofstream raw(broken, std::ios::binary);
    raw << "{broken\n";
  }
  MP_CHECK_EQ(replay(broken, broken_out), 2);
  {
    const auto summary = read_json_lines(broken_out / "replay_summary.json");
    MP_REQUIRE(summary.size() == 1);
    MP_CHECK_EQ(summary[0].find("result")->text, "FAIL");
    MP_CHECK_EQ(summary[0].find("integrity_errors")->text, "1");
    MP_CHECK_EQ(summary[0].find("trades_processed")->text, "0");
  }

  const auto empty = dir / "no-trades.jsonl";
  const auto empty_out = dir / "no-trades-out";
  {
    const std::string payload = R"({"result":null,"id":1})";
    std::ofstream raw(empty, std::ios::binary);
    raw << replay_raw_record(payload, 1, 1000000000LL) << '\n';
  }
  MP_CHECK_EQ(replay(empty, empty_out), 2);
  {
    const auto summary = read_json_lines(empty_out / "replay_summary.json");
    MP_REQUIRE(summary.size() == 1);
    MP_CHECK_EQ(summary[0].find("result")->text, "FAIL");
    MP_CHECK_EQ(summary[0].find("trades_processed")->text, "0");
  }

  const auto backwards = dir / "backwards-time.jsonl";
  const auto backwards_out = dir / "backwards-time-out";
  {
    std::ofstream raw(backwards, std::ios::binary);
    raw << replay_raw_record(replay_trade_payload(1), 1, 2000000000LL) << '\n';
    raw << replay_raw_record(replay_trade_payload(2), 2, 1000000000LL) << '\n';
  }
  MP_CHECK_EQ(replay(backwards, backwards_out), 2);
  {
    const auto summary = read_json_lines(backwards_out / "replay_summary.json");
    MP_REQUIRE(summary.size() == 1);
    MP_CHECK_EQ(summary[0].find("result")->text, "FAIL");
    MP_CHECK_EQ(summary[0].find("integrity_errors")->text, "1");
    MP_CHECK_EQ(summary[0].find("first_error")->text.find("received_monotonic_ns went backwards") !=
                     std::string::npos,
                 true);
  }
  std::filesystem::remove_all(dir);
}

MP_TEST(replay_processes_and_marks_out_of_order_trades) {
  const auto dir = temp_directory("replay-ooo");
  const auto input = dir / "raw.jsonl";
  const auto output = dir / "out";
  {
    std::ofstream raw(input, std::ios::binary);
    raw << replay_raw_record(replay_trade_payload(1), 1, 1000000000LL) << '\n';
    raw << replay_raw_record(replay_trade_payload(3), 2, 2000000000LL) << '\n';
    raw << replay_raw_record(replay_trade_payload(2), 3, 3000000000LL) << '\n';
  }
  MP_CHECK_EQ(replay(input, output), 3);
  const auto summary = read_json_lines(output / "replay_summary.json");
  MP_REQUIRE(summary.size() == 1);
  MP_CHECK_EQ(summary[0].find("result")->text, "PARTIAL");
  MP_CHECK_EQ(summary[0].find("trades_processed")->text, "3");
  MP_CHECK_EQ(summary[0].find("out_of_order")->text, "1");
  const auto events = read_json_lines(output / "normalized_events.jsonl");
  MP_CHECK_EQ(events.size(), 3u);
  MP_CHECK_EQ(events[2].find("trade_id")->text, "2");
  MP_REQUIRE(events[2].find("out_of_order") != nullptr);
  MP_CHECK(events[2].find("out_of_order")->is_bool());
  MP_CHECK(events[2].find("out_of_order")->boolean);
  const auto signals = read_json_lines(output / "signals.jsonl");
  MP_CHECK_EQ(signals.size(), 3u);
  MP_REQUIRE(signals[2].find("out_of_order") != nullptr);
  MP_CHECK(signals[2].find("out_of_order")->boolean);
  std::filesystem::remove_all(dir);
}

MP_TEST(utc_formatting_is_iso_utc) {
  MP_CHECK_EQ(utc_iso_from_ms(0), "1970-01-01T00:00:00.000000000Z");
  MP_CHECK_EQ(utc_iso_from_ms(1700000000123LL), "2023-11-14T22:13:20.123000000Z");
}

int main() { return testing::run_all("unit"); }
