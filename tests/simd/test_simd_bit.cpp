#include "simd/simd_test_util.h"

#include <array>
#include <bit>
#include <cmath>
#include <complex>

using namespace simd_test;

namespace {

template <class T, int N> void bits() {
  using V = sd::vec<T, N>;
  using S = sd::rebind_t<std::make_signed_t<T>, V>;
  constexpr int w = std::numeric_limits<T>::digits;
  rng<T> r(N * 13 + sizeof(T));
  for (int round = 0; round < 6; ++round) {
    const V x([&](int i) {
      return i % 5 == 0 ? T(0) : i % 5 == 1 ? T(~T(0)) : r.any();
    });
    const V m = random_vec<V>(r, false);

    check_lanes("byteswap", sd::byteswap(x),
                [&](int i) { return sd::detail::byteswap(x[i]); });
    check_lanes("bit_reverse", sd::bit_reverse(x),
                [&](int i) { return sd::detail::bit_reverse(x[i]); });
    const V limited = x >> 1;
    check_lanes("bit_ceil", sd::bit_ceil(limited),
                [&](int i) { return std::bit_ceil(limited[i]); });
    check_lanes("bit_floor", sd::bit_floor(x),
                [&](int i) { return std::bit_floor(x[i]); });
    check_mask("has_single_bit", sd::has_single_bit(x & -x), [&](int i) {
      return std::has_single_bit(T(x[i] & negate(x[i])));
    });

    const S counts = sd::popcount(x);
    static_assert(std::is_same_v<std::remove_const_t<decltype(counts)>, S>);
    check_lanes("popcount", counts, [&](int i) { return std::popcount(x[i]); });
    check_lanes("bit_width", sd::bit_width(x),
                [&](int i) { return std::bit_width(x[i]); });
    check_lanes("countl_zero", sd::countl_zero(x),
                [&](int i) { return std::countl_zero(x[i]); });
    check_lanes("countl_one", sd::countl_one(x),
                [&](int i) { return std::countl_one(x[i]); });
    check_lanes("countr_zero", sd::countr_zero(x),
                [&](int i) { return std::countr_zero(x[i]); });
    check_lanes("countr_one", sd::countr_one(x),
                [&](int i) { return std::countr_one(x[i]); });

    const S rotations([&](int i) {
      return static_cast<std::make_signed_t<T>>(i * 3 - 2 * w);
    });
    check_lanes("rotl vector", sd::rotl(x, rotations),
                [&](int i) { return std::rotl(x[i], int(rotations[i])); });
    check_lanes("rotr vector", sd::rotr(x, rotations),
                [&](int i) { return std::rotr(x[i], int(rotations[i])); });
    for (int s = -w - 1; s <= w + 1; s += 3) {
      check_lanes("rotl", sd::rotl(x, s),
                  [&](int i) { return std::rotl(x[i], s); });
      check_lanes("rotr", sd::rotr(x, s),
                  [&](int i) { return std::rotr(x[i], s); });
      check_lanes("shl unsigned", sd::shl(x, s),
                  [&](int i) { return sd::detail::shl(x[i], s); });
      check_lanes("shr unsigned", sd::shr(x, s),
                  [&](int i) { return sd::detail::shr(x[i], s); });
      const S sx(x);
      check_lanes("shl signed", sd::shl(sx, s),
                  [&](int i) { return sd::detail::shl(sx[i], s); });
      check_lanes("shr signed", sd::shr(sx, s),
                  [&](int i) { return sd::detail::shr(sx[i], s); });
    }
    check_lanes("shl vector", sd::shl(x, rotations),
                [&](int i) { return sd::detail::shl(x[i], rotations[i]); });
    check_lanes("shr vector", sd::shr(x, rotations),
                [&](int i) { return sd::detail::shr(x[i], rotations[i]); });
    for (int l = 1; l <= w + 1; l += 2)
      check_lanes("bit_repeat", sd::bit_repeat(x, l),
                  [&](int i) { return sd::detail::bit_repeat(x[i], l); });
    const S lengths(
        [&](int i) { return static_cast<std::make_signed_t<T>>(1 + i % w); });
    check_lanes("bit_repeat vector", sd::bit_repeat(x, lengths), [&](int i) {
      return sd::detail::bit_repeat(x[i], int(lengths[i]));
    });
    check_lanes("bit_compress", sd::bit_compress(x, m),
                [&](int i) { return sd::detail::bit_compress(x[i], m[i]); });
    check_lanes("bit_expand", sd::bit_expand(x, m),
                [&](int i) { return sd::detail::bit_expand(x[i], m[i]); });
    check_lanes("bit_compress scalar", sd::bit_compress(x, m[0]),
                [&](int i) { return sd::detail::bit_compress(x[i], m[0]); });
    check_lanes("bit_expand scalar", sd::bit_expand(x, m[0]),
                [&](int i) { return sd::detail::bit_expand(x[i], m[0]); });
  }
}

template <class T> void bit_widths() {
  bits<T, 1>();
  bits<T, 3>();
  bits<T, 4>();
  bits<T, 8>();
  bits<T, sd::vec<T>::size()>();
}

} // namespace

