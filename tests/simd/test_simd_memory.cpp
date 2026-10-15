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
#include <utility>
#include <vector>

using namespace simd_test;

namespace {

template <class From, class To, int N> void conversion() {
  using F = sd::vec<From, N>;
  using V = sd::vec<To, N>;
  rng<From> r(N + sizeof(From) * 10 + sizeof(To));
  const F x([&](int) {
    if constexpr (std::is_floating_point_v<From> && std::is_integral_v<To>) {
      constexpr long long lo =
          std::is_signed_v<To> ? (sizeof(To) == 1 ? -100 : -30000) : 0;
      constexpr long long hi = sizeof(To) == 1 ? 100 : 30000;
      return static_cast<From>(
                 lo + static_cast<long long>(r.next() % u64(hi - lo))) +
             static_cast<From>(0.25);
    } else if constexpr (std::is_floating_point_v<From> &&
                         std::is_floating_point_v<To>) {
      return static_cast<From>(r.small());
    } else {
      return r.any();
    }
  });
  const V y(x);
  check_lanes("convert", y, [&](int i) { return static_cast<To>(x[i]); });
}

template <class From, int N> void conversions_from() {
  conversion<From, signed char, N>();
  conversion<From, unsigned char, N>();
  conversion<From, short, N>();
  conversion<From, unsigned short, N>();
  conversion<From, int, N>();
  conversion<From, unsigned, N>();
  conversion<From, long long, N>();
  conversion<From, unsigned long long, N>();
  conversion<From, float, N>();
  conversion<From, double, N>();
}

template <int N> void conversions() {
  conversions_from<signed char, N>();
  conversions_from<unsigned char, N>();
  conversions_from<short, N>();
  conversions_from<unsigned short, N>();
  conversions_from<int, N>();
  conversions_from<unsigned, N>();
  conversions_from<long long, N>();
  conversions_from<unsigned long long, N>();
  conversions_from<float, N>();
  conversions_from<double, N>();
}

template <class T, int N> void loads_and_stores() {
  using V = sd::vec<T, N>;
  using M = typename V::mask_type;
  constexpr T sentinel = T(99);
  alignas(64) T source[N + 8];
  for (int i = 0; i < N + 8; ++i)
    source[i] = static_cast<T>(i * 3 + 1);

  const V full = sd::unchecked_load<V>(source, sd::flag_aligned);
  check_lanes("unchecked_load", full, [&](int i) { return source[i]; });
  check_lanes("unchecked_load +1", sd::unchecked_load<V>(source + 1, N + 7),
              [&](int i) { return source[i + 1]; });
  check_lanes("unchecked_load span",
              sd::unchecked_load<V>(std::span<const T>(source, N)),
              [&](int i) { return source[i]; });
  check_lanes("unchecked_load iterator pair",
              sd::unchecked_load<V>(source + 2, source + 2 + N),
              [&](int i) { return source[i + 2]; });
  check_lanes("unchecked_load overaligned",
              sd::unchecked_load<V>(source, sd::flag_overaligned<64>),
              [&](int i) { return source[i]; });
  for (int n = 0; n <= N + 1; ++n) {
    check_lanes("partial_load", sd::partial_load<V>(source, n),
                [&](int i) { return i < n ? source[i] : T(); });
    check_lanes("partial_load iterator pair",
                sd::partial_load<V>(source, source + n),
                [&](int i) { return i < n ? source[i] : T(); });
  }

  const M k([](int i) { return i % 3 != 1; });
  check_lanes("masked unchecked_load", sd::unchecked_load<V>(source, N, k),
              [&](int i) { return k[i] ? source[i] : T(); });
  check_lanes("masked partial_load", sd::partial_load<V>(source, N / 2, k),
              [&](int i) { return k[i] && i < N / 2 ? source[i] : T(); });

  const V v([](int i) { return static_cast<T>(50 - i); });
  T out[N + 4];
  std::fill(std::begin(out), std::end(out), sentinel);
  sd::unchecked_store(v, out, N + 4);
  for (int i = 0; i < N; ++i)
    CHECK(same(out[i], v[i]));
  CHECK(same(out[N], sentinel));

  for (int n = 0; n <= N + 1; ++n) {
    std::fill(std::begin(out), std::end(out), sentinel);
    sd::partial_store(v, out, out + std::min(n, N + 4));
    for (int i = 0; i < N + 4; ++i)
      CHECK(same(out[i], i < n && i < N ? v[i] : sentinel));
  }

  std::fill(std::begin(out), std::end(out), sentinel);
  sd::unchecked_store(v, std::span<T>(out, N), k);
  for (int i = 0; i < N + 4; ++i)
    CHECK(same(out[i], i < N && k[i] ? v[i] : sentinel));

  std::fill(std::begin(out), std::end(out), sentinel);
  sd::partial_store(v, out, N / 2, k);
  for (int i = 0; i < N + 4; ++i)
    CHECK(same(out[i], i < N / 2 && k[i] ? v[i] : sentinel));

  std::array<T, N> array{};
  std::copy(source, source + N, array.begin());
  check_lanes("range constructor", V(array), [&](int i) { return source[i]; });
  check_lanes("masked range constructor", V(array, k),
              [&](int i) { return k[i] ? source[i] : T(); });
  const sd::basic_vec deduced(array);
  static_assert(std::is_same_v<std::remove_const_t<decltype(deduced)>, V>);

  if constexpr (std::is_integral_v<T> && sizeof(T) <= 2) {
    int ints[N];
    for (int i = 0; i < N; ++i)
      ints[i] = static_cast<int>(source[i]) + 1000;
    check_lanes("converting load",
                sd::unchecked_load<V>(ints, N, sd::flag_convert),
                [&](int i) { return static_cast<T>(ints[i]); });
    const sd::vec<int, N> wide = sd::unchecked_load<sd::vec<int, N>>(source, N);
    check_lanes("widening load", wide, [&](int i) { return int(source[i]); });
    int wide_out[N];
    sd::unchecked_store(v, wide_out, N);
    for (int i = 0; i < N; ++i)
      CHECK(wide_out[i] == int(v[i]));
    sd::unchecked_store(sd::vec<int, N>(wide) + 1, out, N, sd::flag_convert);
    for (int i = 0; i < N; ++i)
      CHECK(out[i] == static_cast<T>(source[i] + 1));
  }
  if constexpr (std::is_same_v<T, float>) {
    double doubles[N];
    for (int i = 0; i < N; ++i)
      doubles[i] = i + 0.5;
    check_lanes("double to float load",
                sd::unchecked_load<V>(doubles, N, sd::flag_convert),
                [&](int i) { return static_cast<float>(doubles[i]); });
    const auto as_double = sd::unchecked_load<sd::vec<double, N>>(source, N);
    check_lanes("float to double load", as_double,
                [&](int i) { return double(source[i]); });
  }

  std::vector<T> dynamic(source, source + N + 3);
  check_lanes("vector range load", sd::partial_load<V>(dynamic),
              [&](int i) { return dynamic[size_t(i)]; });
  const auto native = sd::partial_load(dynamic);
  static_assert(
      std::is_same_v<std::remove_const_t<decltype(native)>, sd::basic_vec<T>>);
}

template <class T> void load_store_widths() {
  loads_and_stores<T, 1>();
  loads_and_stores<T, 3>();
  loads_and_stores<T, 4>();
  loads_and_stores<T, 8>();
  loads_and_stores<T, 12>();
  loads_and_stores<T, 16>();
  loads_and_stores<T, sd::vec<T>::size()>();
}

template <class T, int N> void permutations() {
  using V = sd::vec<T, N>;
  using I = sd::rebind_t<int, V>;
  const V v([](int i) { return static_cast<T>(i + 1); });

  check_lanes("permute reverse",
              sd::permute(v, [](int i, int size) { return size - 1 - i; }),
              [&](int i) { return v[N - 1 - i]; });
  check_lanes(
      "permute pairs",
      sd::permute(
          v, [](int i) { return (i ^ 1) < N ? (i ^ 1) : sd::zero_element; }),
      [&](int i) { return (i ^ 1) < N ? v[i ^ 1] : T(); });
  const auto zeroed =
      sd::permute(v, [](int i) { return i % 2 == 0 ? sd::zero_element : i; });
  check_lanes("permute zero_element", zeroed,
              [&](int i) { return i % 2 == 0 ? T() : v[i]; });
  const auto twice = sd::permute<2 * N>(v, [](int i) { return i / 2; });
  static_assert(decltype(twice)::size() == 2 * N);
  check_lanes("permute widening", twice, [&](int i) { return v[i / 2]; });
  if constexpr (N >= 2) {
    const auto first_half = sd::permute<N / 2>(v, [](int i) { return i; });
    check_lanes("permute narrowing", first_half, [&](int i) { return v[i]; });
  }
  int offset = 1;
  check_lanes("permute stateful",
              sd::permute(v, [&](int i) { return (i + offset) % N; }),
              [&](int i) { return v[(i + 1) % N]; });

  const I indices([](int i) { return (i * 5 + 3) % N; });
  check_lanes("dynamic permute", sd::permute(v, indices),
              [&](int i) { return v[(i * 5 + 3) % N]; });
  check_lanes("subscript permute", v[indices],
              [&](int i) { return v[(i * 5 + 3) % N]; });

  const auto k = v > T(N / 2);
  check_mask("mask permute",
             sd::permute(k, [](int i, int size) { return size - 1 - i; }),
             [&](int i) { return k[N - 1 - i]; });
  check_mask("mask dynamic permute", k[indices],
             [&](int i) { return k[(i * 5 + 3) % N]; });

  const auto selector = typename V::mask_type([](int i) { return i % 3 != 0; });
  int selected[N];
  int count = 0;
  for (int i = 0; i < N; ++i)
    if (selector[i])
      selected[count++] = i;
  check_lanes("compress with fill", sd::compress(v, selector, T(77)),
              [&](int i) { return i < count ? v[selected[i]] : T(77); });
  const V packed = sd::compress(v, selector);
  for (int i = 0; i < count; ++i)
    CHECK(same(packed[i], v[selected[i]]));
  check_mask("mask compress", sd::compress(k, selector, true),
             [&](int i) { return i < count ? k[selected[i]] : true; });

  const V original(T(9));
  int next = 0;
  check_lanes("expand", sd::expand(v, selector, original),
              [&](int i) { return selector[i] ? v[next++] : T(9); });
  next = 0;
  check_lanes("expand default", sd::expand(v, selector),
              [&](int i) { return selector[i] ? v[next++] : T(); });
}

template <class T> void permutation_widths() {
  permutations<T, 1>();
  permutations<T, 3>();
  permutations<T, 4>();
  permutations<T, 8>();
  permutations<T, 12>();
  permutations<T, 16>();
  permutations<T, sd::vec<T>::size()>();
}

template <class T, int N> void gather_scatter() {
  using V = sd::vec<T, N>;
  using I = sd::rebind_t<int, V>;
  T table[40];
  for (int i = 0; i < 40; ++i)
    table[i] = static_cast<T>(i * 2 + 1);
  const I indices([](int i) { return (i * 7 + 2) % 40; });

  check_lanes("unchecked_gather_from",
              sd::unchecked_gather_from(table, indices),
              [&](int i) { return table[(i * 7 + 2) % 40]; });
  const auto k = typename I::mask_type([](int i) { return i % 2 == 0; });
  check_lanes("masked gather", sd::unchecked_gather_from(table, k, indices),
              [&](int i) { return k[i] ? table[(i * 7 + 2) % 40] : T(); });

  const I wild(
      [](int i) { return i % 3 == 0 ? 100 + i : (i % 3 == 1 ? -1 - i : i); });
  check_lanes("partial_gather_from", sd::partial_gather_from(table, wild),
              [&](int i) { return i % 3 == 2 ? table[i] : T(); });
  check_lanes("partial gather with mask",
              sd::partial_gather_from(table, k, wild),
              [&](int i) { return k[i] && i % 3 == 2 ? table[i] : T(); });

  const auto as_double = sd::unchecked_gather_from<sd::vec<double, N>>(
      table, indices, sd::flag_convert);
  check_lanes("converting gather", as_double,
              [&](int i) { return double(table[(i * 7 + 2) % 40]); });

  const V v([](int i) { return static_cast<T>(200 + i); });
  T out[40];
  std::fill(std::begin(out), std::end(out), T(0));
  sd::unchecked_scatter_to(v, out, indices);
  for (int i = 0; i < N; ++i)
    CHECK(same(out[(i * 7 + 2) % 40], v[i]));

  std::fill(std::begin(out), std::end(out), T(0));
  sd::partial_scatter_to(v, out, wild);
  for (int i = 0; i < 40; ++i)
    CHECK(same(out[i], i < N && i % 3 == 2 ? v[i] : T(0)));

  std::fill(std::begin(out), std::end(out), T(0));
  sd::unchecked_scatter_to(v, out, k, indices);
  int written = 0;
  for (int i = 0; i < 40; ++i)
    written += out[i] != T(0);
  CHECK(written == sd::reduce_count(k));
}

template <class T> void chunk_and_cat() {
  using V8 = sd::vec<T, 8>;
  const V8 v([](int i) { return static_cast<T>(i + 1); });

  const auto halves = sd::chunk<4>(v);
  static_assert(std::is_same_v<std::remove_const_t<decltype(halves)>,
                               std::array<sd::vec<T, 4>, 2>>);
  check_lanes("chunk 0", halves[0], [&](int i) { return v[i]; });
  check_lanes("chunk 1", halves[1], [&](int i) { return v[i + 4]; });

  const auto uneven = sd::chunk<3>(v);
  static_assert(std::tuple_size_v<std::remove_const_t<decltype(uneven)>> == 3);
  check_lanes("chunk tuple 0", std::get<0>(uneven),
              [&](int i) { return v[i]; });
  check_lanes("chunk tuple 1", std::get<1>(uneven),
              [&](int i) { return v[i + 3]; });
  check_lanes("chunk tuple rest", std::get<2>(uneven),
              [&](int i) { return v[i + 6]; });
  static_assert(std::remove_cvref_t<decltype(std::get<2>(uneven))>::size() ==
                2);

  const auto by_type = sd::chunk<sd::vec<T, 2>>(v);
  check_lanes("chunk by type", by_type[3], [&](int i) { return v[i + 6]; });

  constexpr int W = sd::vec<T>::size();
  const auto native = sd::chunk<sd::vec<T>>(
      sd::vec<T, 2 * W>([](int i) { return static_cast<T>(i); }));
  check_lanes("chunk native", native[1],
              [&](int i) { return static_cast<T>(i + W); });

  const auto k = v > T(3);
  const auto mask_parts = sd::chunk<4>(k);
  check_mask("chunk mask", mask_parts[1], [&](int i) { return k[i + 4]; });

  const auto joined = sd::cat(halves[1], halves[0], sd::vec<T, 1>(T(42)));
  static_assert(decltype(joined)::size() == 9);
  check_lanes("cat", joined, [&](int i) {
    return i < 4 ? v[i + 4] : i < 8 ? v[i - 4] : T(42);
  });
  const auto joined_mask = sd::cat(mask_parts[1], mask_parts[0]);
  check_mask("cat mask", joined_mask,
             [&](int i) { return i < 4 ? k[i + 4] : k[i - 4]; });
}

template <class T> void iterators() {
  using V = sd::vec<T, 8>;
  static_assert(std::random_access_iterator<typename V::iterator>);
  static_assert(std::random_access_iterator<typename V::const_iterator>);
  static_assert(
      std::sized_sentinel_for<std::default_sentinel_t, typename V::iterator>);
  static_assert(std::ranges::random_access_range<V>);
  static_assert(std::ranges::sized_range<V>);
  static_assert(std::random_access_iterator<typename V::mask_type::iterator>);

  const V v([](int i) { return static_cast<T>(10 - i); });
  T expected = T(10);
  for (T x : v)
    CHECK(same(x, expected--));
  CHECK(std::ranges::distance(v) == 8);
  CHECK(same(*std::ranges::min_element(v), T(3)));
  CHECK(std::ranges::count_if(v, [](T x) { return x > T(5); }) == 5);
  auto it = v.begin();
  it += 3;
  CHECK(same(*it, T(7)));
  CHECK(same(it[2], T(5)));
  CHECK(std::default_sentinel - it == 5);
  CHECK(it - v.cbegin() == 3);
  typename V::const_iterator cit = v.begin();
  CHECK(cit < it);
  CHECK((it + 5 == std::default_sentinel));
}

} // namespace

