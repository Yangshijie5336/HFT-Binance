#include "marketpipe.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "marketpipe/runtime.hpp"

namespace {

namespace fs = std::filesystem;

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

void print_help() {
  std::cout
      << "marketpipe " << marketpipe::kVersion << " (C++20)\n\n"
      << "Commands:\n"
      << "  capture   --symbol BTCUSDT --duration 900 --output-dir data/runs\n"
      << "            [--ws-url URL] [--queue-capacity N] [--ring-capacity N]\n"
      << "            [--fsync-every N] [--run-id ID] [--quiet]\n"
      << "  replay    --input raw_frames.jsonl [--output-dir DIR]\n"
      << "  benchmark --events 100000 [--mode all|parse|pipeline|persist]\n"
      << "            [--output-dir data/benchmarks]\n"
      << "  report    --run-dir data/runs/<run>\n"
      << "  synthetic --events 100000   (legacy alias for the factor benchmark)\n\n"
      << "The default capture endpoint is Binance Spot "
         "wss://stream.binance.com:9443/ws/<symbol>@trade.\n"
      << "Only public market data is used. Signals are labels; order execution is disabled.\n";
}

struct Arguments {
  std::string command;
  std::vector<std::string> positional;
  std::vector<std::pair<std::string, std::string>> options;

  std::optional<std::string> get(const std::string& name) const {
    for (const auto& [key, value] : options) {
      if (key == name) return value;
    }
    return std::nullopt;
  }
  std::string get_or(const std::string& name, const std::string& fallback) const {
    auto value = get(name);
    return value ? *value : fallback;
  }
  bool has(const std::string& name) const { return get(name).has_value(); }
};

bool parse_arguments(int argc, char** argv, Arguments& arguments, std::string& error) {
  if (argc < 2) {
    error = "missing command";
    return false;
  }
  arguments.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string token = argv[i];
    if (token == "--help" || token == "-h") {
      arguments.options.emplace_back("help", "true");
      continue;
    }
    if (token.rfind("--", 0) == 0) {
      const std::size_t equals = token.find('=');
      if (equals != std::string::npos) {
        arguments.options.emplace_back(token.substr(0, equals), token.substr(equals + 1));
      } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
        arguments.options.emplace_back(token, argv[++i]);
      } else {
        arguments.options.emplace_back(token, "true");
      }
    } else {
      arguments.positional.push_back(token);
    }
  }
  return true;
}

