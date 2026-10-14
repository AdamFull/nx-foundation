#include "framework/nxtest.h"

#include "core/foundation/containers/bit_array.h"

#include <bitset>
#include <vector>

namespace {

template <usize N> std::vector<usize> set_bits(const nx::bit_array<N> &a) {
  std::vector<usize> out;
  for (usize i = a.find_first(); i != a.npos; i = a.find_next(i))
    out.push_back(i);
  return out;
}

template <usize N>
bool matches(const nx::bit_array<N> &a, const std::bitset<N> &b) {
  if (a.count() != b.count())
    return false;
  for (usize i = 0; i < N; ++i)
    if (a.test(i) != b.test(i))
      return false;
  return true;
}

}

TEST_CASE("bit_array: a fresh array is empty") {
  const nx::bit_array<128> a;
  CHECK(a.size() == 128);
  CHECK(a.count() == 0);
  CHECK(a.none());
  CHECK_FALSE(a.any());
  CHECK_FALSE(a.all());
  for (usize i = 0; i < 128; ++i)
    CHECK_FALSE(a.test(i));
  CHECK(a.find_first() == a.npos);
}

TEST_CASE("bit_array: word_count is ceil(N / 64)") {
  CHECK(nx::bit_array<1>::word_count() == 1);
  CHECK(nx::bit_array<63>::word_count() == 1);
  CHECK(nx::bit_array<64>::word_count() == 1);
  CHECK(nx::bit_array<65>::word_count() == 2);
  CHECK(nx::bit_array<128>::word_count() == 2);
  CHECK(nx::bit_array<129>::word_count() == 3);
  CHECK(nx::bit_array<1000>::word_count() == 16);

  CHECK(sizeof(nx::bit_array<64>) == 8);
  CHECK(sizeof(nx::bit_array<128>) == 16);
  CHECK(sizeof(nx::bit_array<65>) == 16);
}

TEST_CASE("bit_array: set, test, reset and flip a single bit") {
  nx::bit_array<100> a;

  a.set(0);
  CHECK(a.test(0));
  CHECK(a.count() == 1);
  CHECK(a.any());
  CHECK_FALSE(a.none());

  a.set(99);
  CHECK(a.test(99));
  CHECK(a.count() == 2);

  a.set(0, false);
  CHECK_FALSE(a.test(0));
  CHECK(a.count() == 1);

  a.set(99);
  a.set(99);
  CHECK(a.count() == 1);

  a.reset(99);
  CHECK_FALSE(a.test(99));
  CHECK(a.count() == 0);

  a.reset(99);
  CHECK(a.count() == 0);

  a.flip(5);
  CHECK(a.test(5));
  a.flip(5);
  CHECK_FALSE(a.test(5));
}

TEST_CASE("bit_array: bits are independent across word boundaries") {
  nx::bit_array<200> a;
  for (const usize pos : {usize(0), usize(63), usize(64), usize(65), usize(127),
                          usize(128), usize(199)}) {
    a.reset();
    a.set(pos);
    CHECK(a.count() == 1);
    CHECK(a.test(pos));
    CHECK(a.find_first() == pos);
    for (usize i = 0; i < 200; ++i)
      if (i != pos)
        CHECK_FALSE(a.test(i));
  }
}

TEST_CASE("bit_array: set() and reset() operate on the whole array") {
  nx::bit_array<100> a;

  a.set();
  CHECK(a.count() == 100);
  CHECK(a.all());
  CHECK(a.any());
  CHECK_FALSE(a.none());
  for (usize i = 0; i < 100; ++i)
    CHECK(a.test(i));

  a.reset();
  CHECK(a.count() == 0);
  CHECK(a.none());
  CHECK_FALSE(a.all());
}

TEST_CASE("bit_array: set() leaves no padding bits set past N") {
  nx::bit_array<100> a;
  a.set();
  CHECK(a.count() == 100);
  CHECK(a.all());

  nx::bit_array<128> full;
  full.set();
  CHECK(full.count() == 128);
  CHECK(full.all());

  nx::bit_array<1> one;
  one.set();
  CHECK(one.count() == 1);
  CHECK(one.all());
  CHECK(one.test(0));
}

