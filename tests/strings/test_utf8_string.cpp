#include "framework/nxtest.h"
#include <cstring>

#include "core/foundation/strings/utf8_string.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char *kTwo = "\xC3\xA9";
constexpr const char *kThree = "\xE2\x82\xAC";
constexpr const char *kFour = "\xF0\x9F\x98\x80";
constexpr const char *kMixed = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z";

std::string long_text(usize n, char c = 'x') { return std::string(n, c); }

}

TEST_CASE("utf8: sequence_length and char_byte_count classify lead bytes") {
  CHECK(nx::utf8::sequence_length('a') == 1);
  CHECK(nx::utf8::sequence_length('\x7F') == 1);
  CHECK(nx::utf8::sequence_length('\xC3') == 2);
  CHECK(nx::utf8::sequence_length('\xE2') == 3);
  CHECK(nx::utf8::sequence_length('\xF0') == 4);
  CHECK(nx::utf8::sequence_length('\x80') == 0);
  CHECK(nx::utf8::sequence_length('\xBF') == 0);
  CHECK(nx::utf8::sequence_length('\xF8') == 0);
  CHECK(nx::utf8::sequence_length('\xFF') == 0);

  for (int i = 0; i < 256; ++i) {
    const char c = static_cast<char>(i);
    CHECK(nx::utf8::char_byte_count(c) == nx::utf8::sequence_length(c));
  }
}

TEST_CASE("utf8: is_continuation_byte recognises 10xxxxxx") {
  for (int i = 0x80; i <= 0xBF; ++i)
    CHECK(nx::utf8::is_continuation_byte(static_cast<char>(i)));
  for (int i = 0x00; i < 0x80; ++i)
    CHECK_FALSE(nx::utf8::is_continuation_byte(static_cast<char>(i)));
  for (int i = 0xC0; i <= 0xFF; ++i)
    CHECK_FALSE(nx::utf8::is_continuation_byte(static_cast<char>(i)));
}

TEST_CASE("utf8: codepoint and surrogate predicates") {
  CHECK(nx::utf8::is_valid_codepoint(0));
  CHECK(nx::utf8::is_valid_codepoint(0x7F));
  CHECK(nx::utf8::is_valid_codepoint(0xD7FF));
  CHECK_FALSE(nx::utf8::is_valid_codepoint(0xD800));
  CHECK_FALSE(nx::utf8::is_valid_codepoint(0xDBFF));
  CHECK_FALSE(nx::utf8::is_valid_codepoint(0xDFFF));
  CHECK(nx::utf8::is_valid_codepoint(0xE000));
  CHECK(nx::utf8::is_valid_codepoint(0x10FFFF));
  CHECK_FALSE(nx::utf8::is_valid_codepoint(0x110000));

  CHECK(nx::utf8::is_surrogate(0xD800));
  CHECK(nx::utf8::is_surrogate(0xDFFF));
  CHECK_FALSE(nx::utf8::is_surrogate(0xD7FF));
  CHECK_FALSE(nx::utf8::is_surrogate(0xE000));

  CHECK(nx::utf8::is_high_surrogate(0xD800));
  CHECK(nx::utf8::is_high_surrogate(0xDBFF));
  CHECK_FALSE(nx::utf8::is_high_surrogate(0xDC00));
  CHECK(nx::utf8::is_high_surrogate_invalid(0xDC00));
  CHECK_FALSE(nx::utf8::is_high_surrogate_invalid(0xD800));

  CHECK(nx::utf8::is_low_surrogate(0xDC00));
  CHECK(nx::utf8::is_low_surrogate(0xDFFF));
  CHECK_FALSE(nx::utf8::is_low_surrogate(0xDBFF));
  CHECK(nx::utf8::is_low_surrogate_invalid(0xDBFF));
  CHECK_FALSE(nx::utf8::is_low_surrogate_invalid(0xDC00));
}

TEST_CASE("utf8: decode_char reads each sequence length") {
  char32_t cp = 0;
  usize read = 0;

  REQUIRE(nx::utf8::decode_char("a", 1, cp, read));
  CHECK(cp == U'a');
  CHECK(read == 1);

  REQUIRE(nx::utf8::decode_char(kTwo, 2, cp, read));
  CHECK(cp == 0x00E9);
  CHECK(read == 2);

  REQUIRE(nx::utf8::decode_char(kThree, 3, cp, read));
  CHECK(cp == 0x20AC);
  CHECK(read == 3);

  REQUIRE(nx::utf8::decode_char(kFour, 4, cp, read));
  CHECK(cp == 0x1F600);
  CHECK(read == 4);

  REQUIRE(nx::utf8::decode_char("\0", 1, cp, read));
  CHECK(cp == 0);
  CHECK(read == 1);
}

