#include "marketpipe/json.hpp"

#include <charconv>
#include <cstdint>
#include <limits>

namespace marketpipe::json {
namespace {

constexpr std::size_t kMaxDepth = 64;

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  bool parse_document(Value& out, ParseError& error) {
    skip_ws();
    if (!parse_value(out, 0, error)) return false;
    skip_ws();
    if (pos_ != text_.size()) {
      error = {pos_, "trailing characters after JSON value"};
      return false;
    }
    return true;
  }

 private:
  void skip_ws() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool fail(ParseError& error, std::string message) {
    error = {pos_, std::move(message)};
    return false;
  }

  bool literal(std::string_view word) {
    if (text_.substr(pos_, word.size()) != word) return false;
    pos_ += word.size();
    return true;
  }

  bool parse_value(Value& out, std::size_t depth, ParseError& error) {
    if (depth > kMaxDepth) return fail(error, "maximum JSON nesting depth exceeded");
    if (pos_ >= text_.size()) return fail(error, "unexpected end of JSON input");
    switch (text_[pos_]) {
      case '{':
        return parse_object(out, depth, error);
      case '[':
        return parse_array(out, depth, error);
      case '"':
        out.type = Type::String;
        return parse_string(out.text, error);
      case 't':
        if (!literal("true")) return fail(error, "invalid literal");
        out.type = Type::Bool;
        out.boolean = true;
        return true;
      case 'f':
        if (!literal("false")) return fail(error, "invalid literal");
        out.type = Type::Bool;
        out.boolean = false;
        return true;
      case 'n':
        if (!literal("null")) return fail(error, "invalid literal");
        out.type = Type::Null;
        return true;
      default:
        if (text_[pos_] == '-' || (text_[pos_] >= '0' && text_[pos_] <= '9')) {
          return parse_number(out, error);
        }
        return fail(error, "unexpected character while parsing JSON value");
    }
  }

  bool parse_object(Value& out, std::size_t depth, ParseError& error) {
    out.type = Type::Object;
    ++pos_;  // '{'
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    for (;;) {
      skip_ws();
      if (pos_ >= text_.size() || text_[pos_] != '"') {
        return fail(error, "object key must be a string");
      }
      std::string key;
      if (!parse_string(key, error)) return false;
      skip_ws();
      if (pos_ >= text_.size() || text_[pos_] != ':') {
        return fail(error, "expected ':' after object key");
      }
      ++pos_;
      skip_ws();
      Value child;
      if (!parse_value(child, depth + 1, error)) return false;
      out.object.emplace_back(std::move(key), std::move(child));
      skip_ws();
      if (pos_ >= text_.size()) return fail(error, "unterminated object");
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return fail(error, "expected ',' or '}' in object");
    }
  }

  bool parse_array(Value& out, std::size_t depth, ParseError& error) {
    out.type = Type::Array;
    ++pos_;  // '['
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    for (;;) {
      skip_ws();
      Value child;
      if (!parse_value(child, depth + 1, error)) return false;
      out.array.push_back(std::move(child));
      skip_ws();
      if (pos_ >= text_.size()) return fail(error, "unterminated array");
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return fail(error, "expected ',' or ']' in array");
    }
  }