TEST_CASE("bit_array: flip() inverts every live bit and no padding bit") {
  nx::bit_array<100> a;
  a.set(3);
  a.set(50);

  a.flip();
  CHECK(a.count() == 98);
  CHECK_FALSE(a.test(3));
  CHECK_FALSE(a.test(50));
  CHECK(a.test(0));
  CHECK(a.test(99));

  a.flip();
  CHECK(a.count() == 2);
  CHECK(a.test(3));
  CHECK(a.test(50));

  nx::bit_array<70> b;
  b.flip();
  CHECK(b.count() == 70);
  CHECK(b.all());
}

TEST_CASE("bit_array: all() is false while any live bit is clear") {
  nx::bit_array<130> a;
  a.set();
  CHECK(a.all());

  for (const usize pos : {usize(0), usize(63), usize(64), usize(129)}) {
    a.set();
    a.reset(pos);
    CHECK_FALSE(a.all());
    CHECK(a.count() == 129);
  }
}

TEST_CASE("bit_array: operator[] reads and writes through the proxy") {
  nx::bit_array<80> a;

  a[10] = true;
  CHECK(a.test(10));
  CHECK(a[10]);

  a[10] = false;
  CHECK_FALSE(a[10]);

  a[20] = true;
  a[21] = a[20];
  CHECK(a[21]);
  a[22] = a[30];
  CHECK_FALSE(a[22]);

  CHECK(~a[30]);
  CHECK_FALSE(~a[20]);

  a[40].flip();
  CHECK(a[40]);
  a[40].flip();
  CHECK_FALSE(a[40]);

  const nx::bit_array<80> &ca = a;
  CHECK(ca[20]);
  CHECK_FALSE(ca[0]);
}

TEST_CASE("bit_array: the value constructor seeds the low word") {
  const nx::bit_array<64> a{0b1011u};
  CHECK(a.test(0));
  CHECK(a.test(1));
  CHECK_FALSE(a.test(2));
  CHECK(a.test(3));
  CHECK(a.count() == 3);

  const nx::bit_array<4> small{0xffffffffffffffffull};
  CHECK(small.count() == 4);
  CHECK(small.all());

  const nx::bit_array<128> wide{0xffffffffffffffffull};
  CHECK(wide.count() == 64);
  CHECK(wide.test(63));
  CHECK_FALSE(wide.test(64));
}

TEST_CASE("bit_array: find_first and find_next enumerate set bits in order") {
  nx::bit_array<256> a;
  CHECK(a.find_first() == a.npos);

  const std::vector<usize> expected = {0, 1, 63, 64, 65, 127, 128, 200, 255};
  for (const usize p : expected)
    a.set(p);

  CHECK(set_bits(a) == expected);
  CHECK(a.find_first() == 0);
  CHECK(a.find_next(0) == 1);
  CHECK(a.find_next(1) == 63);
  CHECK(a.find_next(63) == 64);
  CHECK(a.find_next(255) == a.npos);

  CHECK(a.find_next(200) == 255);
  CHECK(a.find_next(150) == 200);
}

TEST_CASE("bit_array: find_next handles the boundaries of the array") {
  nx::bit_array<64> a;
  a.set(63);
  CHECK(a.find_first() == 63);
  CHECK(a.find_next(62) == 63);
  CHECK(a.find_next(63) == a.npos);

  nx::bit_array<1> one;
  one.set(0);
  CHECK(one.find_first() == 0);
  CHECK(one.find_next(0) == one.npos);

  nx::bit_array<70> b;
  b.set(69);
  CHECK(b.find_next(69) == b.npos);
  CHECK(b.find_next(1000) == b.npos);
  CHECK(b.find_next(b.npos) == b.npos);
}