TEST_CASE("utf8: decode_char rejects malformed input") {
  char32_t cp = 0;
  usize read = 0;

  CHECK_FALSE(nx::utf8::decode_char("", 0, cp, read));

  CHECK_FALSE(nx::utf8::decode_char("\x80", 1, cp, read));
  CHECK_FALSE(nx::utf8::decode_char("\xFF", 1, cp, read));

  CHECK_FALSE(nx::utf8::decode_char("\xC3\x41", 2, cp, read));
  CHECK_FALSE(nx::utf8::decode_char("\xE2\x82\x41", 3, cp, read));
  CHECK_FALSE(nx::utf8::decode_char("\xF0\x9F\x98\x41", 4, cp, read));
}

TEST_CASE("utf8: decode_char refuses to read past the end of the buffer") {
  const char two[] = {'\xC3'};
  const char three[] = {'\xE2', '\x82'};
  const char four[] = {'\xF0', '\x9F', '\x98'};

  char32_t cp = 0;
  usize read = 0;
  CHECK_FALSE(nx::utf8::decode_char(two, 1, cp, read));
  CHECK_FALSE(nx::utf8::decode_char(three, 1, cp, read));
  CHECK_FALSE(nx::utf8::decode_char(three, 2, cp, read));
  CHECK_FALSE(nx::utf8::decode_char(four, 1, cp, read));
  CHECK_FALSE(nx::utf8::decode_char(four, 2, cp, read));
  CHECK_FALSE(nx::utf8::decode_char(four, 3, cp, read));

  CHECK_FALSE(nx::utf8::validate_utf8(two, 1));
  CHECK_FALSE(nx::utf8::validate_utf8(three, 2));
  CHECK_FALSE(nx::utf8::validate_utf8(four, 3));
}

TEST_CASE("utf8: encode_char produces the canonical byte sequence") {
  char buf[4];
  usize len = 0;

  REQUIRE(nx::utf8::encode_char(U'a', buf, len));
  CHECK(len == 1);
  CHECK(buf[0] == 'a');

  REQUIRE(nx::utf8::encode_char(char32_t(0x00E9), buf, len));
  CHECK(len == 2);
  CHECK(std::string(buf, len) == kTwo);

  REQUIRE(nx::utf8::encode_char(char32_t(0x20AC), buf, len));
  CHECK(len == 3);
  CHECK(std::string(buf, len) == kThree);

  REQUIRE(nx::utf8::encode_char(char32_t(0x1F600), buf, len));
  CHECK(len == 4);
  CHECK(std::string(buf, len) == kFour);

  REQUIRE(nx::utf8::encode_char(char32_t(0x7F), buf, len));
  CHECK(len == 1);
  REQUIRE(nx::utf8::encode_char(char32_t(0x80), buf, len));
  CHECK(len == 2);
  REQUIRE(nx::utf8::encode_char(char32_t(0x7FF), buf, len));
  CHECK(len == 2);
  REQUIRE(nx::utf8::encode_char(char32_t(0x800), buf, len));
  CHECK(len == 3);
  REQUIRE(nx::utf8::encode_char(char32_t(0xFFFF), buf, len));
  CHECK(len == 3);
  REQUIRE(nx::utf8::encode_char(char32_t(0x10000), buf, len));
  CHECK(len == 4);
  REQUIRE(nx::utf8::encode_char(char32_t(0x10FFFF), buf, len));
  CHECK(len == 4);

  CHECK_FALSE(nx::utf8::encode_char(char32_t(0xD800), buf, len));
  CHECK_FALSE(nx::utf8::encode_char(char32_t(0xDFFF), buf, len));
  CHECK_FALSE(nx::utf8::encode_char(char32_t(0x110000), buf, len));
}

TEST_CASE("utf8: encode_char handles UTF-16 units and surrogate pairs") {
  char buf[4];
  usize len = 0;

  REQUIRE(nx::utf8::encode_char(char16_t(0x20AC), buf, len));
  CHECK(len == 3);
  CHECK(std::string(buf, len) == kThree);

  CHECK_FALSE(nx::utf8::encode_char(char16_t(0xD800), buf, len));
  CHECK_FALSE(nx::utf8::encode_char(char16_t(0xDC00), buf, len));

  REQUIRE(nx::utf8::encode_char(char16_t(0xD83D), char16_t(0xDE00), buf, len));
  CHECK(len == 4);
  CHECK(std::string(buf, len) == kFour);

  CHECK_FALSE(
      nx::utf8::encode_char(char16_t(0x0041), char16_t(0xDE00), buf, len));
  CHECK_FALSE(
      nx::utf8::encode_char(char16_t(0xD83D), char16_t(0x0041), buf, len));
}

TEST_CASE("utf8: encode/decode round-trip over the whole scalar range") {
  char buf[4];
  usize len = 0, read = 0;
  char32_t back = 0;

  for (char32_t cp = 0; cp <= 0x10FFFF; cp += 97) {
    if (!nx::utf8::is_valid_codepoint(cp))
      continue;
    REQUIRE(nx::utf8::encode_char(cp, buf, len));
    REQUIRE(nx::utf8::decode_char(buf, len, back, read));
    REQUIRE(back == cp);
    REQUIRE(read == len);
  }
}

