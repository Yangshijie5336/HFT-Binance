#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct Failure : std::exception {
  std::string message;
  explicit Failure(std::string text) : message(std::move(text)) {}
  const char* what() const noexcept override { return message.c_str(); }
};

struct TestCase {
  std::string name;
  std::function<void()> function;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(std::string name, std::function<void()> function) {
    registry().push_back({std::move(name), std::move(function)});
  }
};

inline int run_all(const char* suite) {
  int failed = 0;
  for (const auto& test : registry()) {
    try {
      test.function();
      std::cout << "[PASS] " << suite << "." << test.name << '\n';
    } catch (const Failure& failure) {
      std::cout << "[FAIL] " << suite << "." << test.name << ": " << failure.what() << '\n';
      ++failed;
    } catch (const std::exception& error) {
      std::cout << "[FAIL] " << suite << "." << test.name << ": unexpected exception: "
                << error.what() << '\n';
      ++failed;
    } catch (...) {
      std::cout << "[FAIL] " << suite << "." << test.name << ": unknown exception\n";
      ++failed;
    }
  }
  std::cout << suite << ": " << registry().size() - static_cast<std::size_t>(failed) << "/"
            << registry().size() << " passed\n";
  return failed == 0 ? 0 : 1;
}

template <typename A, typename B>
std::string describe(const A& a, const B& b) {
  std::ostringstream out;
  out << "expected <" << a << "> == <" << b << ">";
  return out.str();
}

}  // namespace testing

#define MP_TEST(name)                                                        \
  static void test_##name();                                                 \
  static ::testing::Registrar registrar_##name(#name, test_##name);          \
  static void test_##name()

#define MP_CHECK(condition)                                                  \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::ostringstream message;                                            \
      message << __FILE__ << ":" << __LINE__ << " CHECK failed: " #condition; \
      throw ::testing::Failure(message.str());                               \
    }                                                                        \
  } while (false)

#define MP_CHECK_EQ(actual, expected)                                        \
  do {                                                                       \
    const auto& actual_value = (actual);                                     \
    const auto& expected_value = (expected);                                 \
    if (!(actual_value == expected_value)) {                                 \
      std::ostringstream message;                                            \
      message << __FILE__ << ":" << __LINE__ << " CHECK_EQ failed: "         \
              << ::testing::describe(actual_value, expected_value);          \
      throw ::testing::Failure(message.str());                               \
    }                                                                        \
  } while (false)

#define MP_REQUIRE(condition) MP_CHECK(condition)
