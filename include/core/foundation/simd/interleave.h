#pragma once

// Loads and stores of interleaved elements (stereo frames, RGBA pixels, xyz
// positions) as one vector per component. An extension; std::simd has no
// counterpart.

#include "core/foundation/simd/simd.h"

#include <array>
#include <cassert>
#include <cstring>
#include <iterator>
#include <memory>
#include <type_traits>

namespace nx::simd {

namespace detail {

template <class V>
using storage_of =
    std::remove_cvref_t<decltype(access::data(std::declval<const V &>()))>;

template <class V, int K>
concept interleavable = simd_vec_type<V> && K >= 1 &&
                        vectorizable_arithmetic<typename V::value_type>;

template <class V>
constexpr storage_of<V> load_part(const typename V::value_type *p) {
  return access::data(simd::unchecked_load<V>(p, V::size()));
}

template <class V>
constexpr void split2(const storage_of<V> &a, const storage_of<V> &b,
                      storage_of<V> &even, storage_of<V> &odd) {
  vops<V>::deinterleave2(a, b, even, odd);
}
template <class V>
constexpr void merge2(const storage_of<V> &a, const storage_of<V> &b,
                      storage_of<V> &lo, storage_of<V> &hi) {
  vops<V>::interleave2(a, b, lo, hi);
}

// Four components are two rounds of two: the first round pairs components
// 0 with 2 and 1 with 3, the second separates them.
template <int K, class V>
constexpr std::array<V, K> split_interleaved(const typename V::value_type *p) {
  constexpr int N = V::size();
  using S = storage_of<V>;
  std::array<V, K> parts{};
  if constexpr (K == 1) {
    parts[0] = simd::unchecked_load<V>(p, N);
  } else if constexpr (K == 2) {
    S even{}, odd{};
    split2<V>(load_part<V>(p), load_part<V>(p + N), even, odd);
    parts = {access::make<V>(even), access::make<V>(odd)};
  } else if constexpr (K == 4) {
    S e1{}, o1{}, e2{}, o2{}, c0{}, c1{}, c2{}, c3{};
    split2<V>(load_part<V>(p), load_part<V>(p + N), e1, o1);
    split2<V>(load_part<V>(p + 2 * N), load_part<V>(p + 3 * N), e2, o2);
    split2<V>(e1, e2, c0, c2);
    split2<V>(o1, o2, c1, c3);
    parts = {access::make<V>(c0), access::make<V>(c1), access::make<V>(c2),
             access::make<V>(c3)};
  } else {
    for (int k = 0; k < K; ++k)
      parts[size_t(k)] = V([&](int i) { return p[i * K + k]; });
  }
  return parts;
}

template <class V, size_t K>
constexpr void merge_interleaved(const std::array<V, K> &parts,
                                 typename V::value_type *p) {
  constexpr int N = V::size();
  using S = storage_of<V>;
  const auto store = [&](const S &s, int at) {
    simd::unchecked_store(access::make<V>(s), p + at, N);
  };
  if constexpr (K == 1) {
    simd::unchecked_store(parts[0], p, N);
  } else if constexpr (K == 2) {
    S lo{}, hi{};
    merge2<V>(access::data(parts[0]), access::data(parts[1]), lo, hi);
    store(lo, 0);
    store(hi, N);
  } else if constexpr (K == 4) {
    S e1{}, e2{}, o1{}, o2{}, r0{}, r1{}, r2{}, r3{};
    merge2<V>(access::data(parts[0]), access::data(parts[2]), e1, e2);
    merge2<V>(access::data(parts[1]), access::data(parts[3]), o1, o2);
    merge2<V>(e1, o1, r0, r1);
    merge2<V>(e2, o2, r2, r3);
    store(r0, 0);
    store(r1, N);
    store(r2, 2 * N);
    store(r3, 3 * N);
  } else {
    for (int i = 0; i < N; ++i)
      for (size_t k = 0; k < K; ++k)
        p[size_t(i) * K + k] = parts[k][i];
  }
}

} // namespace detail

// Lane i of part k is first[i * K + k]. The unchecked forms touch exactly
// K * V::size() elements; the partial forms touch the first n, loading zeros
// past them and storing nothing past them.
template <class V, int K, std::contiguous_iterator I>
  requires detail::interleavable<V, K>
constexpr std::array<V, K>
unchecked_load_interleaved(I first, std::iter_difference_t<I> n) {
  static_assert(std::is_same_v<std::iter_value_t<I>, typename V::value_type>,
                "interleaved loads do not convert");
  assert(n >= K * V::size() && "the range holds every lane of every part");
  (void)n;
  return detail::split_interleaved<K, V>(std::to_address(first));
}

template <class V, int K, std::contiguous_iterator I>
  requires detail::interleavable<V, K>
constexpr std::array<V, K>
partial_load_interleaved(I first, std::iter_difference_t<I> n) {
  static_assert(std::is_same_v<std::iter_value_t<I>, typename V::value_type>,
                "interleaved loads do not convert");
  using T = typename V::value_type;
  constexpr int total = K * V::size();
  if (n >= total)
    return detail::split_interleaved<K, V>(std::to_address(first));
  T buffer[total] = {};
  const T *source = std::to_address(first);
  if (std::is_constant_evaluated()) {
    for (std::iter_difference_t<I> i = 0; i < n; ++i)
      buffer[i] = source[i];
  } else if (n > 0) {
    std::memcpy(buffer, source, sizeof(T) * size_t(n));
  }
  return detail::split_interleaved<K, V>(buffer);
}

template <class V, size_t K, std::contiguous_iterator I>
  requires detail::interleavable<V, int(K)>
constexpr void unchecked_store_interleaved(const std::array<V, K> &parts,
                                           I first,
                                           std::iter_difference_t<I> n) {
  static_assert(std::is_same_v<std::iter_value_t<I>, typename V::value_type>,
                "interleaved stores do not convert");
  assert(n >= int(K) * V::size() && "the range holds every lane of every part");
  (void)n;
  detail::merge_interleaved(parts, std::to_address(first));
}

template <class V, size_t K, std::contiguous_iterator I>
  requires detail::interleavable<V, int(K)>
constexpr void partial_store_interleaved(const std::array<V, K> &parts, I first,
                                         std::iter_difference_t<I> n) {
  static_assert(std::is_same_v<std::iter_value_t<I>, typename V::value_type>,
                "interleaved stores do not convert");
  using T = typename V::value_type;
  constexpr int total = int(K) * V::size();
  if (n >= total)
    return detail::merge_interleaved(parts, std::to_address(first));
  T buffer[total] = {};
  detail::merge_interleaved(parts, buffer);
  T *target = std::to_address(first);
  if (std::is_constant_evaluated()) {
    for (std::iter_difference_t<I> i = 0; i < n; ++i)
      target[i] = buffer[i];
  } else if (n > 0) {
    std::memcpy(target, buffer, sizeof(T) * size_t(n));
  }
}

} // namespace nx::simd