TEST_CASE("bit_array: find_first skips whole empty words") {
  nx::bit_array<1024> a;
  a.set(1000);
  CHECK(a.find_first() == 1000);
  CHECK(a.find_next(1000) == a.npos);

  a.set(0);
  CHECK(a.find_first() == 0);
  CHECK(a.find_next(0) == 1000);
}

TEST_CASE("bit_array: bitwise and/or/xor combine two arrays") {
  nx::bit_array<128> a, b;
  a.set(0);
  a.set(1);
  a.set(64);
  b.set(1);
  b.set(2);
  b.set(64);

  const auto conj = a & b;
  CHECK(conj.count() == 2);
  CHECK(conj.test(1));
  CHECK(conj.test(64));
  CHECK_FALSE(conj.test(0));

  const auto disj = a | b;
  CHECK(disj.count() == 4);
  CHECK(disj.test(0));
  CHECK(disj.test(1));
  CHECK(disj.test(2));
  CHECK(disj.test(64));

  const auto sym = a ^ b;
  CHECK(sym.count() == 2);
  CHECK(sym.test(0));
  CHECK(sym.test(2));
  CHECK_FALSE(sym.test(1));
  CHECK_FALSE(sym.test(64));

  CHECK(a.count() == 3);
  CHECK(b.count() == 3);
}

TEST_CASE("bit_array: compound bitwise assignment mutates in place") {
  nx::bit_array<128> a, b;
  a.set(0);
  a.set(1);
  b.set(1);
  b.set(2);

  nx::bit_array<128> x = a;
  x &= b;
  CHECK(x.count() == 1);
  CHECK(x.test(1));

  x = a;
  x |= b;
  CHECK(x.count() == 3);

  x = a;
  x ^= b;
  CHECK(x.count() == 2);
  CHECK(x.test(0));
  CHECK(x.test(2));

  x = a;
  x &= x;
  CHECK(x == a);
  x |= x;
  CHECK(x == a);
  x ^= x;
  CHECK(x.none());
}

TEST_CASE("bit_array: operator~ complements only the live bits") {
  nx::bit_array<100> a;
  a.set(0);

  const auto n = ~a;
  CHECK(n.count() == 99);
  CHECK_FALSE(n.test(0));
  CHECK(n.test(1));
  CHECK(n.test(99));

  CHECK(~~a == a);

  CHECK(a.count() == 1);

  const nx::bit_array<70> e;
  CHECK((~e).all());
  CHECK((~e).count() == 70);
}

TEST_CASE("bit_array: left shift moves bits up and drops those past N") {
  nx::bit_array<128> a;
  a.set(0);
  a.set(1);

  auto s = a << 1;
  CHECK(s.count() == 2);
  CHECK(s.test(1));
  CHECK(s.test(2));
  CHECK_FALSE(s.test(0));

  s = a << 64;
  CHECK(s.count() == 2);
  CHECK(s.test(64));
  CHECK(s.test(65));

  a.reset();
  a.set(63);
  s = a << 1;
  CHECK(s.count() == 1);
  CHECK(s.test(64));

  a.reset();
  a.set(127);
  s = a << 1;
  CHECK(s.none());

  a.set();
  CHECK((a << 128).none());
  CHECK((a << 1000).none());

  CHECK((a << 0) == a);
}

TEST_CASE("bit_array: right shift moves bits down and drops those below zero") {
  nx::bit_array<128> a;
  a.set(1);
  a.set(2);

  auto s = a >> 1;
  CHECK(s.count() == 2);
  CHECK(s.test(0));
  CHECK(s.test(1));

  a.reset();
  a.set(64);
  s = a >> 1;
  CHECK(s.count() == 1);
  CHECK(s.test(63));

  s = a >> 64;
  CHECK(s.count() == 1);
  CHECK(s.test(0));

  a.reset();
  a.set(0);
  CHECK((a >> 1).none());

  a.set();
  CHECK((a >> 128).none());
  CHECK((a >> 1000).none());
  CHECK((a >> 0) == a);
}