TEST_CASE("simd bit: scalar helpers give the known answers") {
  CHECK(sd::detail::byteswap(0x11223344u) == 0x44332211u);
  CHECK(sd::detail::byteswap(static_cast<short>(0x1234)) == 0x3412);
  CHECK(sd::detail::bit_reverse(u8(1)) == 0x80);
  CHECK(sd::detail::bit_reverse(1u) == 0x80000000u);
  CHECK(sd::detail::bit_reverse(0x1ull) == 0x8000000000000000ull);
  CHECK(sd::detail::bit_repeat(u8(0b101), 3) == 0x6d);
  CHECK(sd::detail::bit_repeat(u8(0b1), 1) == 0xff);
  CHECK(sd::detail::bit_repeat(u8(0xa5), 8) == 0xa5);
  CHECK(sd::detail::bit_repeat(u8(0xa5), 20) == 0xa5);
  CHECK(sd::detail::bit_compress(u8(0b1011'0110), u8(0xf0)) == 0xb);
  CHECK(sd::detail::bit_compress(u8(0xf0), u8(0xaa)) == 0xc);
  CHECK(sd::detail::bit_expand(u8(0b11), u8(0xa0)) == 0xa0);
  CHECK(sd::detail::bit_expand(u8(0b01), u8(0xa0)) == 0x20);
  CHECK(sd::detail::bit_expand(0xffffffffu, 0x0f0f0f0fu) == 0x0f0f0f0fu);
  CHECK(sd::detail::shl(-8, 40) == 0);
  CHECK(sd::detail::shl(-8, -1) == -4);
  CHECK(sd::detail::shl(-8, -40) == -1);
  CHECK(sd::detail::shl(8, -40) == 0);
  CHECK(sd::detail::shr(-7, 1) == -4);
  CHECK(sd::detail::shr(0x10u, -2) == 0x40u);
  CHECK(sd::detail::shl(u8(0xff), 4) == 0xf0);
  CHECK(sd::detail::shr(-1, 100u) == -1);
  CHECK(sd::detail::shl(1, std::numeric_limits<long long>::min()) == 0);
  CHECK(sd::detail::shr(1, std::numeric_limits<int>::min()) == 0);
  CHECK(sd::detail::shl(0x40000000, 1) == std::numeric_limits<int>::min());
}

TEST_CASE("simd bit: functions match <bit> lane by lane") {
  bit_widths<unsigned char>();
  bit_widths<unsigned short>();
  bit_widths<unsigned>();
  bit_widths<unsigned long>();
  bit_widths<unsigned long long>();
  check_lanes(
      "signed byteswap",
      sd::byteswap(sd::vec<int, 4>([](int i) { return 0x01020304 * (i + 1); })),
      [](int i) { return sd::detail::byteswap(0x01020304 * (i + 1)); });
}
