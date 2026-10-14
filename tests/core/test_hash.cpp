#include "framework/nxtest.h"

#include "core/foundation/core/hash.h"

#include <cstring>
#include <set>
#include <string>
#include <vector>

TEST_CASE("hash: fnv1a32 matches the reference vectors") {
  CHECK(nx::hash_fnv1a32("", 0) == 0x811c9dc5u);
  CHECK(nx::hash_fnv1a32("a", 1) == 0xe40c292cu);
  CHECK(nx::hash_fnv1a32("b", 1) == 0xe70c2de5u);
  CHECK(nx::hash_fnv1a32("c", 1) == 0xe60c2c52u);
  CHECK(nx::hash_fnv1a32("foobar", 6) == 0xbf9cf968u);

  static_assert(nx::hash_fnv1a32("", 0) == 0x811c9dc5u);
  static_assert(nx::hash_fnv1a32("foobar", 6) == 0xbf9cf968u);
}

TEST_CASE("hash: fnv1a64 matches the reference vectors") {
  CHECK(nx::hash_fnv1a64("", 0) == 0xcbf29ce484222325ull);
  CHECK(nx::hash_fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull);
  CHECK(nx::hash_fnv1a64("b", 1) == 0xaf63df4c8601f1a5ull);
  CHECK(nx::hash_fnv1a64("foobar", 6) == 0x85944171f73967e8ull);

  static_assert(nx::hash_fnv1a64("foobar", 6) == 0x85944171f73967e8ull);
}

TEST_CASE("hash: fnv1a treats bytes above 0x7f as unsigned") {
  const char high[] = {static_cast<char>(0xff)};
  const u32 h32 = nx::hash_fnv1a32(high, 1);
  const u64 h64 = nx::hash_fnv1a64(high, 1);

  CHECK(h32 == static_cast<u32>((0x811c9dc5u ^ 0xffu) * 0x01000193u));
  CHECK(h64 ==
        static_cast<u64>((0xcbf29ce484222325ull ^ 0xffull) * 0x100000001b3ull));
}

TEST_CASE("hash: fnv1a is sensitive to length, order and embedded NULs") {
  CHECK(nx::hash_fnv1a32("ab", 2) != nx::hash_fnv1a32("ba", 2));
  CHECK(nx::hash_fnv1a64("ab", 2) != nx::hash_fnv1a64("ba", 2));
  CHECK(nx::hash_fnv1a32("abc", 3) != nx::hash_fnv1a32("abc", 2));

  const char a[] = {'x', '\0', 'y'};
  const char b[] = {'x', '\0', 'z'};
  CHECK(nx::hash_fnv1a64(a, 3) != nx::hash_fnv1a64(b, 3));
  CHECK(nx::hash_fnv1a64(a, 3) != nx::hash_fnv1a64(a, 1));
}

TEST_CASE("hash: fnv1a span overloads agree with the pointer overloads") {
  const char text[] = "the quick brown fox";
  const auto bytes = std::as_bytes(std::span(text, sizeof(text) - 1));

  CHECK(nx::hash_fnv1a32(bytes) == nx::hash_fnv1a32(text, sizeof(text) - 1));
  CHECK(nx::hash_fnv1a64(bytes) == nx::hash_fnv1a64(text, sizeof(text) - 1));

  std::span<const std::byte> empty{};
  CHECK(nx::hash_fnv1a32(empty) == 0x811c9dc5u);
  CHECK(nx::hash_fnv1a64(empty) == 0xcbf29ce484222325ull);
}

TEST_CASE("hash: hash_u32 is deterministic, non-trivial and well spread") {
  CHECK(nx::hash_u32(0) == nx::hash_u32(0));
  CHECK(nx::hash_u32(1) != nx::hash_u32(0));
  CHECK(nx::hash_u32(1) != 1u);
  CHECK(nx::hash_u32(0xffffffffu) != 0xffffffffu);

  std::set<u32> seen;
  for (u32 i = 0; i < 20000; ++i)
    seen.insert(nx::hash_u32(i));
  CHECK(seen.size() == 20000);

  int close = 0;
  for (u32 i = 0; i < 1000; ++i) {
    const u32 a = nx::hash_u32(i), b = nx::hash_u32(i + 1);
    if ((a > b ? a - b : b - a) < 16)
      ++close;
  }
  CHECK(close < 10);

  static_assert(nx::hash_u32(123) == nx::hash_u32(123));
}

TEST_CASE("hash: hash_u64 is deterministic, non-trivial and well spread") {
  CHECK(nx::hash_u64(0) == nx::hash_u64(0));
  CHECK(nx::hash_u64(1) != nx::hash_u64(0));
  CHECK(nx::hash_u64(1) != 1ull);

  std::set<u64> seen;
  for (u64 i = 0; i < 20000; ++i)
    seen.insert(nx::hash_u64(i));
  CHECK(seen.size() == 20000);

  seen.clear();
  for (u64 i = 0; i < 5000; ++i)
    seen.insert(nx::hash_u64(i * 0x1000000000000ull));
  CHECK(seen.size() == 5000);

  static_assert(nx::hash_u64(7) == nx::hash_u64(7));
}

