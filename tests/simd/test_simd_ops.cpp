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

template <class T, int N> void min_max_clamp() {
  using V = sd::vec<T, N>;
  rng<T> r(N * 7 + sizeof(T));
  for (int round = 0; round < 4; ++round) {
    const V a = random_vec<V>(r, false);
    const V b = random_vec<V>(r, false);
    check_lanes("min", sd::min(a, b),
                [&](int i) { return std::min(a[i], b[i]); });
    check_lanes("max", sd::max(a, b),
                [&](int i) { return std::max(a[i], b[i]); });
    check_lanes("nx::min", nx::min(a, b),
                [&](int i) { return std::min(a[i], b[i]); });
    const auto [lo, hi] = sd::minmax(a, b);
    check_lanes("minmax first", lo,
                [&](int i) { return std::min(a[i], b[i]); });
    check_lanes("minmax second", hi,
                [&](int i) { return std::max(a[i], b[i]); });
    const V l = sd::min(a, b);
    const V h = sd::max(a, b);
    const V x = random_vec<V>(r, false);
    check_lanes("clamp", sd::clamp(x, l, h),
                [&](int i) { return std::clamp(x[i], l[i], h[i]); });
    check_lanes("select", sd::select(a < b, x, a),
                [&](int i) { return a[i] < b[i] ? x[i] : a[i]; });
    check_lanes("select scalar", sd::select(a < b, x, T(7)),
                [&](int i) { return a[i] < b[i] ? x[i] : T(7); });
    check_lanes("select scalar first", sd::select(a < b, T(5), x),
                [&](int i) { return a[i] < b[i] ? T(5) : x[i]; });
  }
}

template <class T> void min_max_widths() {
  min_max_clamp<T, 1>();
  min_max_clamp<T, 3>();
  min_max_clamp<T, 4>();
  min_max_clamp<T, 8>();
  min_max_clamp<T, 16>();
  min_max_clamp<T, sd::vec<T>::size()>();
}

template <class T, int N> void reductions() {
  using V = sd::vec<T, N>;
  using M = typename V::mask_type;
  rng<T> r(N * 3 + sizeof(T) * 1000);
  for (int round = 0; round < 6; ++round) {
    // Small integral values keep every float sum exact, whatever the order.
    const V a([&](int) {
      if constexpr (std::is_floating_point_v<T>)
        return static_cast<T>(static_cast<int>(r.next() % 64) - 32);
      else
        return r.any();
    });
    const M k([&](int) { return r.flip(); });

    T sum = T(0), product = T(1);
    T minimum = a[0], maximum = a[0];
    T kept_sum = T(0);
    T kept_min = std::numeric_limits<T>::max();
    T kept_max = std::numeric_limits<T>::lowest();
    for (int i = 0; i < N; ++i) {
      sum = wrap_add(sum, a[i]);
      minimum = std::min(minimum, a[i]);
      maximum = std::max(maximum, a[i]);
      if (k[i]) {
        kept_sum = wrap_add(kept_sum, a[i]);
        kept_min = std::min(kept_min, a[i]);
        kept_max = std::max(kept_max, a[i]);
      }
    }
    CHECK(same(sd::reduce(a), sum));
    CHECK(same(sd::reduce(a, std::plus<>{}), sum));
    CHECK(same(sd::reduce(a, k), kept_sum));
    CHECK(same(sd::reduce_min(a), minimum));
    CHECK(same(sd::reduce_max(a), maximum));
    CHECK(same(sd::reduce_min(a, k), kept_min));
    CHECK(same(sd::reduce_max(a, k), kept_max));
    CHECK(same(sd::reduce(a, M(false), std::plus<>{}, T(0)), T(0)));
    CHECK(same(sd::reduce(a, M(false)), T(0)));
    CHECK(same(sd::reduce(a, M(false), std::multiplies<>{}), T(1)));

    const V p = [&] {
      if constexpr (std::is_floating_point_v<T>)
        return V([&](int) { return (r.next() & 1) ? T(2) : T(0.5); });
      else
        return a;
    }();
    for (int i = 0; i < N; ++i)
      product = wrap_mul(product, p[i]);
    CHECK(same(sd::reduce(p, std::multiplies<>{}), product));

    const auto custom = [](const sd::vec<T, 1> &x, const sd::vec<T, 1> &y) {
      return sd::max(x, y);
    };
    CHECK(same(sd::reduce(a, custom), maximum));
    if (sd::any_of(k))
      CHECK(same(sd::reduce(a, k, custom, std::numeric_limits<T>::lowest()),
                 kept_max));
    else
      CHECK(same(sd::reduce(a, k, custom, std::numeric_limits<T>::lowest()),
                 std::numeric_limits<T>::lowest()));

    if constexpr (std::is_integral_v<T>) {
      T all_and = static_cast<T>(~T(0)), all_or = T(0), all_xor = T(0);
      T kept_and = static_cast<T>(~T(0));
      for (int i = 0; i < N; ++i) {
        all_and = static_cast<T>(all_and & a[i]);
        all_or = static_cast<T>(all_or | a[i]);
        all_xor = static_cast<T>(all_xor ^ a[i]);
        if (k[i])
          kept_and = static_cast<T>(kept_and & a[i]);
      }
      CHECK(sd::reduce(a, std::bit_and<>{}) == all_and);
      CHECK(sd::reduce(a, std::bit_or<>{}) == all_or);
      CHECK(sd::reduce(a, std::bit_xor<>{}) == all_xor);
      CHECK(sd::reduce(a, k, std::bit_and<>{}) == kept_and);
    }
  }
  CHECK(same(sd::reduce_min(V(T(3)), M(false)), std::numeric_limits<T>::max()));
  CHECK(same(sd::reduce_max(V(T(3)), M(false)),
             std::numeric_limits<T>::lowest()));
}

