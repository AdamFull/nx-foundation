#include "framework/nxtest.h"

#include "core/foundation/core/random.h"

#include <array>
#include <cstring>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr nx::Rng::Alg kAlgs[] = {nx::Rng::Alg::Pcg, nx::Rng::Alg::Xoshiro256,
                                  nx::Rng::Alg::Splitmix64};

const char *alg_name(nx::Rng::Alg a) {
  switch (a) {
  case nx::Rng::Alg::Pcg:
    return "Pcg";
  case nx::Rng::Alg::Xoshiro256:
    return "Xoshiro256";
  case nx::Rng::Alg::Splitmix64:
    return "Splitmix64";
  }
  return "?";
}

}

TEST_CASE("Guid: default-constructed is all zero and formats as such") {
  constexpr nx::Guid g{};
  for (const u8 b : g.bytes)
    CHECK(b == 0);

  const auto chars = g.to_chars();
  CHECK(std::string(chars.data()) == "00000000-0000-0000-0000-000000000000");
  CHECK(chars[36] == '\0');
  CHECK(g.to_string() == "00000000-0000-0000-0000-000000000000");
}

TEST_CASE("Guid: to_chars produces the canonical 8-4-4-4-12 layout") {
  nx::Guid g{};
  for (int i = 0; i < 16; ++i)
    g.bytes[static_cast<usize>(i)] = static_cast<u8>(i * 0x11);

  const std::string s(g.to_chars().data());
  REQUIRE(s.size() == 36);
  CHECK(s[8] == '-');
  CHECK(s[13] == '-');
  CHECK(s[18] == '-');
  CHECK(s[23] == '-');
  CHECK(s == "00112233-4455-6677-8899-aabbccddeeff");

  for (const char c : s) {
    const bool ok =
        c == '-' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    CHECK(ok);
  }
}

TEST_CASE("Guid: parse accepts a canonical v4 string and recovers the bytes") {
  const auto g = nx::Guid::parse("12345678-1234-4234-8234-1234567890ab");
  REQUIRE(g.has_value());
  CHECK(g->bytes[0] == 0x12);
  CHECK(g->bytes[1] == 0x34);
  CHECK(g->bytes[6] == 0x42);
  CHECK(g->bytes[8] == 0x82);
  CHECK(g->bytes[15] == 0xab);

  const auto up = nx::Guid::parse("12345678-1234-4234-8234-1234567890AB");
  REQUIRE(up.has_value());
  CHECK(*up == *g);

  CHECK(std::string(g->to_chars().data()) ==
        "12345678-1234-4234-8234-1234567890ab");
}

TEST_CASE("Guid: parse accepts every valid variant nibble") {
  for (const char v : {'8', '9', 'a', 'b'}) {
    std::string s = "12345678-1234-4234-8234-1234567890ab";
    s[19] = v;
    CHECK(nx::Guid::parse(s).has_value());
  }
  for (const char v : {'0', '1', '7', 'c', 'd', 'e', 'f'}) {
    std::string s = "12345678-1234-4234-8234-1234567890ab";
    s[19] = v;
    CHECK_FALSE(nx::Guid::parse(s).has_value());
  }
}

TEST_CASE("Guid: parse rejects malformed input") {
  CHECK_FALSE(nx::Guid::parse("").has_value());
  CHECK_FALSE(nx::Guid::parse("12345678").has_value());
  CHECK_FALSE(
      nx::Guid::parse("12345678-1234-4234-8234-1234567890a").has_value());
  CHECK_FALSE(
      nx::Guid::parse("12345678-1234-4234-8234-1234567890abc").has_value());
  CHECK_FALSE(
      nx::Guid::parse("12345678-1234-4234-8234-1234567890ab ").has_value());
  CHECK_FALSE(nx::Guid::parse("1234567812344234823412345678 90ab").has_value());
  CHECK_FALSE(
      nx::Guid::parse("12345678+1234-4234-8234-1234567890ab").has_value());
  CHECK_FALSE(
      nx::Guid::parse("1234567g-1234-4234-8234-1234567890ab").has_value());
  CHECK_FALSE(
      nx::Guid::parse("12345678-1234-4234-8234-1234567890aZ").has_value());
  CHECK_FALSE(
      nx::Guid::parse("12345678-1234-1234-8234-1234567890ab").has_value());
  CHECK_FALSE(
      nx::Guid::parse("12345678-1234-5234-8234-1234567890ab").has_value());

  static_assert(!nx::Guid::parse("nope").has_value());
}

