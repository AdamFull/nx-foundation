#include "simd/simd_test_util.h"

#include "core/foundation/simd/interleave.h"

#include <array>
#include <cstdint>

using namespace simd_test;

namespace {

template <class T, int N, int K> int round_trip() {
  using V = sd::vec<T, N>;
  int bad = 0;
  T data[K * N + 3];
  for (int i = 0; i < K * N + 3; ++i)
    data[i] = static_cast<T>(i * 7919 + 13); // sets high bits of narrow types

  const auto parts = sd::unchecked_load_interleaved<V, K>(data, K * N);
  for (int k = 0; k < K; ++k)
    for (int i = 0; i < N; ++i)
      bad += !same(parts[size_t(k)][i], data[i * K + k]);

  T out[K * N + 3];
  for (auto &x : out)
    x = T(99);
  sd::unchecked_store_interleaved(parts, out, K * N);
  for (int i = 0; i < K * N + 3; ++i)
    bad += !same(out[i], i < K * N ? data[i] : T(99));

  for (int n = 0; n <= K * N; ++n) {
    const auto p = sd::partial_load_interleaved<V, K>(data, n);
    for (int k = 0; k < K; ++k)
      for (int i = 0; i < N; ++i)
        bad += !same(p[size_t(k)][i], i * K + k < n ? data[i * K + k] : T());
    for (auto &x : out)
      x = T(99);
    sd::partial_store_interleaved(parts, out, n);
    for (int i = 0; i < K * N + 3; ++i)
      bad += !same(out[i], i < n ? data[i] : T(99));
  }
  return bad;
}

template <class T> int every_shape() {
  constexpr int W = sd::vec<T>::size();
  int bad = round_trip<T, W, 1>() + round_trip<T, W, 2>() +
            round_trip<T, W, 3>() + round_trip<T, W, 4>() +
            round_trip<T, W, 5>() + round_trip<T, 1, 2>() +
            round_trip<T, 3, 2>() + round_trip<T, 3, 4>();
  if constexpr (2 * W <= 64)
    bad += round_trip<T, 2 * W, 2>() + round_trip<T, 2 * W, 4>();
  if constexpr (4 * W <= 64)
    bad += round_trip<T, 4 * W, 2>();
  return bad;
}

template <class T> void check_one() {
  const int bad = every_shape<T>();
  CHECK(bad == 0);
}
template <class... T> void check_every() { (check_one<T>(), ...); }

constexpr int constexpr_round_trip() {
  std::array<int, 16> data{}, out{};
  for (int i = 0; i < 16; ++i)
    data[size_t(i)] = i * 3;
  const auto parts =
      sd::unchecked_load_interleaved<sd::vec<int, 4>, 4>(data.data(), 16);
  sd::unchecked_store_interleaved(parts, out.data(), 16);
  const auto two =
      sd::partial_load_interleaved<sd::vec<int, 4>, 2>(data.data(), 5);
  return parts[2][3] + (out == data ? 1000 : 0) + two[1][1] * 10000 +
         two[1][2] * 100000;
}

} // namespace

TEST_CASE("simd interleave: every element type, width and component count") {
  check_every<std::int8_t, std::uint8_t, char, std::int16_t, std::uint16_t,
              std::int32_t, std::uint32_t, std::int64_t, std::uint64_t, float,
              double>();
}

TEST_CASE("simd interleave: runs in constant expressions") {
  // parts[2][3] is data[3 * 4 + 2] = 42; two[1][1] is data[3] = 9; lane 2 of
  // the second part is past the five elements, so zero.
  static_assert(constexpr_round_trip() == 42 + 1000 + 90000);
  const int value = constexpr_round_trip();
  CHECK(value == 42 + 1000 + 90000);
}
