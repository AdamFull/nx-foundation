#include "simd/simd_test_util.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <functional>
#include <iterator>
#include <numeric>
#include <ranges>
#include <span>
#include <tuple>
#include <vector>

using namespace simd_test;

namespace {

template <class T, int N> void arithmetic() {
  using V = sd::vec<T, N>;
  rng<T> r(static_cast<u64>(N) * 131 + sizeof(T));
  for (int round = 0; round < 6; ++round) {
    const V a = random_vec<V>(r, wide_signed<T>);
    const V b = random_vec<V>(r, wide_signed<T>);

    check_lanes("a + b", a + b, [&](int i) { return a[i] + b[i]; });
    check_lanes("a - b", a - b, [&](int i) { return a[i] - b[i]; });
    check_lanes("a * b", a * b, [&](int i) { return wrap_mul(a[i], b[i]); });
    check_lanes("-a", -a, [&](int i) { return negate(a[i]); });
    check_lanes("+a", +a, [&](int i) { return +a[i]; });
    check_lanes("a + 3", a + T(3), [&](int i) { return a[i] + T(3); });
    check_lanes("3 - a", T(3) - a, [&](int i) { return T(3) - a[i]; });

    const V d = V([&](int) {
      if constexpr (std::is_floating_point_v<T>)
        return r.any();
      else if constexpr (wide_signed<T>)
        return r.nonzero_small();
      else {
        const T v = r.any();
        return v == T(0) ? T(1) : v;
      }
    });
    check_lanes("a / d", a / d, [&](int i) { return a[i] / d[i]; });

    V c = a;
    c += b;
    check_lanes("+=", c, [&](int i) { return a[i] + b[i]; });
    c = a;
    c -= b;
    check_lanes("-=", c, [&](int i) { return a[i] - b[i]; });
    c = a;
    c *= b;
    check_lanes("*=", c, [&](int i) { return wrap_mul(a[i], b[i]); });
    c = a;
    c /= d;
    check_lanes("/=", c, [&](int i) { return a[i] / d[i]; });

    const auto up = [&](int i) { return static_cast<T>(a[i] + T(1)); };
    const auto up2 = [&](int i) { return static_cast<T>(up(i) + T(1)); };
    const auto down = [&](int i) { return static_cast<T>(up2(i) - T(1)); };
    const auto down2 = [&](int i) { return static_cast<T>(down(i) - T(1)); };
    c = a;
    check_lanes("pre ++", ++c, up);
    check_lanes("post ++", c++, up);
    check_lanes("after post ++", c, up2);
    check_lanes("pre --", --c, down);
    check_lanes("post --", c--, down);
    check_lanes("after post --", c, down2);

    check_mask("a == b", a == b, [&](int i) { return a[i] == b[i]; });
    check_mask("a == a", a == a, [&](int i) { return a[i] == a[i]; });
    check_mask("a != b", a != b, [&](int i) { return a[i] != b[i]; });
    check_mask("a < b", a < b, [&](int i) { return a[i] < b[i]; });
    check_mask("a <= b", a <= b, [&](int i) { return a[i] <= b[i]; });
    check_mask("a > b", a > b, [&](int i) { return a[i] > b[i]; });
    check_mask("a >= b", a >= b, [&](int i) { return a[i] >= b[i]; });
    const V z = sd::select(a < b, a, V(T(0)));
    check_mask("!z", !z, [&](int i) { return !z[i]; });

    if constexpr (std::is_integral_v<T>) {
      check_lanes("a % d", a % d, [&](int i) { return a[i] % d[i]; });
      check_lanes("a & b", a & b, [&](int i) { return a[i] & b[i]; });
      check_lanes("a | b", a | b, [&](int i) { return a[i] | b[i]; });
      check_lanes("a ^ b", a ^ b, [&](int i) { return a[i] ^ b[i]; });
      check_lanes("~a", ~a, [&](int i) { return ~a[i]; });
      c = a;
      c %= d;
      check_lanes("%=", c, [&](int i) { return a[i] % d[i]; });
      c = a;
      c &= b;
      check_lanes("&=", c, [&](int i) { return a[i] & b[i]; });
      c = a;
      c |= b;
      check_lanes("|=", c, [&](int i) { return a[i] | b[i]; });
      c = a;
      c ^= b;
      check_lanes("^=", c, [&](int i) { return a[i] ^ b[i]; });

      const V full = random_vec<V>(r, false);
      const V counts = V([&](int) {
        return static_cast<T>(r.next() % static_cast<u64>(shift_limit<T>()));
      });
      check_lanes("a << counts", full << counts,
                  [&](int i) { return full[i] << counts[i]; });
      check_lanes("a >> counts", full >> counts,
                  [&](int i) { return full[i] >> counts[i]; });
      for (int n = 0; n < int(shift_limit<T>()); n += 3) {
        check_lanes("a << n", full << n, [&](int i) { return full[i] << n; });
        check_lanes("a >> n", full >> n, [&](int i) { return full[i] >> n; });
        c = full;
        c <<= n;
        check_lanes("<<= n", c, [&](int i) { return full[i] << n; });
        c = full;
        c >>= n;
        check_lanes(">>= n", c, [&](int i) { return full[i] >> n; });
      }
      c = full;
      c <<= counts;
      check_lanes("<<= counts", c, [&](int i) { return full[i] << counts[i]; });
      c = full;
      c >>= counts;
      check_lanes(">>= counts", c, [&](int i) { return full[i] >> counts[i]; });
    }
  }

  if constexpr (std::is_integral_v<T> && std::is_signed_v<T> &&
                sizeof(T) < sizeof(int)) {
    const V lowest(std::numeric_limits<T>::lowest());
    check_lanes("lowest / -1", lowest / V(T(-1)),
                [&](int) { return std::numeric_limits<T>::lowest() / -1; });
    check_lanes("lowest % -1", lowest % V(T(-1)), [&](int) { return 0; });
    check_lanes("-lowest", -lowest,
                [&](int) { return -std::numeric_limits<T>::lowest(); });
  }
}

// One width per kind of storage: a scalar, an odd array, part of a
// register, a whole register, several registers, and a non-power-of-two run
// of registers.
template <class T> void arithmetic_widths() {
  constexpr int native = sd::vec<T>::size();
  arithmetic<T, 1>();
  arithmetic<T, 3>();
  arithmetic<T, 4>();
  arithmetic<T, native>();
  arithmetic<T, (native * 2 <= 64 ? native * 2 : native / 2)>();
  arithmetic<T, 12>();
  if constexpr (sizeof(T) == 1)
    arithmetic<T, 64>();
}

template <class T> void float_specials() {
  using V = sd::vec<T>;
  constexpr T inf = std::numeric_limits<T>::infinity();
  constexpr T nan = std::numeric_limits<T>::quiet_NaN();
  const T values[] = {T(0),
                      T(-0.0),
                      inf,
                      -inf,
                      nan,
                      T(1),
                      T(-1),
                      std::numeric_limits<T>::denorm_min(),
                      std::numeric_limits<T>::max(),
                      std::numeric_limits<T>::lowest()};
  constexpr int count = int(std::size(values));
  for (int s = 0; s < count; ++s) {
    const V a([&](int i) { return values[(i + s) % count]; });
    const V b([&](int i) { return values[(i * 3 + s + 1) % count]; });
    check_lanes("special a + b", a + b, [&](int i) { return a[i] + b[i]; });
    check_lanes("special a * b", a * b, [&](int i) { return a[i] * b[i]; });
    check_lanes("special a / b", a / b, [&](int i) { return a[i] / b[i]; });
    check_lanes("special -a", -a, [&](int i) { return -a[i]; });
    check_mask("special a == b", a == b, [&](int i) { return a[i] == b[i]; });
    check_mask("special a != b", a != b, [&](int i) { return a[i] != b[i]; });
    check_mask("special a < b", a < b, [&](int i) { return a[i] < b[i]; });
    check_mask("special a <= b", a <= b, [&](int i) { return a[i] <= b[i]; });
    check_mask("special !a", !a, [&](int i) { return !a[i]; });
    check_lanes("special min", sd::min(a, b),
                [&](int i) { return std::min(a[i], b[i]); });
    check_lanes("special max", sd::max(a, b),
                [&](int i) { return std::max(a[i], b[i]); });
  }
}

} // namespace