TEST_CASE("Guid: parse is usable at compile time") {
  constexpr auto g = nx::Guid::parse("00000000-0000-4000-8000-000000000000");
  static_assert(g.has_value());
  static_assert(g->bytes[6] == 0x40);
  static_assert(g->bytes[8] == 0x80);
  CHECK(std::string(g->to_chars().data()) ==
        "00000000-0000-4000-8000-000000000000");
}

TEST_CASE("Guid: comparison and hashing") {
  const auto a = *nx::Guid::parse("12345678-1234-4234-8234-1234567890ab");
  const auto b = *nx::Guid::parse("12345678-1234-4234-8234-1234567890ab");
  const auto c = *nx::Guid::parse("12345678-1234-4234-8234-1234567890ac");

  CHECK(a == b);
  CHECK(a != c);
  CHECK(a < c);
  CHECK(c > a);
  CHECK(a <= b);

  const std::hash<nx::Guid> hasher;
  CHECK(hasher(a) == hasher(b));
  CHECK(hasher(a) != hasher(c));

  nx::Rng rng(nx::Rng::Alg::Xoshiro256, 12345);
  std::unordered_set<std::size_t> hashes;
  std::set<nx::Guid> values;
  for (int i = 0; i < 2000; ++i) {
    const nx::Guid g = rng.guid();
    hashes.insert(hasher(g));
    values.insert(g);
  }
  CHECK(values.size() == 2000);
  CHECK(hashes.size() == 2000);
}

TEST_CASE("Rng: seeding is deterministic and reproducible per algorithm") {
  for (const nx::Rng::Alg alg : kAlgs) {
    SUBCASE(alg_name(alg)) {
      nx::Rng a(alg, 42);
      nx::Rng b(alg, 42);

      for (int i = 0; i < 200; ++i)
        CHECK(a.next_u64() == b.next_u64());

      a.seed(42);
      b.seed(42);
      CHECK(a.next_u64() == b.next_u64());

      nx::Rng c;
      c.init(alg, 42);
      nx::Rng d(alg, 42);
      CHECK(c.next_u64() == d.next_u64());
    }
  }
}

TEST_CASE("Rng: different seeds produce different streams") {
  for (const nx::Rng::Alg alg : kAlgs) {
    SUBCASE(alg_name(alg)) {
      nx::Rng a(alg, 1);
      nx::Rng b(alg, 2);

      int differing = 0;
      for (int i = 0; i < 100; ++i)
        if (a.next_u64() != b.next_u64())
          ++differing;
      CHECK(differing > 95);
    }
  }
}

TEST_CASE("Rng: next_u32 and next_u64 do not repeat or stick") {
  for (const nx::Rng::Alg alg : kAlgs) {
    SUBCASE(alg_name(alg)) {
      nx::Rng rng(alg, 7);

      std::set<u32> seen32;
      for (int i = 0; i < 5000; ++i)
        seen32.insert(rng.next_u32());
      CHECK(seen32.size() > 4990);

      rng.seed(7);
      std::set<u64> seen64;
      for (int i = 0; i < 5000; ++i)
        seen64.insert(rng.next_u64());
      CHECK(seen64.size() == 5000);
    }
  }
}

TEST_CASE("Rng: next_u32 output bits are all exercised") {
  for (const nx::Rng::Alg alg : kAlgs) {
    SUBCASE(alg_name(alg)) {
      nx::Rng rng(alg, 99);
      u32 ones = 0, zeros = 0;
      for (int i = 0; i < 2000; ++i) {
        const u32 v = rng.next_u32();
        ones |= v;
        zeros |= ~v;
      }
      CHECK(ones == 0xffffffffu);
      CHECK(zeros == 0xffffffffu);
    }
  }
}

TEST_CASE("Rng: next_u64 output bits are all exercised") {
  for (const nx::Rng::Alg alg : kAlgs) {
    SUBCASE(alg_name(alg)) {
      nx::Rng rng(alg, 101);
      u64 ones = 0, zeros = 0;
      for (int i = 0; i < 2000; ++i) {
        const u64 v = rng.next_u64();
        ones |= v;
        zeros |= ~v;
      }
      CHECK(ones == ~u64(0));
      CHECK(zeros == ~u64(0));
    }
  }
}