std::optional<std::uint64_t> parse_uint(const std::string& text) {
  try {
    std::size_t consumed = 0;
    unsigned long long value = std::stoull(text, &consumed);
    if (consumed != text.size()) return std::nullopt;
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<double> parse_double(const std::string& text) {
  try {
    std::size_t consumed = 0;
    double value = std::stod(text, &consumed);
    if (consumed != text.size()) return std::nullopt;
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

std::string default_url(const std::string& symbol) {
  return "wss://stream.binance.com:9443/ws/" + lower(symbol) + "@trade";
}

std::string default_run_id(const std::string& symbol) {
  const std::string stamp = marketpipe::utc_now_iso();
  std::string compact;
  for (char c : stamp) {
    if (c >= '0' && c <= '9') compact.push_back(c);
  }
  return compact.substr(0, 14) + "Z_" + symbol;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace marketpipe;
  install_signal_handlers();

  Arguments arguments;
  std::string argument_error;
  if (!parse_arguments(argc, argv, arguments, argument_error)) {
    if (argc < 2) {
      print_help();
      return 0;
    }
    std::cerr << argument_error << '\n';
    return 2;
  }
  if (arguments.command == "--help" || arguments.command == "-h" || arguments.has("--help")) {
    print_help();
    return 0;
  }

  try {
    if (arguments.command == "capture") {
      CaptureConfig config;
      config.symbol = arguments.get_or("--symbol", "BTCUSDT");
      if (config.symbol.empty()) {
        std::cerr << "--symbol must not be empty\n";
        return 2;
      }
      config.ws_url = arguments.get_or("--ws-url", default_url(config.symbol));
      config.ca_file = arguments.get_or("--ca-file", "");
      const auto duration_text = arguments.get("--duration");
      if (duration_text) {
        auto duration = parse_double(*duration_text);
        if (!duration || *duration <= 0) {
          std::cerr << "--duration must be a positive number\n";
          return 2;
        }
        config.duration_seconds = *duration;
      }
      config.output_dir = arguments.get_or("--output-dir", "data/runs");
      config.run_id = arguments.get_or("--run-id", "");
      if (auto value = arguments.get("--queue-capacity")) {
        auto parsed = parse_uint(*value);
        if (!parsed || *parsed == 0) {
          std::cerr << "--queue-capacity must be positive\n";
          return 2;
        }
        config.queue_capacity = static_cast<std::size_t>(*parsed);
      }
      if (auto value = arguments.get("--ring-capacity")) {
        auto parsed = parse_uint(*value);
        if (!parsed || *parsed == 0) {
          std::cerr << "--ring-capacity must be positive\n";
          return 2;
        }
        config.ring_capacity = static_cast<std::size_t>(*parsed);
      }
      if (auto value = arguments.get("--fsync-every")) {
        auto parsed = parse_uint(*value);
        if (!parsed || *parsed == 0) {
          std::cerr << "--fsync-every must be positive\n";
          return 2;
        }
        config.fsync_every = static_cast<std::size_t>(*parsed);
      }
      if (auto value = arguments.get("--connect-timeout-ms")) {
        auto parsed = parse_uint(*value);
        if (!parsed || *parsed == 0) {
          std::cerr << "--connect-timeout-ms must be positive\n";
          return 2;
        }
        config.connect_timeout_ms = static_cast<int>(*parsed);
      }
      if (auto value = arguments.get("--idle-timeout-ms")) {
        auto parsed = parse_uint(*value);
        if (!parsed || *parsed == 0) {
          std::cerr << "--idle-timeout-ms must be positive\n";
          return 2;
        }
        config.idle_timeout_ms = static_cast<int>(*parsed);
      }
      if (auto value = arguments.get("--reconnect-initial")) {
        auto parsed = parse_double(*value);
        if (!parsed || *parsed < 0) {
          std::cerr << "--reconnect-initial must be non-negative\n";
          return 2;
        }
        config.reconnect_initial_seconds = *parsed;
      }
      if (auto value = arguments.get("--reconnect-max")) {
        auto parsed = parse_double(*value);
        if (!parsed || *parsed <= 0) {
          std::cerr << "--reconnect-max must be positive\n";
          return 2;
        }
        config.reconnect_max_seconds = *parsed;
      }
      if (auto value = arguments.get("--max-connect-failures")) {
        auto parsed = parse_uint(*value);
        if (!parsed || *parsed == 0) {
          std::cerr << "--max-connect-failures must be positive\n";
          return 2;
        }
        config.max_consecutive_connect_failures = static_cast<int>(*parsed);
      }
      config.console_logs = !arguments.has("--quiet");

      CaptureSummary summary;
      return run_capture(config, summary);
    }

    if (arguments.command == "replay") {
      const auto input = arguments.get("--input");
      if (!input) {
        std::cerr << "replay requires --input <raw_frames.jsonl>\n";
        return 2;
      }
      fs::path output = arguments.get_or("--output-dir", "");
      if (output.empty() && !arguments.positional.empty()) output = arguments.positional.front();
      if (output.empty()) {
        output = fs::path("data/replays") / default_run_id("replay");
      }
      return replay(*input, output);
    }

    if (arguments.command == "benchmark" || arguments.command == "synthetic") {
      const auto events_text = arguments.get("--events");
      std::uint64_t events = 100000;
      if (events_text) {
        auto parsed = parse_uint(*events_text);
        if (!parsed || *parsed == 0) {
          std::cerr << "--events must be positive\n";
          return 2;
        }
        events = *parsed;
      }
      if (arguments.command == "synthetic") {
        return synthetic(static_cast<std::size_t>(events));
      }
      const std::string mode = arguments.get_or("--mode", "all");
      const fs::path output = arguments.get_or("--output-dir", "data/benchmarks");
      return run_benchmark(static_cast<std::size_t>(events), mode, output);
    }

    if (arguments.command == "report") {
      const auto run_dir = arguments.get("--run-dir");
      if (!run_dir) {
        std::cerr << "report requires --run-dir <run directory>\n";
        return 2;
      }
      std::string output;
      const int code = run_report(*run_dir, true, output);
      std::cout << output;
      return code;
    }

    std::cerr << "invalid command: " << arguments.command << "\n";
    print_help();
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "fatal: " << error.what() << '\n';
    return 2;
  }
}
