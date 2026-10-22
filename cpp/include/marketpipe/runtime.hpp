#pragma once

// Runtime utilities shared by the collector, report tool and tests:
// bounded queues, durable JSONL writers, hashing, timing, logging and
// process resource sampling.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "marketpipe.hpp"

namespace marketpipe {

std::int64_t steady_ns();
std::int64_t wall_ns();
std::uint64_t process_cpu_ns();
std::uint64_t process_rss_bytes();
std::string utc_now_iso();
std::string utc_iso_from_ms(std::int64_t epoch_ms);
std::string utc_iso_from_ns(std::int64_t epoch_ns);
std::string format_double(double value);
std::string format_json_string(std::string_view text);

class Sha256 {
 public:
  Sha256();
  ~Sha256();
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;
  Sha256(Sha256&& other) noexcept;
  Sha256& operator=(Sha256&& other) noexcept;
  void update(const void* data, std::size_t size);
  void update(std::string_view data) { update(data.data(), data.size()); }
  std::string hex();
  void reset();

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

bool sha256_file(const std::filesystem::path& path, std::string& hex, std::string& error);
bool atomic_write_file(const std::filesystem::path& path, std::string_view content,
                       std::string& error);
bool ensure_directory(const std::filesystem::path& path, std::string& error);

// Thread-safe JSON-lines logger. Every record is flushed; fsync runs every ten
// records and on close so a crash leaves a usable diagnostic trail.
class Logger {
 public:
  explicit Logger(std::filesystem::path path, bool console);
  ~Logger();
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;
  bool open(std::string& error);
  void info(std::string_view event, std::string_view fields_json = "");
  void warn(std::string_view event, std::string_view fields_json = "");
  void error(std::string_view event, std::string_view fields_json = "");
  void close();
  bool failed() const { return failed_.load(std::memory_order_relaxed); }
  std::uint64_t records() const { return records_.load(std::memory_order_relaxed); }
  const std::filesystem::path& path() const { return path_; }

 private:
  void write(std::string_view level, std::string_view event, std::string_view fields_json);
  std::filesystem::path path_;
  bool console_;
  std::FILE* file_{nullptr};
  mutable std::mutex mutex_;
  std::atomic<bool> failed_{false};
  std::atomic<std::uint64_t> records_{0};
  std::uint64_t records_since_sync_{0};
};

// Bounded multi-producer/multi-consumer queue. try_push never blocks and never
// silently drops: once capacity is reached the caller observes false,
// overflowed() becomes true, and the collector must invalidate the sample.
template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity ? capacity : 1) {}

  bool try_push(T value) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) return false;
      if (items_.size() >= capacity_) {
        overflowed_.store(true, std::memory_order_relaxed);
        return false;
      }
      items_.push_back(std::move(value));
      const auto size = items_.size();
      std::size_t high = high_water_.load(std::memory_order_relaxed);
      while (size > high &&
             !high_water_.compare_exchange_weak(high, size, std::memory_order_relaxed)) {
      }
    }
    cv_.notify_one();
    return true;
  }

  // Blocks until an item is available or the queue is closed and empty.
  bool pop(T& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return closed_ || !items_.empty(); });
    if (items_.empty()) return false;
    out = std::move(items_.front());
    items_.pop_front();
    return true;
  }

  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    cv_.notify_all();
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }
  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
  }
  std::size_t capacity() const { return capacity_; }
  std::size_t high_water() const { return high_water_.load(std::memory_order_relaxed); }
  bool overflowed() const { return overflowed_.load(std::memory_order_relaxed); }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<T> items_;
  std::size_t capacity_;
  bool closed_{false};
  std::atomic<bool> overflowed_{false};
  std::atomic<std::size_t> high_water_{0};
};

class LatencySampler {
 public:
  LatencySampler(std::size_t every_n = 5, std::size_t max_samples = 200000);
  void add(std::int64_t value_ns);
  LatencySummary summary() const;
  std::uint64_t seen() const { return seen_; }

 private:
  std::size_t every_n_;
  std::size_t max_samples_;
  std::uint64_t seen_{0};
  std::vector<double> values_us_;
};

enum class DedupResult { New, Duplicate, OutOfOrder };

// Global trade-id de-duplication with bounded memory. IDs are compared
// numerically per symbol for out-of-order detection, and every accepted frame
// carries its connection generation separately (dedup is deliberately global
// across reconnects so a replay of the same trade after reconnect is caught).
class TradeDeduplicator {
 public:
  explicit TradeDeduplicator(std::size_t capacity = 200000);
  DedupResult classify(const Trade& trade);

 private:
  std::size_t capacity_;
  std::unordered_set<std::string> seen_;
  std::deque<std::string> order_;
  std::unordered_map<std::string, long long> last_id_by_symbol_;
};

enum class WriterFault { None, SimulatedDiskFull, FinalSyncFailure };

// Buffered JSONL writer. Records are hashed incrementally and fsynced every
// `fsync_every` records; finalize() flushes, fsyncs, closes and fixes the
// digest. Errors are sticky and reported instead of being swallowed.
class JsonlFileWriter {
 public:
  JsonlFileWriter(std::filesystem::path path, std::size_t fsync_every = 100,
                  WriterFault fault = WriterFault::None,
                  std::uint64_t fail_after_bytes = 0);
  ~JsonlFileWriter();
  JsonlFileWriter(const JsonlFileWriter&) = delete;
  JsonlFileWriter& operator=(const JsonlFileWriter&) = delete;

  bool open(std::string& error);
  bool append(std::string_view record);
  bool flush_batch();
  bool sync_now();
  bool finalize();
  void abort();

  bool is_open() const { return file_ != nullptr; }
  bool finalized() const { return finalized_; }
  bool failed() const { return !error_.empty(); }
  const std::string& error() const { return error_; }
  std::uint64_t records() const { return records_; }
  std::uint64_t bytes() const { return bytes_; }
  std::uint64_t fsync_count() const { return fsync_count_; }
  std::string file_sha256();
  const std::filesystem::path& path() const { return path_; }

 private:
  bool flush_locked();
  bool sync_locked();
  void set_error(std::string message);

  std::filesystem::path path_;
  std::size_t fsync_every_;
  WriterFault fault_;
  std::uint64_t fail_after_bytes_;
  std::FILE* file_{nullptr};
  std::string buffer_;
  std::uint64_t records_{};
  std::uint64_t bytes_{};
  std::uint64_t fsync_count_{};
  std::uint64_t records_since_sync_{};
  bool finalized_{false};
  std::uint64_t simulated_bytes_{};
  std::string error_;
  Sha256 sha_;
  mutable std::mutex mutex_;
};

// Samples process CPU and RSS once per second on a background thread.
class ResourceSampler {
 public:
  ResourceSampler();
  ~ResourceSampler();
  ResourceSampler(const ResourceSampler&) = delete;
  ResourceSampler& operator=(const ResourceSampler&) = delete;
  void start();
  void stop();
  void sample_once();
  double average_cpu_percent() const;
  std::uint64_t peak_rss_bytes() const;
  std::uint64_t samples() const;

 private:
  void run();
  bool sample(double& cpu_percent, std::uint64_t& rss_bytes);

  mutable std::mutex mutex_;
  std::vector<double> cpu_samples_;
  std::vector<std::uint64_t> rss_samples_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  bool started_{false};
  std::uint64_t last_cpu_ns_{0};
  std::int64_t last_sample_ns_{0};
};

// Summarises numeric fields for the human-readable acceptance report.
std::string percentile_line(const LatencySummary& latency);

}  // namespace marketpipe