TEST_CASE("Rng: bounded next_u32 stays in range and covers it") {
  nx::Rng rng(nx::Rng::Alg::Pcg, 5);

  CHECK(rng.next_u32(0) == 0u);

  for (int i = 0; i < 50; ++i)
    CHECK(rng.next_u32(1) == 0u);

  for (const u32 bound : {2u, 3u, 7u, 10u, 100u, 1000u}) {
    std::set<u32> seen;
    for (int i = 0; i < 20000; ++i) {
      const u32 v = rng.next_u32(bound);
      REQUIRE(v < bound);
      seen.insert(v);
    }
    if (bound <= 100)
      CHECK(seen.size() == bound);
  }
}

TEST_CASE("Rng: bounded next_u32 is close to uniform") {
  nx::Rng rng(nx::Rng::Alg::Xoshiro256, 2024);
  constexpr u32 bound = 8;
  constexpr int draws = 200000;
  std::array<int, bound> counts{};

  for (int i = 0; i < draws; ++i) {
    const u32 v = rng.next_u32(bound);
    REQUIRE(v < bound);
    ++counts[v];
  }

  const int expected = draws / static_cast<int>(bound);
  for (const int c : counts) {
    CHECK(c > expected - expected / 20);
    CHECK(c < expected + expected / 20);
  }
}

TEST_CASE("Rng: a default-constructed Rng selects a real algorithm") {
  nx::Rng a;
  CHECK(a.algorithm() == nx::Rng::Alg::Pcg);

  nx::Rng b;
  for (int i = 0; i < 100; ++i)
    CHECK(a.next_u64() == b.next_u64());

  a.seed(12345);
  b.seed(12345);
  CHECK(a.next_u64() == b.next_u64());
}

TEST_CASE("Rng: bounded next_u64 is unbiased") {
  nx::Rng rng(nx::Rng::Alg::Splitmix64, 987654321);
  constexpr u64 bound = 6;
  constexpr int draws = 300000;
  std::array<int, bound> counts{};

  for (int i = 0; i < draws; ++i) {
    const u64 v = rng.next_u64(bound);
    REQUIRE(v < bound);
    ++counts[static_cast<usize>(v)];
  }

  const int expected = draws / static_cast<int>(bound);
  for (const int c : counts) {
    CHECK(c > expected - expected / 20);
    CHECK(c < expected + expected / 20);
  }

  const u64 huge = (~u64{0} / 3) * 2;
  for (int i = 0; i < 2000; ++i)
    REQUIRE(rng.next_u64(huge) < huge);
}

TEST_CASE("Rng: bounded next_u64 stays in range") {
  nx::Rng rng(nx::Rng::Alg::Splitmix64, 11);

  CHECK(rng.next_u64(0) == 0ull);
  for (int i = 0; i < 50; ++i)
    CHECK(rng.next_u64(1) == 0ull);

  for (const u64 bound : {2ull, 13ull, 1000ull, 1ull << 40}) {
    for (int i = 0; i < 2000; ++i)
      REQUIRE(rng.next_u64(bound) < bound);
  }

  std::set<u64> seen;
  for (int i = 0; i < 5000; ++i)
    seen.insert(rng.next_u64(6));
  CHECK(seen.size() == 6);
}

TEST_CASE("Rng: integer range is inclusive at both ends") {
  nx::Rng rng(nx::Rng::Alg::Pcg, 3);

  for (int i = 0; i < 20; ++i) {
    CHECK(rng.range(5, 5) == 5);
    CHECK(rng.range(i64(-7), i64(-7)) == -7);
  }

  std::set<i32> seen;
  for (int i = 0; i < 20000; ++i) {
    const i32 v = rng.range(-3, 4);
    REQUIRE(v >= -3);
    REQUIRE(v <= 4);
    seen.insert(v);
  }
  CHECK(seen.size() == 8);

  std::set<i64> seen64;
  for (int i = 0; i < 20000; ++i) {
    const i64 v = rng.range(i64(10), i64(15));
    REQUIRE(v >= 10);
    REQUIRE(v <= 15);
    seen64.insert(v);
  }
  CHECK(seen64.size() == 6);
}