template <class T> void reduction_widths() {
  reductions<T, 1>();
  reductions<T, 2>();
  reductions<T, 3>();
  reductions<T, 4>();
  reductions<T, 8>();
  reductions<T, 12>();
  reductions<T, 16>();
  reductions<T, sd::vec<T>::size()>();
  if constexpr (sizeof(T) <= 2)
    reductions<T, 32>();
}

template <class T, int N> void masks() {
  using M = sd::mask<T, N>;
  rng<u64> r(N * 17 + sizeof(T));
  for (int round = 0; round < 8; ++round) {
    bool av[N], bv[N];
    for (int i = 0; i < N; ++i) {
      av[i] = r.flip();
      bv[i] = r.flip();
    }
    if (round == 0)
      std::fill(av, av + N, false);
    if (round == 1)
      std::fill(av, av + N, true);
    const M a([&](int i) { return av[i]; });
    const M b([&](int i) { return bv[i]; });

    check_mask("generator", a, [&](int i) { return av[i]; });
    check_mask("a && b", a && b, [&](int i) { return av[i] && bv[i]; });
    check_mask("a || b", a || b, [&](int i) { return av[i] || bv[i]; });
    check_mask("a & b", a & b, [&](int i) { return av[i] && bv[i]; });
    check_mask("a | b", a | b, [&](int i) { return av[i] || bv[i]; });
    check_mask("a ^ b", a ^ b, [&](int i) { return av[i] != bv[i]; });
    check_mask("!a", !a, [&](int i) { return !av[i]; });
    check_mask("a == b", a == b, [&](int i) { return av[i] == bv[i]; });
    check_mask("a != b", a != b, [&](int i) { return av[i] != bv[i]; });
    check_mask("a < b", a < b, [&](int i) { return av[i] < bv[i]; });
    check_mask("a <= b", a <= b, [&](int i) { return av[i] <= bv[i]; });
    check_mask("a > b", a > b, [&](int i) { return av[i] > bv[i]; });
    check_mask("a >= b", a >= b, [&](int i) { return av[i] >= bv[i]; });
    M c = a;
    c &= b;
    check_mask("&=", c, [&](int i) { return av[i] && bv[i]; });
    c = a;
    c |= b;
    check_mask("|=", c, [&](int i) { return av[i] || bv[i]; });
    c = a;
    c ^= b;
    check_mask("^=", c, [&](int i) { return av[i] != bv[i]; });
    check_mask("select mask", sd::select(a, b, !b),
               [&](int i) { return av[i] ? bv[i] : !bv[i]; });
    check_mask("select bool", sd::select(a, true, false),
               [&](int i) { return av[i]; });

    int count = 0, first = -1, last = -1;
    u64 bits = 0;
    std::bitset<N> set;
    for (int i = 0; i < N; ++i) {
      if (av[i]) {
        ++count;
        if (first < 0)
          first = i;
        last = i;
        bits |= u64{1} << i;
        set[size_t(i)] = true;
      }
    }
    CHECK(sd::all_of(a) == (count == N));
    CHECK(sd::any_of(a) == (count > 0));
    CHECK(sd::none_of(a) == (count == 0));
    CHECK(sd::reduce_count(a) == count);
    if (count > 0) {
      CHECK(sd::reduce_min_index(a) == first);
      CHECK(sd::reduce_max_index(a) == last);
    }
    CHECK(a.to_ullong() == bits);
    CHECK(a.to_bitset() == set);
    check_mask("from bitset", M(set), [&](int i) { return av[i]; });
    check_mask("from bits", M(static_cast<unsigned long long>(bits)),
               [&](int i) { return av[i]; });

    if constexpr (sizeof(T) <= 8) {
      using I = sd::detail::integer_from<sizeof(T)>;
      check_lanes("+mask", +a, [&](int i) { return I(av[i] ? 1 : 0); });
      check_lanes("-mask", -a, [&](int i) { return I(av[i] ? -1 : 0); });
      check_lanes("~mask", ~a, [&](int i) { return I(av[i] ? -2 : -1); });
      const auto v = static_cast<sd::vec<T, N>>(a);
      check_lanes("mask to vec", v, [&](int i) { return T(av[i] ? 1 : 0); });
      const sd::vec<double, N> dv(a);
      check_lanes("mask to double vec", dv,
                  [&](int i) { return av[i] ? 1.0 : 0.0; });
    }

    const sd::mask<u8, N> narrow(a);
    check_mask("to 1-byte mask", narrow, [&](int i) { return av[i]; });
    const sd::mask<double, N> wide(a);
    check_mask("to 8-byte mask", wide, [&](int i) { return av[i]; });
    const sd::mask<short, N> shorts(wide);
    check_mask("8- to 2-byte mask", shorts, [&](int i) { return av[i]; });

    int seen = 0;
    for (bool lane : a)
      CHECK(lane == av[seen++]);
    CHECK(seen == N);
  }
  CHECK(sd::all_of(M(true)));
  CHECK(sd::none_of(M(false)));
  if constexpr (N > 1) {
    const M low(0b1u);
    CHECK(low[0]);
    CHECK_FALSE(low[1]);
  }
}