TEST_CASE("simd: conversions between all element types") {
  conversions<1>();
  conversions<4>();
  conversions<8>();
  conversions<16>();
  conversions<3>();
}

TEST_CASE("simd: loads and stores") {
  load_store_widths<signed char>();
  load_store_widths<unsigned short>();
  load_store_widths<int>();
  load_store_widths<unsigned long long>();
  load_store_widths<float>();
  load_store_widths<double>();
}

TEST_CASE("simd: permute, compress and expand") {
  permutation_widths<u8>();
  permutation_widths<short>();
  permutation_widths<int>();
  permutation_widths<float>();
  permutation_widths<double>();
}

TEST_CASE("simd: gather and scatter") {
  gather_scatter<int, 4>();
  gather_scatter<int, 8>();
  gather_scatter<float, 8>();
  gather_scatter<u8, 16>();
  gather_scatter<double, 3>();
  gather_scatter<long long, 12>();
}

TEST_CASE("simd: chunk and cat") {
  chunk_and_cat<u8>();
  chunk_and_cat<short>();
  chunk_and_cat<int>();
  chunk_and_cat<float>();
  chunk_and_cat<double>();
}

TEST_CASE("simd: vectors and masks are random-access ranges") {
  iterators<int>();
  iterators<float>();
  iterators<u8>();
}

