#include "marketpipe/runtime.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

#include <openssl/evp.h>

#include "marketpipe/json.hpp"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <io.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace marketpipe {
namespace {

std::string errno_message(const std::string& prefix) {
  std::ostringstream out;
  out << prefix << ": " << std::strerror(errno);
  return out.str();
}

bool sync_stream(std::FILE* file, std::string& error) {
  if (std::fflush(file) != 0) {
    error = errno_message("fflush failed");
    return false;
  }
#ifdef _WIN32
  if (_commit(_fileno(file)) != 0) {
    error = errno_message("commit/fsync failed");
    return false;
  }
#else
  if (::fsync(::fileno(file)) != 0) {
    error = errno_message("fsync failed");
    return false;
  }
#endif
  return true;
}

std::tm utc_tm(std::time_t seconds) {
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &seconds);
#else
  gmtime_r(&seconds, &tm);
#endif
  return tm;
}

}  // namespace

std::int64_t steady_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::int64_t wall_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::uint64_t process_cpu_ns() {
#ifdef _WIN32
  FILETIME creation{}, exit_time{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user)) return 0;
  auto to_ns = [](const FILETIME& ft) -> std::uint64_t {
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart * 100ULL;
  };
  return to_ns(kernel) + to_ns(user);
#else
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
  return static_cast<std::uint64_t>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1000ULL;
#endif
}

std::uint64_t process_rss_bytes() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                           sizeof(counters))) {
    return static_cast<std::uint64_t>(counters.WorkingSetSize);
  }
  return 0;
#else
  std::ifstream statm("/proc/self/statm");
  long pages = 0;
  if (statm >> pages) {
    return static_cast<std::uint64_t>(pages) *
           static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
  }
  return 0;
#endif
}

std::string utc_now_iso() { return utc_iso_from_ns(wall_ns()); }

std::string utc_iso_from_ms(std::int64_t epoch_ms) {
  return utc_iso_from_ns(epoch_ms * 1000000LL);
}

std::string utc_iso_from_ns(std::int64_t epoch_ns) {
  const std::int64_t seconds = epoch_ns / 1000000000LL;
  const std::int64_t nanos = epoch_ns % 1000000000LL;
  const std::tm tm = utc_tm(static_cast<std::time_t>(seconds));
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(9) << std::setfill('0')
      << nanos << 'Z';
  return out.str();
}

std::string format_double(double value) {
  if (!std::isfinite(value)) return "0";
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << value;
  return out.str();
}

std::string format_json_string(std::string_view text) { return json::escape(text); }

struct Sha256::Impl {
  EVP_MD_CTX* context{nullptr};
};

Sha256::Sha256() : impl_(new Impl()) {
  impl_->context = EVP_MD_CTX_new();
  EVP_DigestInit_ex(impl_->context, EVP_sha256(), nullptr);
}

Sha256::~Sha256() {
  if (impl_) {
    if (impl_->context) EVP_MD_CTX_free(impl_->context);
    delete impl_;
  }
}

Sha256::Sha256(Sha256&& other) noexcept : impl_(other.impl_) { other.impl_ = nullptr; }

