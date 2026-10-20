#include "framework/nxtest.h"

#include "core/foundation/strings/format.h"

#include <cstring>
#include <string>

TEST_CASE("format: formats into an owning nx::string") {
  CHECK(nx::format("hello") == "hello");
  CHECK(nx::format("{}", 42) == "42");
  CHECK(nx::format("{} + {} = {}", 1, 2, 3) == "1 + 2 = 3");
  CHECK(nx::format("{1} {0}", "world", "hello") == "hello world");
  CHECK(nx::format("{:>5}", 7) == "    7");
  CHECK(nx::format("{:04d}", 42) == "0042");
  CHECK(nx::format("{:.3f}", 1.5) == "1.500");
  CHECK(nx::format("{{literal braces}}") == "{literal braces}");
  CHECK(nx::format("").empty());
}

TEST_CASE("format: accepts std::string, string_view and nx::string arguments") {
  CHECK(nx::format("{}", std::string("abc")) == "abc");
  CHECK(nx::format("{}", std::string_view("def")) == "def");

  const nx::string s("ghi");
  CHECK(nx::format("{}", s) == "ghi");
  CHECK(nx::format("[{:>5}]", s) == "[  ghi]");
  CHECK(nx::format("{}{}", s, s) == "ghighi");

  const nx::string big(std::string(300, 'z'));
  CHECK(nx::format("{}", big).size() == 300);
}

TEST_CASE("format: produces a result long enough to leave the SSO buffer") {
  const nx::string out = nx::format("{}", std::string(1000, 'x'));
  CHECK(out.size() == 1000);
  CHECK(out.front() == 'x');
  CHECK(out.back() == 'x');
  CHECK(out.c_str()[1000] == '\0');
}

TEST_CASE("format_to: writes into a fixed buffer and NUL-terminates") {
  char buf[64];

  const char *r = nx::format_to(buf, "hello {}", 42);
  CHECK(r == buf);
  CHECK(std::strcmp(buf, "hello 42") == 0);

  nx::format_to(buf, "{}-{}-{}", 1, 2, 3);
  CHECK(std::strcmp(buf, "1-2-3") == 0);

  nx::format_to(buf, "plain");
  CHECK(std::strcmp(buf, "plain") == 0);

  nx::format_to(buf, "");
  CHECK(buf[0] == '\0');
}

TEST_CASE("format_to: formatting failures stay inside the fixed buffer API") {
  char buf[64];
  const nx::string_view invalid = "{";
  int value = 42;
  auto store = std::make_format_args(value);
  const std::format_args args = store;
  static_assert(noexcept(nx::vformat_to(buf, invalid, args)));

  CHECK(nx::vformat_to(buf, invalid, args) == buf);
  CHECK(std::strcmp(buf, "[format error]") == 0);
}

TEST_CASE("format_to: truncates instead of overflowing the buffer") {
  char small[8];
  std::memset(small, 'Z', sizeof(small));

  nx::format_to(small, "{}", "abcdefghijklmnop");
  CHECK(std::strlen(small) == 7);
  CHECK(std::strcmp(small, "abcdefg") == 0);
  CHECK(small[7] == '\0');

  std::memset(small, 'Z', sizeof(small));
  nx::format_to(small, "{}", "1234567");
  CHECK(std::strcmp(small, "1234567") == 0);
  CHECK(small[7] == '\0');

  std::memset(small, 'Z', sizeof(small));
  nx::format_to(small, "{}", "123456");
  CHECK(std::strcmp(small, "123456") == 0);

  char tiny[5];
  nx::format_to(tiny, "{}{}{}", "aa", "bb", "cc");
  CHECK(std::strcmp(tiny, "aabb") == 0);

  char one[1];
  one[0] = 'Z';
  nx::format_to(one, "{}", "abc");
  CHECK(one[0] == '\0');
}

TEST_CASE("format_to: does not write past the end of the array") {
  struct {
    char buf[16];
    char guard[8];
  } s;
  std::memset(&s, 0x7E, sizeof(s));

  nx::format_to(s.buf, "{}", std::string(1000, 'q'));
  CHECK(std::strlen(s.buf) == 15);
  for (char c : s.guard)
    CHECK(c == 0x7E);
}

TEST_CASE("format: the nx::string formatter honours width and fill") {
  const nx::string s("ab");
  CHECK(nx::format("{:*<6}", s) == "ab****");
  CHECK(nx::format("{:*>6}", s) == "****ab");
  CHECK(nx::format("{:*^6}", s) == "**ab**");
  CHECK(nx::format("{:.1}", s) == "a");

  const nx::string empty;
  CHECK(nx::format("[{}]", empty) == "[]");
  CHECK(nx::format("[{:>3}]", empty) == "[   ]");
}

TEST_CASE("format: round-trips text containing NULs and UTF-8") {
  const nx::string utf8("caf\xC3\xA9");
  const nx::string out = nx::format("{}!", utf8);
  CHECK(out == "caf\xC3\xA9!");
  CHECK(out.code_point_count() == 5);

  const nx::string with_nul("a\0b", usize(3));
  CHECK(nx::format("{}", with_nul).size() == 3);
}

TEST_CASE("format_to: a cut at the end never splits a character") {
  char tight[5];
  nx::format_to(tight, "{}", "ab\u65e5");
  CHECK(std::strcmp(tight, "ab") == 0);

  char fits[6];
  nx::format_to(fits, "{}", "ab\u65e5");
  CHECK(std::strcmp(fits, "ab\u65e5") == 0);

  char emoji[4];
  nx::format_to(emoji, "{}", "\U0001F600");
  CHECK(std::strcmp(emoji, "") == 0);

  char ascii[4];
  nx::format_to(ascii, "{}", "abcdef");
  CHECK(std::strcmp(ascii, "abc") == 0);
}

TEST_CASE("utf8: complete_prefix drops only a sequence cut short") {
  using nx::utf8::complete_prefix;
  static_assert(complete_prefix("") == 0);
  static_assert(complete_prefix("abc") == 3);
  static_assert(complete_prefix("a\xE6\x97") == 1);
  static_assert(complete_prefix("a\xE6\x97\xA5") == 4);
  static_assert(complete_prefix("a\xF0") == 1);
  static_assert(complete_prefix("a\x97") == 2);
  CHECK(true);
}