TEST_CASE("simd: generator calls happen once per lane, in order") {
  std::vector<int> calls;
  const sd::vec<int, 8> v([&](auto i) {
    static_assert(
        std::is_same_v<decltype(i),
                       std::integral_constant<int, decltype(i)::value>>);
    calls.push_back(i);
    return i * 2;
  });
  CHECK(calls.size() == 8u);
  for (int i = 0; i < 8; ++i)
    CHECK(calls[size_t(i)] == i);
  CHECK(v[7] == 14);
}

TEST_CASE("simd: iota") {
  check_lanes("iota int", sd::iota<sd::vec<int, 12>>, [](int i) { return i; });
  check_lanes("iota float", sd::iota<sd::vec<float>>,
              [](int i) { return float(i); });
  check_lanes("iota u8", sd::iota<sd::vec<u8, 64>>,
              [](int i) { return u8(i); });
  static_assert(sd::iota<int> == 0);
}

TEST_CASE("simd: type relationships match std::simd") {
  static_assert(std::is_same_v<sd::vec<float>, sd::basic_vec<float>>);
  static_assert(std::is_same_v<sd::vec<int>::mask_type, sd::mask<int>>);
  static_assert(std::is_same_v<sd::vec<float, 8>::mask_type,
                               sd::basic_mask<4, sd::vec<float, 8>::abi_type>>);
  static_assert(sd::vec<double, 5>::size() == 5);
  static_assert(sd::mask<u8, 64>::size() == 64);
  static_assert(std::is_same_v<sd::rebind_t<double, sd::vec<float, 8>>,
                               sd::vec<double, 8>>);
  static_assert(std::is_same_v<sd::rebind_t<double, sd::mask<float, 8>>,
                               sd::mask<double, 8>>);
  static_assert(
      std::is_same_v<sd::resize_t<3, sd::vec<short, 8>>, sd::vec<short, 3>>);
  static_assert(
      std::is_same_v<sd::resize_t<3, sd::mask<short, 8>>, sd::mask<short, 3>>);
  static_assert(sd::alignment_v<sd::vec<float, 4>> == 16);
  static_assert(sd::alignment_v<sd::vec<float, 4>, double> == 32);
  static_assert(sd::alignment_v<sd::vec<u8, 64>> == 64);

  static_assert(std::is_trivially_copyable_v<sd::vec<float>>);
  static_assert(std::is_trivially_copyable_v<sd::vec<double, 7>>);
  static_assert(std::is_trivially_copyable_v<sd::mask<int, 12>>);
  static_assert(std::is_trivially_default_constructible_v<sd::vec<int>>);
  static_assert(alignof(std::pair<sd::vec<float>, sd::vec<float>>) ==
                alignof(sd::vec<float>));
  static_assert(alignof(std::array<sd::vec<double, 8>, 2>) ==
                alignof(sd::vec<double, 8>));
  static_assert(alignof(std::tuple<sd::mask<int>, sd::vec<u8, 64>>) ==
                std::max(alignof(sd::mask<int>), alignof(sd::vec<u8, 64>)));
  static_assert(alignof(sd::vec<float>) >=
                    sizeof(float) * sd::vec<float>::size() ||
                sd::vec<float>::size() == 1);
  static_assert(sizeof(sd::vec<float, 4>) == 16);
  static_assert(sizeof(sd::vec<int, 8>) == 32);

  static_assert(!std::is_default_constructible_v<sd::basic_vec<bool>>);
  static_assert(!std::is_default_constructible_v<sd::basic_vec<long double>>);
  static_assert(!std::is_default_constructible_v<sd::basic_vec<int *>>);
  static_assert(!std::is_default_constructible_v<
                sd::basic_mask<3, sd::vec<int>::abi_type>>);

  // Broadcasts are implicit only when every value of the source fits.
  static_assert(std::is_convertible_v<float, sd::vec<float>>);
  static_assert(std::is_convertible_v<short, sd::vec<int>>);
  static_assert(std::is_convertible_v<int, sd::vec<double>>);
  static_assert(!std::is_convertible_v<int, sd::vec<float>>);
  static_assert(!std::is_convertible_v<double, sd::vec<float>>);
  static_assert(!std::is_convertible_v<unsigned, sd::vec<int>>);
  static_assert(!std::is_convertible_v<int, sd::vec<unsigned>>);
  static_assert(
      std::is_convertible_v<std::integral_constant<int, 3>, sd::vec<float>>);
  static_assert(
      std::is_convertible_v<std::integral_constant<int, -3>, sd::vec<short>>);
  static_assert(!std::is_convertible_v<std::integral_constant<int, -3>,
                                       sd::vec<unsigned>>);
  static_assert(
      !std::is_convertible_v<std::integral_constant<int, (1 << 24) + 1>,
                             sd::vec<float>>);

  // Element conversions: implicit when value-preserving and not narrowing
  // in rank.
  static_assert(std::is_convertible_v<sd::vec<float, 4>, sd::vec<double, 4>>);
  static_assert(!std::is_convertible_v<sd::vec<double, 4>, sd::vec<float, 4>>);
  static_assert(std::is_constructible_v<sd::vec<float, 4>, sd::vec<double, 4>>);
  static_assert(std::is_convertible_v<sd::vec<short, 4>, sd::vec<int, 4>>);
  static_assert(!std::is_convertible_v<sd::vec<int, 4>, sd::vec<float, 4>>);
  static_assert(
      !std::is_convertible_v<sd::vec<long long, 4>, sd::vec<long, 4>> ||
      sizeof(long) == sizeof(long long));
  static_assert(!std::is_constructible_v<sd::vec<float, 4>, sd::vec<float, 8>>);

  static_assert(!std::is_convertible_v<bool, sd::mask<int>>);
  static_assert(std::is_constructible_v<sd::mask<int>, bool>);
  static_assert(
      !std::is_convertible_v<sd::mask<float, 4>, sd::mask<double, 4>>);
  static_assert(
      std::is_constructible_v<sd::mask<double, 4>, sd::mask<float, 4>>);
  static_assert(std::is_convertible_v<sd::mask<float, 4>, sd::vec<int, 4>>);
  static_assert(!std::is_convertible_v<sd::mask<float, 4>, sd::vec<short, 4>>);

  static_assert(
      std::is_same_v<
          decltype(sd::flag_convert | sd::flag_aligned | sd::flag_convert),
          sd::flags<sd::detail::convert_flag, sd::detail::aligned_flag>>);
}