TEST_CASE(
    "utf8: validate_utf8 accepts well-formed and rejects malformed text") {
  CHECK(nx::utf8::validate_utf8("", 0));
  CHECK(nx::utf8::validate_utf8("hello", 5));
  CHECK(nx::utf8::validate_utf8(kMixed, 11));

  const char bad_lead[] = {'\x80'};
  CHECK_FALSE(nx::utf8::validate_utf8(bad_lead, 1));
  const char bad_cont[] = {'\xC3', 'A'};
  CHECK_FALSE(nx::utf8::validate_utf8(bad_cont, 2));
}

TEST_CASE("utf8: for_each_codepoint decodes and substitutes U+FFFD") {
  std::vector<char32_t> cps;
  nx::utf8::for_each_codepoint(kMixed, 11,
                               [&](char32_t cp) { cps.push_back(cp); });
  REQUIRE(cps.size() == 5);
  CHECK(cps[0] == U'a');
  CHECK(cps[1] == 0x00E9);
  CHECK(cps[2] == 0x20AC);
  CHECK(cps[3] == 0x1F600);
  CHECK(cps[4] == U'z');

  const char bad[] = {'a', '\xFF', 'b'};
  cps.clear();
  nx::utf8::for_each_codepoint(bad, 3, [&](char32_t cp) { cps.push_back(cp); });
  REQUIRE(cps.size() == 3);
  CHECK(cps[0] == U'a');
  CHECK(cps[1] == 0xFFFD);
  CHECK(cps[2] == U'b');

  const char trunc[] = {'a', '\xE2', '\x82'};
  cps.clear();
  nx::utf8::for_each_codepoint(trunc, 3,
                               [&](char32_t cp) { cps.push_back(cp); });
  REQUIRE(cps.size() == 3);
  CHECK(cps[0] == U'a');
  CHECK(cps[1] == 0xFFFD);
  CHECK(cps[2] == 0xFFFD);

  int calls = 0;
  nx::utf8::for_each_codepoint("", 0, [&](char32_t) { ++calls; });
  CHECK(calls == 0);
}

TEST_CASE("utf8: next_char and prev_char step whole characters") {
  const nx::string_view mixed(kMixed, 11);

  CHECK(nx::utf8::next_char(mixed, 0) == 1);
  CHECK(nx::utf8::next_char(mixed, 1) == 3);
  CHECK(nx::utf8::next_char(mixed, 3) == 6);
  CHECK(nx::utf8::next_char(mixed, 6) == 10);
  CHECK(nx::utf8::next_char(mixed, 10) == 11);

  CHECK(nx::utf8::prev_char(mixed, 11) == 10);
  CHECK(nx::utf8::prev_char(mixed, 10) == 6);
  CHECK(nx::utf8::prev_char(mixed, 6) == 3);
  CHECK(nx::utf8::prev_char(mixed, 3) == 1);
  CHECK(nx::utf8::prev_char(mixed, 1) == 0);

  CHECK(nx::utf8::next_char(mixed, 7) == 10);
  CHECK(nx::utf8::prev_char(mixed, 8) == 6);

  CHECK(nx::utf8::next_char(mixed, 11) == 11);
  CHECK(nx::utf8::next_char(mixed, 99) == 11);
  CHECK(nx::utf8::prev_char(mixed, 0) == 0);
  CHECK(nx::utf8::prev_char(mixed, 99) == 10);
  CHECK(nx::utf8::next_char("", 0) == 0);
  CHECK(nx::utf8::prev_char("", 0) == 0);
}

TEST_CASE("utf8: count_chars counts characters, not bytes") {
  CHECK(nx::utf8::count_chars("") == 0);
  CHECK(nx::utf8::count_chars("hello") == 5);
  CHECK(nx::utf8::count_chars(nx::string_view(kMixed, 11)) == 5);

  CHECK(nx::utf8::count_chars(nx::string_view("a\xFF", 2)) == 2);

  const nx::string_view mixed(kMixed, 11);
  usize walked = 0;
  for (usize at = 0; at < mixed.size(); at = nx::utf8::next_char(mixed, at))
    ++walked;
  CHECK(walked == nx::utf8::count_chars(mixed));
}

TEST_CASE("utf8: char_at decodes one character, or says 0") {
  const nx::string_view mixed(kMixed, 11);
  CHECK(nx::utf8::char_at(mixed, 0) == U'a');
  CHECK(nx::utf8::char_at(mixed, 1) == 0x00E9);
  CHECK(nx::utf8::char_at(mixed, 3) == 0x20AC);
  CHECK(nx::utf8::char_at(mixed, 6) == 0x1F600);
  CHECK(nx::utf8::char_at(mixed, 10) == U'z');

  CHECK(nx::utf8::char_at(mixed, 11) == 0);
  CHECK(nx::utf8::char_at("", 0) == 0);
  CHECK(nx::utf8::char_at(nx::string_view("\x80", 1), 0) == 0);
  CHECK(nx::utf8::char_at(nx::string_view("\xC3", 1), 0) == 0);
  CHECK(nx::utf8::char_at(nx::string_view("\xC3\x41", 2), 0) == 0);
}