template <class T> void mask_widths() {
  masks<T, 1>();
  masks<T, 2>();
  masks<T, 3>();
  masks<T, 4>();
  masks<T, 8>();
  masks<T, 12>();
  masks<T, 16>();
  masks<T, 32>();
  masks<T, 64>();
}

} // namespace

TEST_CASE("simd: min, max, minmax, clamp and select") {
  min_max_widths<signed char>();
  min_max_widths<unsigned char>();
  min_max_widths<short>();
  min_max_widths<unsigned short>();
  min_max_widths<int>();
  min_max_widths<unsigned>();
  min_max_widths<long long>();
  min_max_widths<unsigned long long>();
  min_max_widths<float>();
  min_max_widths<double>();
}

TEST_CASE("simd: reductions with and without masks") {
  reduction_widths<signed char>();
  reduction_widths<unsigned char>();
  reduction_widths<short>();
  reduction_widths<unsigned short>();
  reduction_widths<int>();
  reduction_widths<unsigned>();
  reduction_widths<long long>();
  reduction_widths<unsigned long long>();
  reduction_widths<float>();
  reduction_widths<double>();
}

TEST_CASE("simd: scalar reduction overloads") {
  CHECK(sd::reduce(5) == 5);
  CHECK(sd::reduce(5, false) == 0);
  CHECK(sd::reduce(5, false, std::multiplies<>{}) == 1);
  CHECK(sd::reduce(5, true, std::multiplies<>{}) == 5);
  CHECK(sd::reduce_min(5, false) == std::numeric_limits<int>::max());
  CHECK(sd::reduce_max(5.f, false) == std::numeric_limits<float>::lowest());
  CHECK(sd::reduce_max(5.f, true) == 5.f);
  CHECK(sd::all_of(true));
  CHECK_FALSE(sd::any_of(false));
  CHECK(sd::none_of(false));
  CHECK(sd::reduce_count(true) == 1);
  CHECK(sd::reduce_min_index(true) == 0);
  CHECK(sd::reduce_max_index(true) == 0);
}

TEST_CASE("simd: masks of every element size and width") {
  mask_widths<i8>();
  mask_widths<i16>();
  mask_widths<float>();
  mask_widths<double>();
  mask_widths<std::complex<double>>();
}
