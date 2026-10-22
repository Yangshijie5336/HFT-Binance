#pragma once

// Small, dependency-free JSON DOM used for WebSocket frames.
//
// Design note: numbers are kept as the exact wire text.  Nothing in this
// parser converts a price or quantity through double, so a raw frame and a
// parsed trade both retain the exchange's decimal representation.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace marketpipe::json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
  Type type{Type::Null};
  bool boolean{false};
  // Number: exact source text. String: decoded UTF-8.
  std::string text;
  std::vector<Value> array;
  std::vector<std::pair<std::string, Value>> object;

  bool is_null() const { return type == Type::Null; }
  bool is_bool() const { return type == Type::Bool; }
  bool is_number() const { return type == Type::Number; }
  bool is_string() const { return type == Type::String; }
  bool is_array() const { return type == Type::Array; }
  bool is_object() const { return type == Type::Object; }

  // Returns nullptr when this is not an object or the key is absent.
  const Value* find(std::string_view key) const;
  const Value* find_ci(std::string_view key) const;
};

struct ParseError {
  std::size_t offset{0};
  std::string message;
};

// Parses exactly one JSON value, allowing surrounding whitespace.
// Rejects trailing non-whitespace, invalid escapes, control characters and
// nesting deeper than 64 levels.
bool parse(std::string_view input, Value& out, ParseError& error);

// Escapes a UTF-8 string for embedding in a JSON string literal.
std::string escape(std::string_view input);

// Convenience accessors. Return false when the type/number is unsuitable.
bool to_int64(const Value& value, long long& out);
bool to_uint64(const Value& value, unsigned long long& out);
bool to_double(const Value& value, double& out);

}  // namespace marketpipe::json
