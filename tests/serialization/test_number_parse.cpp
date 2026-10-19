#include "framework/nxtest.h"

#include "core/foundation/serialization/number_parse.h"

#include <atomic>
#include <clocale>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

std::from_chars_result parse(const char *text, f64 &out) {
  return nx::number::parse_f64(text, text + std::strlen(text), out);
}

class numeric_locale_restore {
public:
  numeric_locale_restore() {
    if (const char *current = std::setlocale(LC_NUMERIC, nullptr))
      m_original = current;
  }

  ~numeric_locale_restore() {
    if (!m_original.empty())
      (void)std::setlocale(LC_NUMERIC, m_original.c_str());
  }

private:
  std::string m_original;
};

} // namespace

TEST_CASE("number parse: decimal values and result pointers match from_chars") {
  const char text[] = "-12.5e1tail";
  f64 value = 0.0;
  const std::from_chars_result result =
      nx::number::parse_f64(text, text + sizeof(text) - 1u, value);

  CHECK(result.ec == std::errc{});
  CHECK(result.ptr == text + 7);
  CHECK(value == -125.0);

  value = 0.0;
  CHECK(parse(".25", value).ec == std::errc{});
  CHECK(value == 0.25);

  const char hexadecimal[] = "0x1";
  value = 42.0;
  const std::from_chars_result hexadecimal_result = nx::number::parse_f64(
      hexadecimal, hexadecimal + sizeof(hexadecimal) - 1u, value);
  CHECK(hexadecimal_result.ec == std::errc{});
  CHECK(hexadecimal_result.ptr == hexadecimal + 1);
  CHECK(value == 0.0);
}

TEST_CASE(
    "number parse: invalid prefixes are rejected without changing output") {
  for (const char *text : {"", " text", "+1.0"}) {
    f64 value = 42.0;
    const std::from_chars_result result = parse(text, value);
    CHECK(result.ec == std::errc::invalid_argument);
    CHECK(result.ptr == text);
    CHECK(value == 42.0);
  }
}

TEST_CASE("number parse: out of range is infinity or zero on every platform") {
  f64 value = 1.0;
  CHECK(parse("1e5000", value).ec == std::errc::result_out_of_range);
  CHECK(std::isinf(value));
  CHECK(value > 0.0);

  value = 1.0;
  CHECK(parse("-12.5e400", value).ec == std::errc::result_out_of_range);
  CHECK(std::isinf(value));
  CHECK(value < 0.0);

  value = 1.0;
  CHECK(parse("1e-5000", value).ec == std::errc::result_out_of_range);
  CHECK(value == 0.0);

  value = 1.0;
  CHECK(parse("-0.000001e-400", value).ec == std::errc::result_out_of_range);
  CHECK(value == 0.0);
  CHECK(std::signbit(value));

  value = 1.0;
  CHECK(parse("123456e-320", value).ec == std::errc{});
  CHECK(value > 0.0);

  value = 1.0;
  CHECK(parse("4.9e-324", value).ec == std::errc{});
  CHECK(value > 0.0);
}

TEST_CASE("number parse: long literals are not limited by the stack buffer") {
  const std::string text = "1." + std::string(600, '0') + "5";
  f64 value = 0.0;
  const std::from_chars_result result =
      nx::number::parse_f64(text.data(), text.data() + text.size(), value);

  CHECK(result.ec == std::errc{});
  CHECK(result.ptr == text.data() + text.size());
  CHECK(value == 1.0);
}

TEST_CASE("number parse: conversion is independent of the process locale") {
  const numeric_locale_restore restore;
  const char *candidates[] = {"German_Germany.1252", "de-DE", "de_DE.UTF-8",
                              "fr_FR.UTF-8", "French_France.1252"};
  for (const char *candidate : candidates) {
    if (std::setlocale(LC_NUMERIC, candidate) != nullptr)
      break;
  }

  const char *before_ptr = std::setlocale(LC_NUMERIC, nullptr);
  REQUIRE(before_ptr != nullptr);
  const std::string before = before_ptr;

  f64 value = 0.0;
  const std::from_chars_result result = parse("1.5", value);
  const char *after_ptr = std::setlocale(LC_NUMERIC, nullptr);

  REQUIRE(after_ptr != nullptr);
  CHECK(result.ec == std::errc{});
  CHECK(value == 1.5);
  CHECK(std::string(after_ptr) == before);
}

TEST_CASE("number parse: concurrent conversions do not share locale state") {
  std::atomic<u32> failures{0};
  std::vector<std::thread> threads;
  for (u32 thread_index = 0; thread_index < 8; ++thread_index) {
    threads.emplace_back([&failures] {
      for (u32 iteration = 0; iteration < 1000; ++iteration) {
        f64 value = 0.0;
        const std::from_chars_result result = parse("-1234.5e-2", value);
        if (result.ec != std::errc{} || value != -12.345)
          ++failures;
      }
    });
  }
  for (std::thread &thread : threads)
    thread.join();

  CHECK(failures.load() == 0u);
}
