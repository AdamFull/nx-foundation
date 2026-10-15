#pragma once

#include "core/foundation/simd/algorithm.h"

#include <bit>

namespace nx::simd {

namespace detail {

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

template <class V>
concept unsigned_vec =
    simd_vec_type<V> && standard_unsigned<typename V::value_type>;

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

template <class R, class V, class F> constexpr R map_bits(const V &v, F f) {
  return vec_generate<R>([&](int i) { return f(v[i]); });
}

} // namespace detail

template <detail::simd_vec_type V>
  requires std::integral<typename V::value_type>
constexpr V byteswap(const V &v) noexcept {
  return detail::map_bits<V>(v, [](auto x) { return detail::byteswap(x); });
}

template <detail::simd_integral V>
  requires detail::standard_unsigned<typename V::value_type>
constexpr V bit_reverse(const V &v) noexcept {
  return detail::map_bits<V>(v, [](auto x) { return detail::bit_reverse(x); });
}

template <detail::unsigned_vec V> constexpr V bit_ceil(const V &v) {
  return detail::map_bits<V>(v, [](auto x) {
    assert(x <= (std::numeric_limits<decltype(x)>::max() >> 1) + 1);
    return std::bit_ceil(x);
  });
}
template <detail::unsigned_vec V> constexpr V bit_floor(const V &v) noexcept {
  return detail::map_bits<V>(v, [](auto x) { return std::bit_floor(x); });
}
template <detail::unsigned_vec V>
constexpr typename V::mask_type has_single_bit(const V &v) noexcept {
  return detail::make_mask<typename V::mask_type>(
      [&](int i) { return std::has_single_bit(v[i]); });
}

template <detail::simd_integral VX, detail::simd_integral VS>
  requires detail::standard_integer<typename VX::value_type> &&
           detail::standard_integer<typename VS::value_type> &&
           (VX::size() == VS::size()) &&
           (sizeof(typename VX::value_type) == sizeof(typename VS::value_type))
constexpr VX shl(const VX &x, const VS &s) noexcept {
  return detail::vec_generate<VX>(
      [&](int i) { return detail::shl(x[i], s[i]); });
}
template <detail::simd_integral VX, detail::simd_integral VS>
  requires detail::standard_integer<typename VX::value_type> &&
           detail::standard_integer<typename VS::value_type> &&
           (VX::size() == VS::size()) &&
           (sizeof(typename VX::value_type) == sizeof(typename VS::value_type))
constexpr VX shr(const VX &x, const VS &s) noexcept {
  return detail::vec_generate<VX>(
      [&](int i) { return detail::shr(x[i], s[i]); });
}
template <detail::simd_integral V, class S>
  requires detail::standard_integer<typename V::value_type> &&
           detail::standard_integer<S>
constexpr V shl(const V &x, S s) noexcept {
  return detail::map_bits<V>(x, [s](auto e) { return detail::shl(e, s); });
}
template <detail::simd_integral V, class S>
  requires detail::standard_integer<typename V::value_type> &&
           detail::standard_integer<S>
constexpr V shr(const V &x, S s) noexcept {
  return detail::map_bits<V>(x, [s](auto e) { return detail::shr(e, s); });
}

template <detail::simd_vec_type V0, detail::simd_vec_type V1>
  requires detail::standard_unsigned<typename V0::value_type> &&
           std::integral<typename V1::value_type> &&
           (V0::size() == V1::size()) &&
           (sizeof(typename V0::value_type) == sizeof(typename V1::value_type))
constexpr V0 rotl(const V0 &v0, const V1 &v1) noexcept {
  return detail::vec_generate<V0>(
      [&](int i) { return std::rotl(v0[i], static_cast<int>(v1[i])); });
}
template <detail::simd_vec_type V0, detail::simd_vec_type V1>
  requires detail::standard_unsigned<typename V0::value_type> &&
           std::integral<typename V1::value_type> &&
           (V0::size() == V1::size()) &&
           (sizeof(typename V0::value_type) == sizeof(typename V1::value_type))
constexpr V0 rotr(const V0 &v0, const V1 &v1) noexcept {
  return detail::vec_generate<V0>(
      [&](int i) { return std::rotr(v0[i], static_cast<int>(v1[i])); });
}
template <detail::unsigned_vec V> constexpr V rotl(const V &v, int s) noexcept {
  return detail::map_bits<V>(v, [s](auto x) { return std::rotl(x, s); });
}
template <detail::unsigned_vec V> constexpr V rotr(const V &v, int s) noexcept {
  return detail::map_bits<V>(v, [s](auto x) { return std::rotr(x, s); });
}

template <detail::simd_integral V0, detail::simd_integral V1>
  requires detail::standard_unsigned<typename V0::value_type> &&
           std::integral<typename V1::value_type> &&
           (V0::size() == V1::size()) &&
           (sizeof(typename V0::value_type) == sizeof(typename V1::value_type))
constexpr V0 bit_repeat(const V0 &v0, const V1 &v1) {
  return detail::vec_generate<V0>([&](int i) {
    return detail::bit_repeat(v0[i], static_cast<int>(v1[i]));
  });
}
template <detail::simd_integral V>
  requires detail::standard_unsigned<typename V::value_type>
constexpr V bit_repeat(const V &v, int l) {
  return detail::map_bits<V>(v,
                             [l](auto x) { return detail::bit_repeat(x, l); });
}

#define NX_SIMD_BIT_COUNT(name)                                                \
  template <detail::unsigned_vec V>                                            \
  constexpr rebind_t<std::make_signed_t<typename V::value_type>, V> name(      \
      const V &v) noexcept {                                                   \
    using R = rebind_t<std::make_signed_t<typename V::value_type>, V>;         \
    return detail::map_bits<R>(v, [](auto x) {                                 \
      return static_cast<typename R::value_type>(std::name(x));                \
    });                                                                        \
  }
NX_SIMD_BIT_COUNT(bit_width)
NX_SIMD_BIT_COUNT(countl_zero)
NX_SIMD_BIT_COUNT(countl_one)
NX_SIMD_BIT_COUNT(countr_zero)
NX_SIMD_BIT_COUNT(countr_one)
NX_SIMD_BIT_COUNT(popcount)
#undef NX_SIMD_BIT_COUNT

template <detail::simd_integral V>
  requires detail::standard_unsigned<typename V::value_type>
constexpr V bit_compress(const V &v, const V &m) noexcept {
  return detail::vec_generate<V>(
      [&](int i) { return detail::bit_compress(v[i], m[i]); });
}
template <detail::simd_integral V>
  requires detail::standard_unsigned<typename V::value_type>
constexpr V bit_compress(const V &v, typename V::value_type m) noexcept {
  return detail::map_bits<V>(
      v, [m](auto x) { return detail::bit_compress(x, m); });
}
template <detail::simd_integral V>
  requires detail::standard_unsigned<typename V::value_type>
constexpr V bit_expand(const V &v, const V &m) noexcept {
  return detail::vec_generate<V>(
      [&](int i) { return detail::bit_expand(v[i], m[i]); });
}
template <detail::simd_integral V>
  requires detail::standard_unsigned<typename V::value_type>
constexpr V bit_expand(const V &v, typename V::value_type m) noexcept {
  return detail::map_bits<V>(v,
                             [m](auto x) { return detail::bit_expand(x, m); });
}

} // namespace nx::simd