TEST_CASE("utf8: append_char writes the encoding, or nothing at all") {
  nx::string out;
  CHECK(nx::utf8::append_char(out, U'a'));
  CHECK(nx::utf8::append_char(out, 0x00E9));
  CHECK(nx::utf8::append_char(out, 0x20AC));
  CHECK(nx::utf8::append_char(out, 0x1F600));
  CHECK(nx::utf8::append_char(out, U'z'));
  CHECK(out.view() == nx::string_view(kMixed, 11));

  const usize before = out.size();
  CHECK_FALSE(nx::utf8::append_char(out, char32_t(0xD800)));
  CHECK_FALSE(nx::utf8::append_char(out, char32_t(0x110000)));
  CHECK(out.size() == before);
}

TEST_CASE("string: default-constructed is empty, inline and NUL-terminated") {
  const nx::string s;
  CHECK(s.empty());
  CHECK(s.size() == 0);
  CHECK(s.length() == 0);
  CHECK(s.c_str()[0] == '\0');
  CHECK(s.begin() == s.end());
  CHECK(s.capacity() > 0);
  CHECK(s.view().empty());
  CHECK(s.max_size() > 0);

  CHECK(sizeof(nx::string) == 3 * sizeof(void *));
}

TEST_CASE("string: construction from char pointers, lengths and repeats") {
  const nx::string a("hello");
  CHECK(a.size() == 5);
  CHECK(a == "hello");
  CHECK(a.c_str()[5] == '\0');

  const nx::string b("hello world", usize(5));
  CHECK(b == "hello");

  const nx::string c(usize(3), 'x');
  CHECK(c == "xxx");

  const nx::string d('q');
  CHECK(d == "q");
  CHECK(d.size() == 1);

  const nx::string e("");
  CHECK(e.empty());

  const nx::string f(usize(0), 'x');
  CHECK(f.empty());

  const nx::string g("a\0b", usize(3));
  CHECK(g.size() == 3);
  CHECK(g[1] == '\0');
  CHECK(g[2] == 'b');
}

TEST_CASE("string: construction transcodes from UTF-16 and UTF-32 sources") {
  const nx::string from16(u"hé€");
  CHECK(from16 == std::string("h") + kTwo + kThree);
  CHECK(from16.code_point_count() == 3);

  const nx::string from32(U"hé€\U0001F600");
  CHECK(from32.code_point_count() == 4);
  CHECK(from32.is_valid_utf8());

  const nx::string emoji(u"\U0001F600");
  CHECK(emoji.size() == 4);
  CHECK(emoji == kFour);
  CHECK(emoji.code_point_count() == 1);

  const nx::string from8(u8"café");
  CHECK(from8.code_point_count() == 4);
  CHECK(from8.size() == 5);

  const nx::string fromw(L"hé");
  CHECK(fromw.code_point_count() == 2);

  const nx::string s1{std::string("abc")};
  CHECK(s1 == "abc");
  const nx::string s2{std::u16string(u"€")};
  CHECK(s2 == kThree);
  const nx::string s3{std::u32string(U"\U0001F600")};
  CHECK(s3 == kFour);
}

TEST_CASE("string: a lone UTF-16 surrogate becomes U+FFFD") {
  const char16_t lone[] = {0xD800, 0};
  const nx::string s(lone, usize(1));
  CHECK(s.code_point_count() == 1);
  CHECK(s.size() == 3);
  CHECK(s.is_valid_utf8());

  const char16_t bad_pair[] = {0xD800, 0x0041};
  const nx::string t(bad_pair, usize(2));
  CHECK(t.code_point_count() == 2);
  CHECK(t.view().ends_with("A"));

  const char32_t bad32[] = {0x110000};
  const nx::string u(bad32, usize(1));
  CHECK(u.code_point_count() == 1);
  CHECK(u.size() == 3);
}

TEST_CASE("string: small strings stay inline and long ones spill to the heap") {
  nx::string s;
  const usize sso_cap = s.capacity();
  CHECK(sso_cap >= 20);

  for (usize i = 0; i < sso_cap; ++i)
    s.push_back('a');
  CHECK(s.size() == sso_cap);
  CHECK(s.capacity() == sso_cap);

  s.push_back('b');
  CHECK(s.size() == sso_cap + 1);
  CHECK(s.capacity() > sso_cap);
  CHECK(s.back() == 'b');
  CHECK(s.front() == 'a');
  CHECK(s.c_str()[s.size()] == '\0');
}

