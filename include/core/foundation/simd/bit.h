#pragma once

#include "core/foundation/simd/algorithm.h"
#include "core/foundation/simd/bit_scalar.h"

#include <bit>

namespace nx::simd {

namespace detail {

template <class V>
concept unsigned_vec =
    simd_vec_type<V> && standard_unsigned<typename V::value_type>;

template <class R, class V, class F> constexpr R map_bits(const V &v, F f) {
  return vec_generate<R>([&](int i) { return f(v[i]); });
}

template <class V> constexpr V clz(const V &v) {
  return access::make<V>(vops<V>::countl_zero(access::data(v)));
}
template <class V> constexpr V pop(const V &v) {
  return access::make<V>(vops<V>::popcount(access::data(v)));
}
template <class V> constexpr V lanes_of(int x) {
  return V(static_cast<typename V::value_type>(x));
}
template <unsigned_vec V>
using bit_count_t = rebind_t<std::make_signed_t<typename V::value_type>, V>;

template <class V>
inline constexpr int digits_of =
    std::numeric_limits<typename V::value_type>::digits;

} // namespace detail

template <detail::simd_vec_type V>
  requires std::integral<typename V::value_type>
constexpr V byteswap(const V &v) noexcept {
  return detail::access::make<V>(
      detail::vops<V>::byteswap(detail::access::data(v)));
}

template <detail::simd_integral V>
  requires detail::standard_unsigned<typename V::value_type>
constexpr V bit_reverse(const V &v) noexcept {
  return detail::access::make<V>(
      detail::vops<V>::bit_reverse(detail::access::data(v)));
}

template <detail::unsigned_vec V> constexpr V bit_ceil(const V &v) {
  using T = typename V::value_type;
  constexpr int w = detail::digits_of<V>;
  assert(all_of(v <= V(T((std::numeric_limits<T>::max() >> 1) + 1))));
  const V shift = (detail::lanes_of<V>(w) - detail::clz(v - V(T(1)))) &
                  detail::lanes_of<V>(w - 1);
  return select(v <= V(T(1)), V(T(1)), V(T(1)) << shift);
}
template <detail::unsigned_vec V> constexpr V bit_floor(const V &v) noexcept {
  using T = typename V::value_type;
  constexpr int w = detail::digits_of<V>;
  const V shift = (detail::lanes_of<V>(w - 1) - detail::clz(v)) &
                  detail::lanes_of<V>(w - 1);
  return select(v == V(T(0)), V(T(0)), V(T(1)) << shift);
}
template <detail::unsigned_vec V>
constexpr typename V::mask_type has_single_bit(const V &v) noexcept {
  return detail::pop(v) == detail::lanes_of<V>(1);
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
  constexpr int w = detail::digits_of<V0>;
  const V0 s = V0(v1) & detail::lanes_of<V0>(w - 1);
  return (v0 << s) |
         (v0 >> ((detail::lanes_of<V0>(w) - s) & detail::lanes_of<V0>(w - 1)));
}
template <detail::simd_vec_type V0, detail::simd_vec_type V1>
  requires detail::standard_unsigned<typename V0::value_type> &&
           std::integral<typename V1::value_type> &&
           (V0::size() == V1::size()) &&
           (sizeof(typename V0::value_type) == sizeof(typename V1::value_type))
constexpr V0 rotr(const V0 &v0, const V1 &v1) noexcept {
  constexpr int w = detail::digits_of<V0>;
  const V0 s = V0(v1) & detail::lanes_of<V0>(w - 1);
  return (v0 >> s) |
         (v0 << ((detail::lanes_of<V0>(w) - s) & detail::lanes_of<V0>(w - 1)));
}
template <detail::unsigned_vec V> constexpr V rotl(const V &v, int s) noexcept {
  constexpr int w = detail::digits_of<V>;
  s &= w - 1;
  return (v << s) | (v >> ((w - s) & (w - 1)));
}
template <detail::unsigned_vec V> constexpr V rotr(const V &v, int s) noexcept {
  constexpr int w = detail::digits_of<V>;
  s &= w - 1;
  return (v >> s) | (v << ((w - s) & (w - 1)));
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

template <detail::unsigned_vec V>
constexpr detail::bit_count_t<V> popcount(const V &v) noexcept {
  return detail::bit_count_t<V>(detail::pop(v));
}
template <detail::unsigned_vec V>
constexpr detail::bit_count_t<V> countl_zero(const V &v) noexcept {
  return detail::bit_count_t<V>(detail::clz(v));
}
template <detail::unsigned_vec V>
constexpr detail::bit_count_t<V> countl_one(const V &v) noexcept {
  return detail::bit_count_t<V>(detail::clz(~v));
}
template <detail::unsigned_vec V>
constexpr detail::bit_count_t<V> countr_zero(const V &v) noexcept {
  return detail::bit_count_t<V>(detail::pop(~v & (v - detail::lanes_of<V>(1))));
}
template <detail::unsigned_vec V>
constexpr detail::bit_count_t<V> countr_one(const V &v) noexcept {
  return detail::bit_count_t<V>(detail::pop(v & ~(v + detail::lanes_of<V>(1))));
}
template <detail::unsigned_vec V>
constexpr detail::bit_count_t<V> bit_width(const V &v) noexcept {
  return detail::bit_count_t<V>(detail::lanes_of<V>(detail::digits_of<V>) -
                                detail::clz(v));
}

template <detail::simd_integral V>
  requires detail::standard_unsigned<typename V::value_type>
constexpr V bit_compress(const V &v, const V &m) noexcept {
  return detail::access::make<V>(detail::vops<V>::bit_compress(
      detail::access::data(v), detail::access::data(m)));
}
template <detail::simd_integral V>
  requires detail::standard_unsigned<typename V::value_type>
constexpr V bit_compress(const V &v, typename V::value_type m) noexcept {
  return bit_compress(v, V(m));
}
template <detail::simd_integral V>
  requires detail::standard_unsigned<typename V::value_type>
constexpr V bit_expand(const V &v, const V &m) noexcept {
  return detail::access::make<V>(detail::vops<V>::bit_expand(
      detail::access::data(v), detail::access::data(m)));
}
template <detail::simd_integral V>
  requires detail::standard_unsigned<typename V::value_type>
constexpr V bit_expand(const V &v, typename V::value_type m) noexcept {
  return bit_expand(v, V(m));
}

} // namespace nx::simd