TEST_CASE(
    "bit_array: shifting a non-word-multiple size keeps the padding clear") {
  nx::bit_array<100> a;
  a.set(99);
  a.set(80);
  const auto s = a << 10;
  CHECK(s.count() == 1);
  CHECK(s.test(90));

  nx::bit_array<100> b;
  b.set();
  const auto t = b << 1;
  CHECK(t.count() == 99);
  CHECK_FALSE(t.test(0));
  CHECK(t.test(99));
}

TEST_CASE("bit_array: shift round-trips agree with a std::bitset model") {
  constexpr usize N = 200;
  nx::bit_array<N> a;
  std::bitset<N> model;

  for (const usize p : {usize(0), usize(1), usize(63), usize(64), usize(65),
                        usize(130), usize(199)}) {
    a.set(p);
    model.set(p);
  }
  REQUIRE(matches(a, model));

  for (const usize sh : {usize(0), usize(1), usize(7), usize(63), usize(64),
                         usize(65), usize(128), usize(199)}) {
    CHECK(matches(a << sh, model << sh));
    CHECK(matches(a >> sh, model >> sh));
  }
}

TEST_CASE("bit_array: equality compares every live bit") {
  nx::bit_array<100> a, b;
  CHECK(a == b);

  a.set(50);
  CHECK_FALSE(a == b);
  CHECK(a != b);

  b.set(50);
  CHECK(a == b);

  nx::bit_array<100> c, d;
  c.set();
  c.reset(7);
  d.flip();
  d.flip(7);
  CHECK(c == d);
}

TEST_CASE("bit_array: copy and assignment are value semantics") {
  nx::bit_array<128> a;
  a.set(1);
  a.set(100);

  nx::bit_array<128> b = a;
  CHECK(b == a);

  b.set(2);
  CHECK_FALSE(b == a);
  CHECK(a.count() == 2);

  nx::bit_array<128> c;
  c = a;
  CHECK(c == a);
  c.reset();
  CHECK(a.count() == 2);
}

TEST_CASE("bit_array: data() exposes the packed words") {
  nx::bit_array<128> a;
  a.set(0);
  a.set(64);

  const u64 *w = a.data();
  REQUIRE(w != nullptr);
  CHECK(w[0] == 1ull);
  CHECK(w[1] == 1ull);

  a.set(63);
  CHECK(w[0] == ((1ull << 63) | 1ull));
}

TEST_CASE("bit_array: is usable in a constant expression") {
  constexpr auto built = [] {
    nx::bit_array<128> a;
    a.set(3);
    a.set(70);
    a.flip(3);
    a.set(5);
    return a;
  }();

  static_assert(built.count() == 2);
  static_assert(built.test(5));
  static_assert(built.test(70));
  static_assert(!built.test(3));
  static_assert(built.find_first() == 5);
  static_assert(built.find_next(5) == 70);
  static_assert(built.find_next(70) == built.npos);
  static_assert(built.any());
  static_assert(!built.none());
  static_assert(!built.all());

  CHECK(built.count() == 2);
}

TEST_CASE("bit_array: mirrors a std::bitset under a long mixed workload") {
  constexpr usize N = 333;
  nx::bit_array<N> a;
  std::bitset<N> model;

  u64 state = 0x243F6A8885A308D3ull;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  for (int step = 0; step < 10000; ++step) {
    const u64 r = next();
    const usize pos = static_cast<usize>(r % N);
    switch (r % 3) {
    case 0:
      a.set(pos);
      model.set(pos);
      break;
    case 1:
      a.reset(pos);
      model.reset(pos);
      break;
    default:
      a.flip(pos);
      model.flip(pos);
      break;
    }
  }

  CHECK(matches(a, model));
  CHECK(a.count() == model.count());
  CHECK(a.any() == model.any());
  CHECK(a.all() == model.all());
  CHECK(a.none() == model.none());

  std::vector<usize> expected;
  for (usize i = 0; i < N; ++i)
    if (model.test(i))
      expected.push_back(i);
  CHECK(set_bits(a) == expected);
}
