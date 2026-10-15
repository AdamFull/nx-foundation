#pragma once

#include "core/foundation/simd/detail.h"

#include <bit>
#include <cassert>

namespace nx::simd::detail {

template <class T>
concept standard_unsigned =
    std::is_same_v<T, unsigned char> || std::is_same_v<T, unsigned short> ||
    std::is_same_v<T, unsigned int> || std::is_same_v<T, unsigned long> ||
    std::is_same_v<T, unsigned long long>;

template <class T>
concept standard_integer =
    standard_unsigned<T> || std::is_same_v<T, signed char> ||
    std::is_same_v<T, short> || std::is_same_v<T, int> ||
    std::is_same_v<T, long> || std::is_same_v<T, long long>;

// The scalar <bit> functions C++20 does not have yet.

template <std::integral T> constexpr T byteswap(T v) noexcept {
  if constexpr (sizeof(T) == 1) {
    return v;
  } else {
    using U = std::make_unsigned_t<T>;
    U u = static_cast<U>(v);
    U r = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
      r = static_cast<U>((r << 8) | (u & 0xffu));
      u = static_cast<U>(u >> 8);
    }
    return static_cast<T>(r);
  }
}

template <standard_unsigned T> constexpr T bit_reverse(T x) noexcept {
  T r = 0;
  for (int i = 0; i < std::numeric_limits<T>::digits; ++i) {
    r = static_cast<T>((r << 1) | (x & 1u));
    x = static_cast<T>(x >> 1);
  }
  return r;
}

template <standard_unsigned T> constexpr T bit_repeat(T x, int l) {
  assert(l > 0);
  constexpr int w = std::numeric_limits<T>::digits;
  if (l >= w)
    return x;
  const T pattern = static_cast<T>(x & ((T(1) << l) - 1u));
  T r = 0;
  for (int i = 0; i < w; i += l)
    r = static_cast<T>(r | (pattern << i));
  return r;
}

template <standard_unsigned T> constexpr T bit_compress(T x, T m) noexcept {
  T r = 0;
  int k = 0;
  for (int i = 0; i < std::numeric_limits<T>::digits; ++i)
    if ((m >> i) & 1u)
      r = static_cast<T>(r | (((x >> i) & 1u) << k++));
  return r;
}

template <standard_unsigned T> constexpr T bit_expand(T x, T m) noexcept {
  T r = 0;
  int k = 0;
  for (int i = 0; i < std::numeric_limits<T>::digits; ++i)
    if ((m >> i) & 1u)
      r = static_cast<T>(r | (((x >> k++) & 1u) << i));
  return r;
}

template <class T, class S> constexpr int clamped_shift(S s) noexcept {
  constexpr int w = std::numeric_limits<std::make_unsigned_t<T>>::digits;
  if (std::cmp_greater(s, w))
    return w;
  if (std::cmp_less(s, -w))
    return -w;
  return static_cast<int>(s);
}

// x * 2^c rounded towards negative infinity, reduced modulo 2^width.
template <class T> constexpr T shift_by(T x, int c) noexcept {
  using U = std::make_unsigned_t<T>;
  constexpr int w = std::numeric_limits<U>::digits;
  if (c >= w)
    return T(0);
  if (c >= 0)
    return static_cast<T>(static_cast<U>(static_cast<U>(x) << c));
  if (c <= -w)
    return x < T(0) ? T(-1) : T(0);
  return static_cast<T>(x >> -c);
}

template <standard_integer T, standard_integer S>
constexpr T shl(T x, S s) noexcept {
  return shift_by(x, clamped_shift<T>(s));
}
template <standard_integer T, standard_integer S>
constexpr T shr(T x, S s) noexcept {
  return shift_by(x, -clamped_shift<T>(s));
}

} // namespace nx::simd::detail
