#pragma once
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_set>

namespace marketpipe {
struct Trade { std::string symbol, side, id, price_text, qty_text, exchange_time; double price{}, qty{}; std::uint64_t frame_seq{}; };
struct Factors { double return_n{}, imbalance{}, rate_10s{}, average_size{}; std::size_t window_trades{}; };
struct Signal { std::string action, reason; double score{}; };

class Journal {
 public:
  explicit Journal(const std::filesystem::path& path);
  void append(const std::string& payload, std::uint64_t seq, const std::string& run_id);
  void close();
  std::uint64_t records() const { return records_; }
 private:
  std::ofstream out_; std::uint64_t records_{};
};

std::optional<Trade> parse_binance_trade(const std::string& json, std::uint64_t frame_seq);
class FactorEngine {
 public:
  Factors update(const Trade& trade);
 private:
  std::deque<Trade> window_; std::deque<double> prices_; double buy_qty_{}; double sell_qty_{};
};
Signal evaluate(const Factors& factors);
int replay(const std::filesystem::path& input, const std::filesystem::path& output);
int synthetic(std::size_t count);
}