TEST_CASE("simd: arithmetic matches scalar code for every element type") {
  arithmetic_widths<signed char>();
  arithmetic_widths<unsigned char>();
  arithmetic_widths<char>();
  arithmetic_widths<short>();
  arithmetic_widths<unsigned short>();
  arithmetic_widths<int>();
  arithmetic_widths<unsigned>();
  arithmetic_widths<long>();
  arithmetic_widths<unsigned long>();
  arithmetic_widths<long long>();
  arithmetic_widths<unsigned long long>();
  arithmetic_widths<float>();
  arithmetic_widths<double>();
}

TEST_CASE("simd: character types behave as their integer storage") {
  arithmetic<wchar_t, 3>();
  arithmetic<wchar_t, sd::vec<wchar_t>::size()>();
  arithmetic<char8_t, 3>();
  arithmetic<char8_t, sd::vec<char8_t>::size()>();
  arithmetic<char16_t, 3>();
  arithmetic<char16_t, sd::vec<char16_t>::size()>();
  arithmetic<char32_t, 3>();
  arithmetic<char32_t, sd::vec<char32_t>::size()>();
}

TEST_CASE("simd: floating-point specials follow IEEE and std::min/max") {
  float_specials<float>();
  float_specials<double>();
}

TEST_CASE("simd: shifts by a count and by a vector are told apart") {
  const sd::vec<int, 4> v([](int i) { return 1 << i; });
  check_lanes("v << 2", v << 2, [&](int i) { return (1 << i) << 2; });
  check_lanes("v << v", v << sd::vec<int, 4>(1), [&](int i) { return 2 << i; });
  const sd::vec<u8, 16> bytes(u8(0x81));
  check_lanes("u8 << 9", bytes << 9, [](int) { return u8(0); });
  check_lanes("u8 >> 7", bytes >> 7, [](int) { return u8(1); });
  const sd::vec<i8, 16> signed_bytes(i8(-128));
  check_lanes("i8 >> 20", signed_bytes >> 20, [](int) { return i8(-1); });
  check_lanes("i8 << 7", sd::vec<i8, 16>(i8(3)) << 7,
              [](int) { return i8(-128); });
}