namespace {

constexpr int constexpr_checks() {
  using V = sd::vec<int, 8>;
  const V a([](int i) { return i - 3; });
  const V b(4);
  int failures = 0;
  const V sum = a + b * b - a / b;
  for (int i = 0; i < 8; ++i)
    failures += sum[i] != (i - 3) + 16 - (i - 3) / 4;
  failures += sd::reduce(a) != 4;
  failures += sd::reduce_min(a) != -3;
  failures += sd::reduce_max(a, a < 2) != 1;
  failures += sd::reduce_count(a > 0) != 4;
  failures += sd::reduce_min_index(a > 0) != 4;
  failures += !sd::all_of((a << 1) == a + a);
  failures += sd::select(a < 0, -a, a)[0] != 3;
  failures += sd::min(a, b)[7] != 4;
  failures += sd::permute(a, [](int i) { return 7 - i; })[0] != 4;
  const auto halves = sd::chunk<4>(a);
  failures += halves[1][0] != 1;
  failures += sd::cat(halves[1], halves[0])[4] != -3;
  int out[8] = {};
  sd::unchecked_store(a, out);
  failures += sd::unchecked_load<V>(out)[5] != 2;
  failures += sd::partial_load<V>(out, 3)[3] != 0;
  failures += sd::compress(a, a > 0)[0] != 1;
  failures += (+(a > 0))[7] != 1;
  failures += sd::mask<int, 8>(0x0fu).to_ullong() != 0x0f;

  const sd::vec<float, 4> f([](int i) { return float(i) * 0.5f; });
  failures += sd::reduce(f * 2.f) != 6.f;
  failures += sd::vec<double, 4>(f)[3] != 1.5;
  failures += sd::vec<int, 4>(f * 4.f)[3] != 6;
  failures += sd::reduce_max(f) != 1.5f;

  const sd::vec<u8, 16> bytes([](int i) { return u8(i * 16); });
  failures += sd::reduce(bytes, std::bit_or<>{}) != 0xf0;
  failures += sd::reduce_count(bytes >= u8(128)) != 8;
  failures += sd::iota<sd::vec<short, 6>>[5] != 5;
  return failures;
}

} // namespace

TEST_CASE("simd: vectors and masks work in constant expressions") {
  static_assert(constexpr_checks() == 0);
  CHECK(constexpr_checks() == 0);
}