TEST_CASE("string: element access, iteration and bounds checking") {
  nx::string s("hello");

  CHECK(s[0] == 'h');
  CHECK(s[4] == 'o');
  s[0] = 'H';
  CHECK(s == "Hello");

  CHECK(s.at(1) == 'e');
  s.at(1) = 'E';
  CHECK(s == "HEllo");
  CHECK_THROWS_AS(s.at(5), std::out_of_range);
  CHECK_THROWS_AS(s.at(usize(-1)), std::out_of_range);

  const nx::string &cs = s;
  CHECK(cs[0] == 'H');
  CHECK(cs.at(0) == 'H');
  CHECK_THROWS_AS(cs.at(5), std::out_of_range);

  CHECK(s.front() == 'H');
  CHECK(s.back() == 'o');
  s.front() = 'h';
  s.back() = 'O';
  CHECK(s == "hEllO");
  CHECK(cs.front() == 'h');
  CHECK(cs.back() == 'O');

  CHECK(std::string(s.begin(), s.end()) == "hEllO");
  CHECK(std::string(s.cbegin(), s.cend()) == "hEllO");
  CHECK(std::string(s.rbegin(), s.rend()) == "OllEh");
  CHECK(std::string(cs.begin(), cs.end()) == "hEllO");
  CHECK(std::string(cs.rbegin(), cs.rend()) == "OllEh");

  for (char &c : s)
    c = 'z';
  CHECK(s == "zzzzz");

  nx::string e;
  CHECK_THROWS_AS(e.at(0), std::out_of_range);
}

TEST_CASE("string: push_back, pop_back, clear and resize") {
  nx::string s;
  for (char c = 'a'; c <= 'e'; ++c)
    s.push_back(c);
  CHECK(s == "abcde");

  s.pop_back();
  CHECK(s == "abcd");
  CHECK(s.c_str()[4] == '\0');

  s.resize(2);
  CHECK(s == "ab");
  s.resize(5);
  CHECK(s.size() == 5);
  CHECK(s[4] == '\0');
  s.resize(2);
  s.resize(5, 'z');
  CHECK(s == "abzzz");
  s.resize(5, 'q');
  CHECK(s == "abzzz");
  s.resize(0);
  CHECK(s.empty());

  s.resize(1000, 'k');
  CHECK(s.size() == 1000);
  CHECK(s[999] == 'k');
  CHECK(s.c_str()[1000] == '\0');

  s.clear();
  CHECK(s.empty());
  CHECK(s.c_str()[0] == '\0');
}

TEST_CASE("string: append in all its overloads") {
  nx::string s("a");

  s.append("bc");
  CHECK(s == "abc");
  s.append("defg", usize(2));
  CHECK(s == "abcde");
  s.append(usize(2), 'f');
  CHECK(s == "abcdeff");
  s.append(nx::string("gh"));
  CHECK(s == "abcdeffgh");
  s.append(std::string("ij"));
  CHECK(s == "abcdeffghij");
  s.append(nx::string_view("kl"));
  CHECK(s == "abcdeffghijkl");

  nx::string t;
  t.append(u"€");
  CHECK(t == kThree);
  t.clear();
  t.append(U"\U0001F600");
  CHECK(t == kFour);
  t.clear();
  t.append(usize(2), char32_t(0x00E9));
  CHECK(t == std::string(kTwo) + kTwo);

  nx::string u;
  u.append_codepoint(U'a');
  u.append_codepoint(0x20AC);
  CHECK(u == std::string("a") + kThree);
  u.clear();
  u.append_codepoint(0xD800);
  CHECK(u.size() == 3);
  CHECK(u.code_point_count() == 1);

  nx::string v("x");
  v.append("", usize(0));
  v.append(usize(0), 'y');
  CHECK(v == "x");
}

TEST_CASE("string: operator+= mirrors append") {
  nx::string s;
  s += "ab";
  s += 'c';
  s += nx::string("de");
  s += std::string("fg");
  s += nx::string_view("hi");
  CHECK(s == "abcdefghi");

  nx::string t;
  t += char32_t(0x20AC);
  CHECK(t == kThree);
}

TEST_CASE("string: self-append is handled correctly") {
  nx::string s("abc");
  s.append(s);
  CHECK(s == "abcabc");

  nx::string t("ab");
  for (int i = 0; i < 8; ++i)
    t.append(t);
  CHECK(t.size() == 2u * 256u);
  CHECK(t.view().starts_with("abab"));
  CHECK(t.view().ends_with("abab"));
  for (usize i = 0; i < t.size(); ++i)
    REQUIRE(t[i] == (i % 2 == 0 ? 'a' : 'b'));
}

TEST_CASE("string: insert places text at a byte offset") {
  nx::string s("ac");
  s.insert(1, "b");
  CHECK(s == "abc");

  s.insert(0, "0");
  CHECK(s == "0abc");
  s.insert(s.size(), "z");
  CHECK(s == "0abcz");

  s.insert(2, "");
  CHECK(s == "0abcz");

  nx::string t("ab");
  t.insert(1, std::string(100, 'x'));
  CHECK(t.size() == 102);
  CHECK(t.front() == 'a');
  CHECK(t.back() == 'b');
  CHECK(t[1] == 'x');

  nx::string u("abc");
  u.insert(1, u.view());
  CHECK(u == "aabcbc");
}