TEST_CASE("Rng: integer range handles spans that overflow the signed type") {
  nx::Rng rng(nx::Rng::Alg::Xoshiro256, 31337);

  constexpr i32 lo32 = -2147483647 - 1;
  constexpr i32 hi32 = 2147483647;

  bool saw_negative = false, saw_positive = false;
  i32 seen_min = hi32, seen_max = lo32;
  for (int i = 0; i < 20000; ++i) {
    const i32 v = rng.range(lo32, hi32);
    saw_negative |= v < 0;
    saw_positive |= v > 0;
    seen_min = nx::min(seen_min, v);
    seen_max = nx::max(seen_max, v);
  }
  CHECK(saw_negative);
  CHECK(saw_positive);
  CHECK(seen_min < lo32 / 2);
  CHECK(seen_max > hi32 / 2);

  for (int i = 0; i < 20000; ++i) {
    const i32 v = rng.range(lo32, 0);
    REQUIRE(v >= lo32);
    REQUIRE(v <= 0);
  }
  for (int i = 0; i < 20000; ++i) {
    const i32 v = rng.range(-1000000000, 1000000000);
    REQUIRE(v >= -1000000000);
    REQUIRE(v <= 1000000000);
  }

  constexpr i64 lo64 = -9223372036854775807LL - 1;
  constexpr i64 hi64 = 9223372036854775807LL;
  bool neg64 = false, pos64 = false;
  for (int i = 0; i < 20000; ++i) {
    const i64 v = rng.range(lo64, hi64);
    neg64 |= v < 0;
    pos64 |= v > 0;
  }
  CHECK(neg64);
  CHECK(pos64);

  for (int i = 0; i < 20000; ++i) {
    const i64 v = rng.range(lo64, i64(0));
    REQUIRE(v >= lo64);
    REQUIRE(v <= 0);
  }

  for (int i = 0; i < 500; ++i) {
    const i32 v = rng.range(lo32, lo32 + 1);
    REQUIRE((v == lo32 || v == lo32 + 1));
    const i32 w = rng.range(hi32 - 1, hi32);
    REQUIRE((w == hi32 - 1 || w == hi32));
  }
}

TEST_CASE("Rng: integer range swaps inverted bounds") {
  nx::Rng rng(nx::Rng::Alg::Pcg, 17);
  for (int i = 0; i < 500; ++i) {
    const i32 v = rng.range(10, 0);
    CHECK(v >= 0);
    CHECK(v <= 10);

    const i64 w = rng.range(i64(20), i64(-20));
    CHECK(w >= -20);
    CHECK(w <= 20);
  }
}

TEST_CASE("Rng: float generators stay in [0, 1)") {
  for (const nx::Rng::Alg alg : kAlgs) {
    SUBCASE(alg_name(alg)) {
      nx::Rng rng(alg, 8);

      f32 min32 = 2.0f, max32 = -1.0f;
      for (int i = 0; i < 20000; ++i) {
        const f32 v = rng.next_f32();
        REQUIRE(v >= 0.0f);
        REQUIRE(v < 1.0f);
        min32 = nx::min(min32, v);
        max32 = nx::max(max32, v);
      }
      CHECK(min32 < 0.01f);
      CHECK(max32 > 0.99f);

      f64 min64 = 2.0, max64 = -1.0;
      for (int i = 0; i < 20000; ++i) {
        const f64 v = rng.next_f64();
        REQUIRE(v >= 0.0);
        REQUIRE(v < 1.0);
        min64 = nx::min(min64, v);
        max64 = nx::max(max64, v);
      }
      CHECK(min64 < 0.01);
      CHECK(max64 > 0.99);
    }
  }
}

TEST_CASE("Rng: float generators have a mean near one half") {
  nx::Rng rng(nx::Rng::Alg::Xoshiro256, 33);
  f64 sum = 0.0;
  constexpr int n = 100000;
  for (int i = 0; i < n; ++i)
    sum += rng.next_f64();
  CHECK(sum / n == nxtest::Approx(0.5).margin(0.01));
}

TEST_CASE("Rng: float range covers the requested interval") {
  nx::Rng rng(nx::Rng::Alg::Pcg, 21);

  f32 lo32 = 1e9f, hi32 = -1e9f;
  for (int i = 0; i < 20000; ++i) {
    const f32 v = rng.range(-1.0f, 1.0f);
    REQUIRE(v >= -1.0f);
    REQUIRE(v < 1.0f);
    lo32 = nx::min(lo32, v);
    hi32 = nx::max(hi32, v);
  }
  CHECK(lo32 < -0.98f);
  CHECK(hi32 > 0.98f);

  for (int i = 0; i < 5000; ++i) {
    const f64 v = rng.range(10.0, 20.0);
    REQUIRE(v >= 10.0);
    REQUIRE(v < 20.0);
  }

  for (int i = 0; i < 20; ++i)
    CHECK(rng.range(3.0f, 3.0f) == 3.0f);
}