Sha256& Sha256::operator=(Sha256&& other) noexcept {
  if (this != &other) {
    if (impl_) {
      if (impl_->context) EVP_MD_CTX_free(impl_->context);
      delete impl_;
    }
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

void Sha256::update(const void* data, std::size_t size) {
  if (impl_ && impl_->context && size > 0) {
    EVP_DigestUpdate(impl_->context, data, size);
  }
}

std::string Sha256::hex() {
  if (!impl_ || !impl_->context) return std::string(64, '0');
  EVP_MD_CTX* copy = EVP_MD_CTX_new();
  EVP_MD_CTX_copy_ex(copy, impl_->context);
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  EVP_DigestFinal_ex(copy, digest, &length);
  EVP_MD_CTX_free(copy);
  static const char* kHex = "0123456789abcdef";
  std::string output;
  output.reserve(length * 2);
  for (unsigned int i = 0; i < length; ++i) {
    output.push_back(kHex[(digest[i] >> 4) & 0x0F]);
    output.push_back(kHex[digest[i] & 0x0F]);
  }
  return output;
}

void Sha256::reset() {
  if (impl_ && impl_->context) {
    EVP_MD_CTX_reset(impl_->context);
    EVP_DigestInit_ex(impl_->context, EVP_sha256(), nullptr);
  }
}

std::string sha256_hex(std::string_view data) {
  Sha256 hash;
  hash.update(data);
  return hash.hex();
}

bool sha256_file(const std::filesystem::path& path, std::string& hex, std::string& error) {
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (!file) {
    error = errno_message(("cannot open " + path.string()).c_str());
    return false;
  }
  Sha256 hash;
  std::vector<char> buffer(1024 * 1024);
  for (;;) {
    const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
    if (read > 0) hash.update(buffer.data(), read);
    if (read < buffer.size()) {
      if (std::ferror(file)) {
        error = errno_message("read failed during SHA-256");
        std::fclose(file);
        return false;
      }
      break;
    }
  }
  std::fclose(file);
  hex = hash.hex();
  return true;
}

bool ensure_directory(const std::filesystem::path& path, std::string& error) {
  std::error_code ec;
  if (path.empty()) return true;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    error = "cannot create directory " + path.string() + ": " + ec.message();
    return false;
  }
  if (!std::filesystem::is_directory(path, ec)) {
    error = path.string() + " is not a directory";
    return false;
  }
  return true;
}

bool atomic_write_file(const std::filesystem::path& path, std::string_view content,
                       std::string& error) {
  std::string dir_error;
  if (!ensure_directory(path.parent_path(), dir_error)) {
    error = dir_error;
    return false;
  }
  const auto temporary = path.string() + ".tmp";
  std::FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file) {
    error = errno_message(("cannot create " + temporary).c_str());
    return false;
  }
  const std::size_t written = std::fwrite(content.data(), 1, content.size(), file);
  if (written != content.size()) {
    error = errno_message("short write while writing temporary file");
    std::fclose(file);
    std::remove(temporary.c_str());
    return false;
  }
  if (!sync_stream(file, error)) {
    std::fclose(file);
    std::remove(temporary.c_str());
    return false;
  }
  if (std::fclose(file) != 0) {
    error = errno_message("fclose failed");
    std::remove(temporary.c_str());
    return false;
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::rename(temporary, path, ec);
  if (ec) {
    error = "cannot rename temporary file: " + ec.message();
    std::remove(temporary.c_str());
    return false;
  }
  return true;
}

Logger::Logger(std::filesystem::path path, bool console)
    : path_(std::move(path)), console_(console) {}

Logger::~Logger() { close(); }

bool Logger::open(std::string& error) {
  std::string dir_error;
  if (!ensure_directory(path_.parent_path(), dir_error)) {
    error = dir_error;
    return false;
  }
  file_ = std::fopen(path_.string().c_str(), "ab");
  if (!file_) {
    error = errno_message(("cannot open log " + path_.string()).c_str());
    return false;
  }
  return true;
}

void Logger::write(std::string_view level, std::string_view event, std::string_view fields_json) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!file_) return;
  std::ostringstream line;
  line << "{\"ts\":\"" << utc_now_iso() << "\",\"level\":\"" << level << "\",\"event\":\""
       << json::escape(event) << "\",\"fields\":{" << fields_json << "}}\n";
  const std::string text = line.str();
  if (std::fwrite(text.data(), 1, text.size(), file_) != text.size()) {
    failed_.store(true, std::memory_order_relaxed);
    return;
  }
  std::fflush(file_);
  ++records_since_sync_;
  records_.fetch_add(1, std::memory_order_relaxed);
  if (records_since_sync_ >= 10) {
    std::string error;
    if (!sync_stream(file_, error)) failed_.store(true, std::memory_order_relaxed);
    records_since_sync_ = 0;
  }
  if (console_) {
    std::cerr << "[" << level << "] " << event;
    if (!fields_json.empty()) std::cerr << " {" << fields_json << '}';
    std::cerr << '\n';
  }
}

void Logger::info(std::string_view event, std::string_view fields_json) {
  write("INFO", event, fields_json);
}
void Logger::warn(std::string_view event, std::string_view fields_json) {
  write("WARN", event, fields_json);
}
void Logger::error(std::string_view event, std::string_view fields_json) {
  write("ERROR", event, fields_json);
}

void Logger::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!file_) return;
  std::string error;
  if (!sync_stream(file_, error)) failed_.store(true, std::memory_order_relaxed);
  std::fclose(file_);
  file_ = nullptr;
}

LatencySampler::LatencySampler(std::size_t every_n, std::size_t max_samples)
    : every_n_(every_n ? every_n : 1), max_samples_(max_samples) {
  values_us_.reserve(std::min<std::size_t>(max_samples_, 200000));
}

void LatencySampler::add(std::int64_t value_ns) {
  ++seen_;
  if (seen_ % every_n_ != 0 || values_us_.size() >= max_samples_) return;
  values_us_.push_back(static_cast<double>(std::max<std::int64_t>(0, value_ns)) / 1000.0);
}