TEST_CASE("string: erase removes a byte range") {
  nx::string s("abcdef");

  s.erase(1, 2);
  CHECK(s == "adef");

  s.erase(2, 100);
  CHECK(s == "ad");

  nx::string t("abcdef");
  t.erase(3);
  CHECK(t == "abc");

  t.erase();
  CHECK(t.empty());
  CHECK(t.c_str()[0] == '\0');

  nx::string u("abc");
  u.erase(1, 0);
  CHECK(u == "abc");
  u.erase(3, 5);
  CHECK(u == "abc");
}

TEST_CASE("string: reserve and shrink_to_fit") {
  nx::string s("hi");
  const usize sso = s.capacity();

  s.reserve(1000);
  CHECK(s.capacity() >= 1000);
  CHECK(s == "hi");

  s.shrink_to_fit();
  CHECK(s.capacity() == sso);
  CHECK(s == "hi");
  CHECK(s.c_str()[2] == '\0');

  nx::string t(long_text(500));
  t.reserve(5000);
  CHECK(t.capacity() >= 5000);
  t.shrink_to_fit();
  CHECK(t.capacity() >= 500);
  CHECK(t.capacity() < 5000);
  CHECK(t.size() == 500);
  CHECK(t.back() == 'x');

  nx::string u("x");
  u.reserve(1);
  CHECK(u.capacity() == sso);
  u.shrink_to_fit();
  CHECK(u.capacity() == sso);
}

TEST_CASE("string: copy and move semantics, inline and heap") {
  SUBCASE("inline") {
    nx::string a("short");
    const nx::string b = a;
    CHECK(b == "short");
    CHECK(a == "short");

    nx::string c;
    c = a;
    CHECK(c == "short");
    c[0] = 'S';
    CHECK(a == "short");

    const nx::string d = std::move(a);
    CHECK(d == "short");
    CHECK(a.empty());
  }
  SUBCASE("heap") {
    nx::string a(long_text(500, 'a'));
    const nx::string b = a;
    CHECK(b.size() == 500);
    CHECK(b.data() != a.data());

    nx::string c("x");
    c = a;
    CHECK(c.size() == 500);
    CHECK(c.data() != a.data());

    const char *raw = a.data();
    const nx::string d = std::move(a);
    CHECK(d.size() == 500);
    CHECK(d.data() == raw);
    CHECK(a.empty());

    nx::string e(long_text(300, 'e'));
    e = std::move(c);
    CHECK(e.size() == 500);
    CHECK(c.empty());
  }
}

TEST_CASE("string: self-assignment leaves the contents intact") {
  nx::string s("hello world this is long enough to be on the heap");
  const nx::string *alias = &s;
  s = *alias;
  CHECK(s == "hello world this is long enough to be on the heap");

  nx::string *malias = &s;
  s = std::move(*malias);
  CHECK(s == "hello world this is long enough to be on the heap");

  nx::string t("short");
  const nx::string *talias = &t;
  t = *talias;
  CHECK(t == "short");
}

TEST_CASE("string: assign replaces the contents") {
  nx::string s("original");

  s.assign("new", usize(3));
  CHECK(s == "new");
  s.assign("newer");
  CHECK(s == "newer");
  s.assign(std::string("newest"));
  CHECK(s == "newest");
  s.assign(u"€");
  CHECK(s == kThree);

  nx::string t("abcdef");
  t.assign(t.data() + 2, usize(3));
  CHECK(t == "cde");

  nx::string u;
  u = "abc";
  CHECK(u == "abc");
  u = std::string("de");
  CHECK(u == "de");
  u = 'f';
  CHECK(u == "f");
  u = char32_t(0x20AC);
  CHECK(u == kThree);
}

TEST_CASE("string: swap exchanges contents across storage kinds") {
  nx::string a("short");
  nx::string b(long_text(500, 'b'));

  a.swap(b);
  CHECK(a.size() == 500);
  CHECK(b == "short");

  swap(a, b);
  CHECK(a == "short");
  CHECK(b.size() == 500);

  nx::string c("one"), d("two");
  c.swap(d);
  CHECK(c == "two");
  CHECK(d == "one");

  nx::string e(long_text(100, 'e')), f(long_text(200, 'f'));
  e.swap(f);
  CHECK(e.size() == 200);
  CHECK(f.size() == 100);

  nx::string g("x"), h;
  g.swap(h);
  CHECK(g.empty());
  CHECK(h == "x");
}

TEST_CASE(
    "string: view, string, u8string, u16string and u32string conversions") {
  const nx::string s(kMixed);

  CHECK(s.view() == kMixed);
  CHECK(s.view().size() == 11);
  const nx::string_view implicit = s;
  CHECK(implicit == kMixed);
  CHECK(implicit.data() == s.data());

  CHECK(s.string() == kMixed);
  CHECK(s.u8string().size() == 11);

  const std::u16string u16 = s.u16string();
  CHECK(u16.size() == 6);
  CHECK(u16[0] == u'a');
  CHECK(u16[1] == 0x00E9);
  CHECK(u16[2] == 0x20AC);
  CHECK(u16[3] == 0xD83D);
  CHECK(u16[4] == 0xDE00);
  CHECK(u16[5] == u'z');

  const std::u32string u32 = s.u32string();
  CHECK(u32.size() == 5);
  CHECK(u32[3] == 0x1F600);

  const nx::string e;
  CHECK(e.string().empty());
  CHECK(e.u16string().empty());
  CHECK(e.u32string().empty());
}