TEST_CASE("Rng: chance honours its probability") {
  nx::Rng rng(nx::Rng::Alg::Xoshiro256, 55);

  for (int i = 0; i < 500; ++i) {
    CHECK_FALSE(rng.chance(0.0f));
    CHECK_FALSE(rng.chance(-1.0f));
    CHECK(rng.chance(1.0f));
    CHECK(rng.chance(2.0f));
  }

  constexpr int n = 100000;
  int hits = 0;
  for (int i = 0; i < n; ++i)
    if (rng.chance(0.25f))
      ++hits;
  CHECK(static_cast<f64>(hits) / n == nxtest::Approx(0.25).margin(0.01));
}

TEST_CASE("Rng: fill writes exactly the requested number of bytes") {
  nx::Rng rng(nx::Rng::Alg::Pcg, 77);

  for (const usize n :
       {usize(0), usize(1), usize(7), usize(8), usize(9), usize(15), usize(16),
        usize(31), usize(64), usize(1000)}) {
    std::vector<std::byte> buf(n + 8, std::byte{0xCD});
    rng.fill(std::span(buf.data(), n));

    for (usize i = n; i < buf.size(); ++i)
      CHECK(buf[i] == std::byte{0xCD});
  }

  std::vector<std::byte> big(4096);
  rng.fill(big);
  int nonzero = 0;
  for (const std::byte b : big)
    if (b != std::byte{0})
      ++nonzero;
  CHECK(nonzero > 4000);

  std::vector<std::byte> a(64), b(64);
  rng.seed(77);
  rng.fill(a);
  rng.seed(77);
  rng.fill(b);
  CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}

TEST_CASE("Rng: guid produces well-formed, parseable v4 identifiers") {
  nx::Rng rng(nx::Rng::Alg::Xoshiro256, 4242);

  for (int i = 0; i < 2000; ++i) {
    const nx::Guid g = rng.guid();

    CHECK((g.bytes[6] & 0xf0) == 0x40);
    CHECK((g.bytes[8] & 0xc0) == 0x80);

    const auto chars = g.to_chars();
    const auto parsed = nx::Guid::parse(nx::string_view(chars.data(), 36));
    REQUIRE(parsed.has_value());
    CHECK(*parsed == g);
  }
}

TEST_CASE("Rng: guid is unique across a large batch and across seeds") {
  std::set<nx::Guid> seen;
  for (const nx::Rng::Alg alg : kAlgs) {
    nx::Rng rng(alg, 909);
    for (int i = 0; i < 5000; ++i)
      seen.insert(rng.guid());
  }
  CHECK(seen.size() == 3 * 5000);
}

TEST_CASE("Rng: the algorithms produce distinct streams from the same seed") {
  nx::Rng pcg(nx::Rng::Alg::Pcg, 1000);
  nx::Rng xo(nx::Rng::Alg::Xoshiro256, 1000);
  nx::Rng sm(nx::Rng::Alg::Splitmix64, 1000);

  int same = 0;
  for (int i = 0; i < 100; ++i) {
    const u64 a = pcg.next_u64();
    const u64 b = xo.next_u64();
    const u64 c = sm.next_u64();
    if (a == b || b == c || a == c)
      ++same;
  }
  CHECK(same == 0);
}

TEST_CASE("Rng: splitmix64 matches its reference output") {
  nx::Rng rng(nx::Rng::Alg::Splitmix64, 0);
  CHECK(rng.next_u64() == 0xe220a8397b1dcdafull);
  CHECK(rng.next_u64() == 0x6e789e6aa1b965f4ull);
  CHECK(rng.next_u64() == 0x06c45d188009454full);
  CHECK(rng.next_u64() == 0xf88bb8a8724c81ecull);
}

TEST_CASE("Rng: state is copyable and copies continue the same stream") {
  nx::Rng a(nx::Rng::Alg::Xoshiro256, 31337);
  for (int i = 0; i < 10; ++i)
    (void)a.next_u64();

  nx::Rng b = a;
  for (int i = 0; i < 100; ++i)
    CHECK(a.next_u64() == b.next_u64());
}