  static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  static void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7F) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool parse_hex4(std::uint32_t& out, ParseError& error) {
    if (pos_ + 4 > text_.size()) return fail(error, "truncated \\u escape");
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const int digit = hex_digit(text_[pos_ + static_cast<std::size_t>(i)]);
      if (digit < 0) return fail(error, "invalid \\u escape");
      value = (value << 4) | static_cast<std::uint32_t>(digit);
    }
    pos_ += 4;
    out = value;
    return true;
  }

  bool parse_string(std::string& out, ParseError& error) {
    ++pos_;  // opening quote
    out.clear();
    while (pos_ < text_.size()) {
      const unsigned char c = static_cast<unsigned char>(text_[pos_]);
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c < 0x20) return fail(error, "unescaped control character in string");
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        ++pos_;
        continue;
      }
      ++pos_;
      if (pos_ >= text_.size()) return fail(error, "truncated escape sequence");
      const char esc = text_[pos_++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          std::uint32_t cp = 0;
          if (!parse_hex4(cp, error)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
              return fail(error, "unpaired high surrogate in \\u escape");
            }
            pos_ += 2;
            std::uint32_t low = 0;
            if (!parse_hex4(low, error)) return false;
            if (low < 0xDC00 || low > 0xDFFF) {
              return fail(error, "invalid low surrogate in \\u escape");
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail(error, "unpaired low surrogate in \\u escape");
          }
          append_utf8(out, cp);
          break;
        }
        default:
          return fail(error, "invalid escape character");
      }
    }
    return fail(error, "unterminated string");
  }

  bool parse_number(Value& out, ParseError& error) {
    const std::size_t start = pos_;
    if (text_[pos_] == '-') ++pos_;
    if (pos_ >= text_.size()) return fail(error, "truncated number");
    if (text_[pos_] == '0') {
      ++pos_;
    } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    } else {
      return fail(error, "invalid number");
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
        return fail(error, "number fraction requires a digit");
      }
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
        return fail(error, "number exponent requires a digit");
      }
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    }
    out.type = Type::Number;
    out.text.assign(text_.substr(start, pos_ - start));
    return true;
  }

  std::string_view text_;
  std::size_t pos_{0};
};

}  // namespace

const Value* Value::find(std::string_view key) const {
  if (type != Type::Object) return nullptr;
  for (const auto& entry : object) {
    if (entry.first == key) return &entry.second;
  }
  return nullptr;
}

const Value* Value::find_ci(std::string_view key) const {
  if (type != Type::Object) return nullptr;
  auto lower = [](unsigned char c) {
    return static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
  };
  for (const auto& entry : object) {
    if (entry.first.size() != key.size()) continue;
    bool same = true;
    for (std::size_t i = 0; i < key.size(); ++i) {
      if (lower(static_cast<unsigned char>(entry.first[i])) !=
          lower(static_cast<unsigned char>(key[i]))) {
        same = false;
        break;
      }
    }
    if (same) return &entry.second;
  }
  return nullptr;
}

bool parse(std::string_view input, Value& out, ParseError& error) {
  out = Value{};
  error = ParseError{};
  Parser parser(input);
  return parser.parse_document(out, error);
}

std::string escape(std::string_view input) {
  static const char* kHex = "0123456789abcdef";
  std::string output;
  output.reserve(input.size() + 8);
  for (const char raw : input) {
    const unsigned char c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\b': output += "\\b"; break;
      case '\f': output += "\\f"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:
        if (c < 0x20) {
          output += "\\u00";
          output.push_back(kHex[(c >> 4) & 0x0F]);
          output.push_back(kHex[c & 0x0F]);
        } else {
          output.push_back(raw);
        }
        break;
    }
  }
  return output;
}

bool to_int64(const Value& value, long long& out) {
  if (!value.is_number()) return false;
  const char* begin = value.text.data();
  const char* end = begin + value.text.size();
  long long parsed = 0;
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) return false;
  out = parsed;
  return true;
}

bool to_uint64(const Value& value, unsigned long long& out) {
  if (!value.is_number()) return false;
  const char* begin = value.text.data();
  const char* end = begin + value.text.size();
  unsigned long long parsed = 0;
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) return false;
  out = parsed;
  return true;
}

bool to_double(const Value& value, double& out) {
  if (!value.is_number()) return false;
  const char* begin = value.text.data();
  const char* end = begin + value.text.size();
  double parsed = 0.0;
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) return false;
  if (parsed != parsed || parsed > std::numeric_limits<double>::max() ||
      parsed < -std::numeric_limits<double>::max()) {
    return false;
  }
  out = parsed;
  return true;
}

}  // namespace marketpipe::json