LatencySummary LatencySampler::summary() const {
  LatencySummary result;
  result.samples = values_us_.size();
  if (values_us_.empty()) return result;
  std::vector<double> ordered = values_us_;
  std::sort(ordered.begin(), ordered.end());
  auto percentile = [&](double p) {
    const std::size_t index = static_cast<std::size_t>(
        std::min<double>(static_cast<double>(ordered.size() - 1),
                         std::max<double>(0.0, static_cast<double>(ordered.size() - 1) * p)));
    return ordered[index];
  };
  result.p50_us = percentile(0.50);
  result.p95_us = percentile(0.95);
  result.p99_us = percentile(0.99);
  result.max_us = ordered.back();
  return result;
}

TradeDeduplicator::TradeDeduplicator(std::size_t capacity) : capacity_(capacity ? capacity : 1) {}

DedupResult TradeDeduplicator::classify(const Trade& trade) {
  const std::string key = trade.event_key();
  if (seen_.find(key) != seen_.end()) return DedupResult::Duplicate;

  long long numeric_id = 0;
  bool numeric = false;
  if (!trade.id.empty()) {
    try {
      std::size_t consumed = 0;
      numeric_id = std::stoll(trade.id, &consumed);
      numeric = consumed == trade.id.size();
    } catch (...) {
      numeric = false;
    }
  }

  DedupResult result = DedupResult::New;
  auto last = last_id_by_symbol_.find(trade.symbol);
  if (numeric && last != last_id_by_symbol_.end() && numeric_id <= last->second) {
    result = DedupResult::OutOfOrder;
  }

  seen_.insert(key);
  order_.push_back(key);
  if (numeric) {
    auto [it, inserted] = last_id_by_symbol_.emplace(trade.symbol, numeric_id);
    if (!inserted && numeric_id > it->second) it->second = numeric_id;
  }
  while (seen_.size() > capacity_ && !order_.empty()) {
    seen_.erase(order_.front());
    order_.pop_front();
  }
  return result;
}

JsonlFileWriter::JsonlFileWriter(std::filesystem::path path, std::size_t fsync_every,
                                 WriterFault fault, std::uint64_t fail_after_bytes)
    : path_(std::move(path)),
      fsync_every_(fsync_every ? fsync_every : 1),
      fault_(fault),
      fail_after_bytes_(fail_after_bytes) {}

JsonlFileWriter::~JsonlFileWriter() {
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

bool JsonlFileWriter::open(std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string dir_error;
  if (!ensure_directory(path_.parent_path(), dir_error)) {
    error = dir_error;
    set_error(error);
    return false;
  }
  file_ = std::fopen(path_.string().c_str(), "wb");
  if (!file_) {
    error = errno_message(("cannot open " + path_.string()).c_str());
    set_error(error);
    return false;
  }
  buffer_.reserve(256 * 1024);
  return true;
}

void JsonlFileWriter::set_error(std::string message) {
  if (error_.empty()) error_ = std::move(message);
}

bool JsonlFileWriter::append(std::string_view record) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!error_.empty() || finalized_ || !file_) return false;
  if (fault_ == WriterFault::SimulatedDiskFull && fail_after_bytes_ > 0 &&
      simulated_bytes_ + record.size() + 1 > fail_after_bytes_) {
    set_error("ENOSPC (simulated disk full) while writing " + path_.string());
    return false;
  }
  buffer_.append(record);
  buffer_.push_back('\n');
  simulated_bytes_ += record.size() + 1;
  ++records_;
  ++records_since_sync_;
  if (buffer_.size() >= 1024 * 1024) {
    if (!flush_locked()) return false;
  }
  if (records_since_sync_ >= fsync_every_) {
    if (!sync_locked()) return false;
  }
  return true;
}

bool JsonlFileWriter::flush_locked() {
  if (buffer_.empty()) return error_.empty();
  if (!file_) {
    set_error("flush attempted on closed writer");
    return false;
  }
  const std::size_t written = std::fwrite(buffer_.data(), 1, buffer_.size(), file_);
  if (written != buffer_.size()) {
    set_error(errno_message("short write (disk full?) to " + path_.string()));
    return false;
  }
  sha_.update(buffer_);
  bytes_ += written;
  buffer_.clear();
  return true;
}

bool JsonlFileWriter::sync_locked() {
  if (!flush_locked()) return false;
  if (!file_) return false;
  std::string error;
  if (!sync_stream(file_, error)) {
    set_error(error);
    return false;
  }
  records_since_sync_ = 0;
  ++fsync_count_;
  return true;
}

bool JsonlFileWriter::flush_batch() {
  std::lock_guard<std::mutex> lock(mutex_);
  return flush_locked();
}

bool JsonlFileWriter::sync_now() {
  std::lock_guard<std::mutex> lock(mutex_);
  return sync_locked();
}