TEST_CASE("hash: hash_ptr distinguishes distinct addresses") {
  std::vector<int> storage(512);
  std::set<usize> seen;
  for (int &slot : storage)
    seen.insert(nx::hash_ptr(&slot));
  CHECK(seen.size() == storage.size());

  CHECK(nx::hash_ptr(&storage[0]) == nx::hash_ptr(&storage[0]));
  CHECK_NOTHROW(nx::hash_ptr(nullptr));
}

TEST_CASE("hash: hash_comb is order-sensitive and mixes both operands") {
  CHECK(nx::hash_comb(1, 2) != nx::hash_comb(2, 1));
  CHECK(nx::hash_comb(0, 0) != 0u);
  CHECK(nx::hash_comb(1, 2) != nx::hash_comb(1, 3));
  CHECK(nx::hash_comb(1, 2) != nx::hash_comb(3, 2));

  std::set<usize> seen;
  for (usize a = 0; a < 150; ++a)
    for (usize b = 0; b < 150; ++b)
      seen.insert(nx::hash_comb(nx::hash_u64(a), nx::hash_u64(b)));
  CHECK(seen.size() == 150 * 150);

  static_assert(nx::hash_comb(1, 2) == nx::hash_comb(1, 2));
}

TEST_CASE("fnv1a64: folds values and is order dependent") {
  nx::fnv1a64 a;
  a.combine(1).combine(2);
  nx::fnv1a64 b;
  b.combine(2).combine(1);

  CHECK(a.value() != b.value());
  CHECK(a.value() != nx::FNV1A64_BASIS);

  nx::fnv1a64 again;
  again.combine(1).combine(2);
  CHECK(again.value() == a.value());
}

TEST_CASE("fnv1a64: starts from the real FNV-1a basis") {
  CHECK(nx::FNV1A64_BASIS == 0xcbf29ce484222325ull);
  CHECK(nx::FNV1A64_PRIME == 0x100000001b3ull);
  CHECK(nx::FNV1A32_BASIS == 0x811c9dc5u);
  CHECK(nx::FNV1A32_PRIME == 0x01000193u);

  CHECK(nx::fnv1a64().value() == nx::FNV1A64_BASIS);
  CHECK(nx::fnv1a64().combine(0xff).value() ==
        static_cast<u64>((nx::FNV1A64_BASIS ^ 0xffull) * nx::FNV1A64_PRIME));
}

TEST_CASE("fnv1a64: combine_all matches repeated combine") {
  nx::fnv1a64 one_by_one;
  one_by_one.combine(7).combine(9).combine(11);

  nx::fnv1a64 together;
  together.combine_all(7, 9, 11);

  CHECK(one_by_one.value() == together.value());
}

TEST_CASE("fnv1a64: widens enums and small integers") {
  enum class kind : u16 { first = 3, second = 4 };

  nx::fnv1a64 from_enum;
  from_enum.combine_all(kind::first, u8{5});
  nx::fnv1a64 from_ints;
  from_ints.combine_all(u64{3}, u64{5});

  CHECK(from_enum.value() == from_ints.value());
}

TEST_CASE("fnv1a64: seeds and works at compile time") {
  constexpr nx::fnv1a64 seeded(1234);
  CHECK(seeded.value() == 1234);

  static_assert(nx::fnv1a64().combine(42).value() ==
                (nx::FNV1A64_BASIS ^ 42ull) * nx::FNV1A64_PRIME);
}

TEST_CASE("fnv1a: combine_bytes reproduces the free functions") {
  const char text[] = "nx2d render graph";
  const usize len = sizeof(text) - 1;

  CHECK(nx::fnv1a64().combine_bytes(text, len).value() ==
        nx::hash_fnv1a64(text, len));
  CHECK(nx::fnv1a32().combine_bytes(text, len).value() ==
        nx::hash_fnv1a32(text, len));

  CHECK(nx::fnv1a64().combine_bytes("", 0).value() == nx::FNV1A64_BASIS);
  CHECK(nx::fnv1a32().combine_bytes("", 0).value() == nx::FNV1A32_BASIS);
}

TEST_CASE("fnv1a: folding a value differs from folding its bytes") {
  const u32 value = 0x01020304u;
  const char bytes[4] = {4, 3, 2, 1};

  CHECK(nx::fnv1a64().combine(value).value() !=
        nx::fnv1a64().combine_bytes(bytes, 4).value());
}

TEST_CASE("fnv1a32: behaves like its 64-bit twin at 32 bits") {
  nx::fnv1a32 a;
  a.combine(1).combine(2);
  nx::fnv1a32 b;
  b.combine(2).combine(1);

  CHECK(a.value() != b.value());
  CHECK(nx::fnv1a32().value() == nx::FNV1A32_BASIS);
  CHECK(nx::fnv1a32().combine(42).value() ==
        static_cast<u32>((nx::FNV1A32_BASIS ^ 42u) * nx::FNV1A32_PRIME));

  nx::fnv1a32 together;
  together.combine_all(7, 9);
  nx::fnv1a32 apart;
  apart.combine(7).combine(9);
  CHECK(together.value() == apart.value());

  static_assert(nx::fnv1a32().combine(1).value() ==
                static_cast<u32>((nx::FNV1A32_BASIS ^ 1u) * nx::FNV1A32_PRIME));
}