TEST_CASE("string: substr slices by byte offset") {
  const nx::string s("hello world");

  CHECK(s.substr(0, 5) == "hello");
  CHECK(s.substr(6) == "world");
  CHECK(s.substr(6, 100) == "world");
  CHECK(s.substr(11).empty());
  CHECK(s.substr().size() == 11);
  CHECK(s.substr(3, 0).empty());

  CHECK_THROWS_AS(s.substr(12), std::out_of_range);
}

TEST_CASE("string: search and comparison helpers") {
  const nx::string s("hello world");

  CHECK(s.find("world") == 6);
  CHECK(s.find("o") == 4);
  CHECK(s.find("o", 5) == 7);
  CHECK(s.find("missing") == nx::string::npos);
  CHECK(s.find('w') == 6);
  CHECK(s.find('z') == nx::string::npos);
  CHECK(s.rfind("o") == 7);
  CHECK(s.rfind("l") == 9);

  CHECK(s.starts_with("hello"));
  CHECK_FALSE(s.starts_with("world"));
  CHECK(s.ends_with("world"));
  CHECK_FALSE(s.ends_with("hello"));
  CHECK(s.contains("lo w"));
  CHECK_FALSE(s.contains("zz"));

  CHECK(s.compare("hello world") == 0);
  CHECK(s.compare("hello") > 0);
  CHECK(s.compare("z") < 0);
}

TEST_CASE("string: comparison operators") {
  const nx::string a("abc");
  const nx::string b("abc");
  const nx::string c("abd");

  CHECK(a == b);
  CHECK(a != c);
  CHECK(a < c);
  CHECK(c > a);
  CHECK(a <= b);
  CHECK(a >= b);
  CHECK((a <=> b) == std::strong_ordering::equal);
  CHECK((a <=> c) == std::strong_ordering::less);

  CHECK(a == "abc");
  CHECK(a == nx::string_view("abc"));
  CHECK(a != "abd");
  CHECK((a <=> nx::string_view("abd")) == std::strong_ordering::less);

  const nx::string long_a(long_text(100, 'z'));
  const nx::string long_b(long_text(100, 'z'));
  CHECK(long_a == long_b);
  CHECK(long_a.data() != long_b.data());
}

TEST_CASE("string: operator+ concatenates without mutating the operands") {
  const nx::string a("foo");
  const nx::string b("bar");

  CHECK(a + b == "foobar");
  CHECK(a + "baz" == "foobaz");
  CHECK("baz" + a == "bazfoo");
  CHECK(a + nx::string_view("qux") == "fooqux");
  CHECK(nx::string_view("qux") + a == "quxfoo");
  CHECK(a + '!' == "foo!");

  CHECK(a == "foo");
  CHECK(b == "bar");

  const nx::string big =
      nx::string(long_text(300, 'a')) + nx::string_view(long_text(300, 'b'));
  CHECK(big.size() == 600);
  CHECK(big[0] == 'a');
  CHECK(big[599] == 'b');
}

TEST_CASE("string: code_point_count counts characters, not bytes") {
  CHECK(nx::string().code_point_count() == 0);
  CHECK(nx::string("hello").code_point_count() == 5);
  CHECK(nx::string(kTwo).code_point_count() == 1);
  CHECK(nx::string(kThree).code_point_count() == 1);
  CHECK(nx::string(kFour).code_point_count() == 1);

  const nx::string mixed(kMixed);
  CHECK(mixed.size() == 11);
  CHECK(mixed.code_point_count() == 5);

  const nx::string bad("a\xFF"
                       "b",
                       usize(3));
  CHECK(bad.code_point_count() == 3);
}

TEST_CASE("string: is_valid_utf8 rejects malformed byte sequences") {
  CHECK(nx::string().is_valid_utf8());
  CHECK(nx::string("hello").is_valid_utf8());
  CHECK(nx::string(kMixed).is_valid_utf8());

  CHECK_FALSE(nx::string("\x80", usize(1)).is_valid_utf8());
  CHECK_FALSE(nx::string("\xFF", usize(1)).is_valid_utf8());
  CHECK_FALSE(nx::string("\xC3", usize(1)).is_valid_utf8());
  CHECK_FALSE(nx::string("\xE2\x82", usize(2)).is_valid_utf8());
  CHECK_FALSE(nx::string("\xC3\x41", usize(2)).is_valid_utf8());
}

TEST_CASE("string: get_allocator is callable") {
  const nx::string s("x");
  CHECK(s.get_allocator() == s.get_allocator());
}