bool JsonlFileWriter::finalize() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (finalized_) return error_.empty();
  bool ok = true;
  if (file_) {
    if (!sync_locked()) ok = false;
    if (fault_ == WriterFault::FinalSyncFailure) {
      set_error("simulated final fsync failure");
      ok = false;
    }
    if (std::fclose(file_) != 0) {
      set_error(errno_message("fclose failed"));
      ok = false;
    }
    file_ = nullptr;
  }
  finalized_ = true;
  return ok && error_.empty();
}

void JsonlFileWriter::abort() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  finalized_ = true;
}

std::string JsonlFileWriter::file_sha256() {
  std::lock_guard<std::mutex> lock(mutex_);
  return sha_.hex();
}

ResourceSampler::ResourceSampler() = default;
ResourceSampler::~ResourceSampler() { stop(); }

void ResourceSampler::start() {
  if (started_) return;
  started_ = true;
  stop_.store(false, std::memory_order_relaxed);
  thread_ = std::thread([this] { run(); });
}

void ResourceSampler::stop() {
  if (!started_) return;
  stop_.store(true, std::memory_order_relaxed);
  if (thread_.joinable()) thread_.join();
  started_ = false;
}

void ResourceSampler::sample_once() {
  double cpu = 0.0;
  std::uint64_t rss = 0;
  if (!sample(cpu, rss)) return;
  std::lock_guard<std::mutex> lock(mutex_);
  cpu_samples_.push_back(cpu);
  rss_samples_.push_back(rss);
}

void ResourceSampler::run() {
  while (!stop_.load(std::memory_order_relaxed)) {
    double cpu = 0.0;
    std::uint64_t rss = 0;
    if (sample(cpu, rss)) {
      std::lock_guard<std::mutex> lock(mutex_);
      cpu_samples_.push_back(cpu);
      rss_samples_.push_back(rss);
    }
    for (int i = 0; i < 10 && !stop_.load(std::memory_order_relaxed); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

#ifdef _WIN32
bool ResourceSampler::sample(double& cpu_percent, std::uint64_t& rss_bytes) {
  FILETIME creation{}, exit{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return false;
  auto to_ns = [](const FILETIME& ft) -> std::uint64_t {
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart * 100ULL;  // 100 ns units
  };
  const std::uint64_t cpu_ns = to_ns(kernel) + to_ns(user);
  const std::int64_t now = steady_ns();
  if (last_cpu_ns_ != 0 && last_sample_ns_ != 0 && now > last_sample_ns_) {
    const double wall = static_cast<double>(now - last_sample_ns_);
    cpu_percent = 100.0 * static_cast<double>(cpu_ns - last_cpu_ns_) / wall;
  } else {
    cpu_percent = 0.0;
  }
  last_cpu_ns_ = cpu_ns;
  last_sample_ns_ = now;
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                           sizeof(counters))) {
    rss_bytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
  }
  return true;
}
#else
bool ResourceSampler::sample(double& cpu_percent, std::uint64_t& rss_bytes) {
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return false;
  const std::uint64_t cpu_ns =
      static_cast<std::uint64_t>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000000ULL +
      static_cast<std::uint64_t>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1000ULL;
  const std::int64_t now = steady_ns();
  if (last_cpu_ns_ != 0 && last_sample_ns_ != 0 && now > last_sample_ns_) {
    cpu_percent = 100.0 * static_cast<double>(cpu_ns - last_cpu_ns_) /
                  static_cast<double>(now - last_sample_ns_);
  } else {
    cpu_percent = 0.0;
  }
  last_cpu_ns_ = cpu_ns;
  last_sample_ns_ = now;
  std::ifstream statm("/proc/self/statm");
  long pages = 0;
  if (statm >> pages) {
    rss_bytes = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
  } else {
    rss_bytes = 0;
  }
  return true;
}
#endif

double ResourceSampler::average_cpu_percent() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (cpu_samples_.empty()) return 0.0;
  double total = 0.0;
  for (double sample : cpu_samples_) total += sample;
  return total / static_cast<double>(cpu_samples_.size());
}

std::uint64_t ResourceSampler::peak_rss_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::uint64_t peak = 0;
  for (std::uint64_t sample : rss_samples_) peak = std::max(peak, sample);
  return peak;
}

std::uint64_t ResourceSampler::samples() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cpu_samples_.size();
}

std::string percentile_line(const LatencySummary& latency) {
  std::ostringstream out;
  out << "samples=" << latency.samples << " p50_us=" << format_double(latency.p50_us)
      << " p95_us=" << format_double(latency.p95_us) << " p99_us=" << format_double(latency.p99_us)
      << " max_us=" << format_double(latency.max_us);
  return out.str();
}

}  // namespace marketpipe
