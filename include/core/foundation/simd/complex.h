#pragma once

#include "core/foundation/simd/algorithm.h"

#include <complex>

namespace nx::simd {

namespace detail {

template <class V>
using complex_real_t = rebind_t<typename V::value_type::value_type, V>;

template <class R, class V, class F> R map_complex(const V &v, F f) {
  return vec_generate<R>([&](int i) { return f(v[i]); });
}

} // namespace detail

template <detail::simd_complex V>
constexpr detail::complex_real_t<V> real(const V &v) noexcept {
  return v.real();
}
template <detail::simd_complex V>
constexpr detail::complex_real_t<V> imag(const V &v) noexcept {
  return v.imag();
}
template <detail::simd_complex V>
constexpr detail::complex_real_t<V> norm(const V &v) noexcept {
  const auto re = v.real();
  const auto im = v.imag();
  return re * re + im * im;
}
template <detail::simd_complex V> constexpr V conj(const V &v) noexcept {
  return V(v.real(), -v.imag());
}

template <detail::simd_complex V> detail::complex_real_t<V> abs(const V &v) {
  return detail::map_complex<detail::complex_real_t<V>>(
      v, [](auto c) { return std::abs(c); });
}
template <detail::simd_complex V> detail::complex_real_t<V> arg(const V &v) {
  return detail::map_complex<detail::complex_real_t<V>>(
      v, [](auto c) { return std::arg(c); });
}

#define NX_SIMD_COMPLEX_UNARY(name)                                            \
  template <detail::simd_complex V> V name(const V &v) {                       \
    return detail::map_complex<V>(v, [](auto c) { return std::name(c); });     \
  }
NX_SIMD_COMPLEX_UNARY(proj)
NX_SIMD_COMPLEX_UNARY(exp)
NX_SIMD_COMPLEX_UNARY(log)
NX_SIMD_COMPLEX_UNARY(log10)
NX_SIMD_COMPLEX_UNARY(sqrt)
NX_SIMD_COMPLEX_UNARY(sin)
NX_SIMD_COMPLEX_UNARY(asin)
NX_SIMD_COMPLEX_UNARY(cos)
NX_SIMD_COMPLEX_UNARY(acos)
NX_SIMD_COMPLEX_UNARY(tan)
NX_SIMD_COMPLEX_UNARY(atan)
NX_SIMD_COMPLEX_UNARY(sinh)
NX_SIMD_COMPLEX_UNARY(asinh)
NX_SIMD_COMPLEX_UNARY(cosh)
NX_SIMD_COMPLEX_UNARY(acosh)
NX_SIMD_COMPLEX_UNARY(tanh)
NX_SIMD_COMPLEX_UNARY(atanh)
#undef NX_SIMD_COMPLEX_UNARY

template <detail::simd_floating_point V>
rebind_t<std::complex<typename V::value_type>, V> polar(const V &x,
                                                        const V &y = {}) {
  return detail::vec_generate<
      rebind_t<std::complex<typename V::value_type>, V>>(
      [&](int i) { return std::polar(x[i], y[i]); });
}

template <detail::simd_complex V> V pow(const V &x, const V &y) {
  return detail::vec_generate<V>([&](int i) { return std::pow(x[i], y[i]); });
}

} // namespace nx::simd