TEST_CASE("string: survives a long mixed workload against std::string") {
  nx::string s;
  std::string model;

  u64 state = 0xC0FFEE1234567890ull;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  for (int step = 0; step < 10000; ++step) {
    const u64 r = next();
    const char c = static_cast<char>('a' + (r % 26));
    const usize pos =
        model.empty() ? 0 : static_cast<usize>(r >> 8) % model.size();

    switch (r % 6) {
    case 0:
    case 1:
      s.push_back(c);
      model.push_back(c);
      break;
    case 2:
      if (!model.empty()) {
        s.pop_back();
        model.pop_back();
      }
      break;
    case 3: {
      const std::string chunk(static_cast<usize>(r >> 16) % 8, c);
      s.append(chunk.data(), chunk.size());
      model.append(chunk);
      break;
    }
    case 4: {
      const std::string chunk(static_cast<usize>(r >> 16) % 5, c);
      s.insert(pos, nx::string_view(chunk.data(), chunk.size()));
      model.insert(pos, chunk);
      break;
    }
    default: {
      const usize n = static_cast<usize>(r >> 24) % 4;
      s.erase(pos, n);
      model.erase(pos, n);
      break;
    }
    }

    REQUIRE(s.size() == model.size());
  }

  CHECK(s.view() == nx::string_view(model.data(), model.size()));
  CHECK(s.c_str()[s.size()] == '\0');
}

TEST_CASE("string: char overloads match the string_view ones") {
  const nx::string s("a/b/c.txt");
  CHECK(s.rfind('/') == 3);
  CHECK(s.rfind('/', 2) == 1);
  CHECK(s.rfind('x', 0) == nx::string::npos);
  CHECK(s.starts_with('a'));
  CHECK_FALSE(s.starts_with('b'));
  CHECK(s.ends_with('t'));
  CHECK(s.contains('.'));
  CHECK_FALSE(s.contains('?'));
  CHECK_FALSE(nx::string().starts_with('a'));
  CHECK_FALSE(nx::string().ends_with('a'));
}

TEST_CASE("string: find_first_of and friends follow std::string") {
  const nx::string s("  key = value  ");
  const std::string std_s("  key = value  ");
  CHECK(s.find_first_not_of(' ') == std_s.find_first_not_of(' '));
  CHECK(s.find_last_not_of(' ') == std_s.find_last_not_of(' '));
  CHECK(s.find_first_of("=:") == std_s.find_first_of("=:"));
  CHECK(s.find_last_of('e') == std_s.find_last_of('e'));
  CHECK(s.find_first_of('e', 6) == std_s.find_first_of('e', 6));
  CHECK(s.find_last_of(" =", 4) == std_s.find_last_of(" =", 4));
  CHECK(s.find_first_not_of(" k", 0) == std_s.find_first_not_of(" k", 0));
  CHECK(s.find_last_not_of(" ue", 100) == std_s.find_last_not_of(" ue", 100));
  CHECK(s.find_first_of('?') == nx::string::npos);
}

TEST_CASE("string: nx::format comes with nx::string") {
  const nx::string name("world");
  const nx::string out = nx::format("hello {} {}", name, 7);
  CHECK(out == nx::string_view("hello world 7"));
  CHECK(std::format("[{:>6}]", name) == "[ world]");
}

TEST_CASE("utf8: only well-formed sequences decode") {
  const auto valid = [](const char *bytes) {
    return nx::utf8::validate_utf8(bytes, std::strlen(bytes));
  };
  for (const char *good :
       {"\x7F", "\xC2\x80", "\xDF\xBF", "\xE0\xA0\x80", "\xED\x9F\xBF",
        "\xEE\x80\x80", "\xEF\xBF\xBF", "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF"})
    CHECK(valid(good));
  for (const char *bad :
       {"\xC0\x80", "\xC1\xBF", "\xE0\x80\x80", "\xE0\x9F\xBF", "\xED\xA0\x80",
        "\xED\xBF\xBF", "\xF0\x80\x80\x80", "\xF0\x8F\xBF\xBF",
        "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xF7\xBF\xBF\xBF", "\xF8",
        "\x80", "\xE0\xA0"})
    CHECK_FALSE(valid(bad));

  char32_t cp = 0;
  usize read = 0;
  CHECK_FALSE(nx::utf8::decode_char("\xED\xA0\x80", 3, cp, read));
  CHECK(cp == U'\uFFFD');
  CHECK(read == 1);
  CHECK_FALSE(nx::string("\xC0\x80").is_valid_utf8());
}

TEST_CASE("utf8: UTF-16 and UTF-32 output never holds a surrogate") {
  const nx::string encoded_surrogate("a\xED\xA0\x80z");
  const std::u16string wide = encoded_surrogate.u16string();
  for (const char16_t unit : wide)
    CHECK_FALSE(nx::utf8::is_surrogate(unit));
  CHECK(wide.front() == u'a');
  CHECK(wide.back() == u'z');
  for (const char32_t c : nx::string("\xF4\x90\x80\x80").u32string())
    CHECK(nx::utf8::is_valid_codepoint(c));
}
