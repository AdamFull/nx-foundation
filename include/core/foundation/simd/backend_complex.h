#pragma once

#include "core/foundation/simd/backend_generic.h"

namespace nx::simd::detail {

template <class F, class Abi> struct complex_storage {
  vec_storage<F, deduce_abi_t<F, Abi::template size<std::complex<F>>>> re, im;
};

// Complex lanes keep their real and imaginary parts in two real vectors, so
// arithmetic runs on whole registers without shuffling pairs apart.
template <class F, class Abi>
struct vec_ops<std::complex<F>, Abi>
    : vec_ops_base<vec_ops<std::complex<F>, Abi>, std::complex<F>,
                   Abi::template size<std::complex<F>>, complex_storage<F, Abi>,
                   mask_ops<sizeof(std::complex<F>), Abi>> {
  using T = std::complex<F>;
  static constexpr int N = Abi::template size<T>;
  using RA = deduce_abi_t<F, N>;
  using R = vec_ops<F, RA>;
  using RM = mask_ops<sizeof(F), RA>;
  using M = mask_ops<sizeof(T), Abi>;
  using S = complex_storage<F, Abi>;
  using MS = typename M::storage;
  using base = vec_ops_base<vec_ops, T, N, S, M>;

  static constexpr S broadcast(T x) {
    return {R::broadcast(x.real()), R::broadcast(x.imag())};
  }
  static constexpr T get(const S &a, int i) {
    return T(R::get(a.re, i), R::get(a.im, i));
  }
  template <class G> static constexpr S generate(G &&g) {
    struct values {
      T v[N];
    } c{};
    for (int i = 0; i < N; ++i)
      c.v[i] = static_cast<T>(g(i));
    return {R::generate([&](int i) { return c.v[i].real(); }),
            R::generate([&](int i) { return c.v[i].imag(); })};
  }

  static constexpr typename RM::storage real_mask(const MS &k) {
    if (std::is_constant_evaluated())
      return RM::generate([&](int i) { return M::get(k, i); });
    return RM::from_bits(M::to_bits(k));
  }
  static constexpr MS complex_mask(const typename RM::storage &k) {
    if (std::is_constant_evaluated())
      return M::generate([&](int i) { return RM::get(k, i); });
    return M::from_bits(RM::to_bits(k));
  }

  static constexpr S neg(const S &a) { return {R::neg(a.re), R::neg(a.im)}; }
  static constexpr S add(const S &a, const S &b) {
    return {R::add(a.re, b.re), R::add(a.im, b.im)};
  }
  static constexpr S sub(const S &a, const S &b) {
    return {R::sub(a.re, b.re), R::sub(a.im, b.im)};
  }
  static constexpr S mul(const S &a, const S &b) {
    return {R::sub(R::mul(a.re, b.re), R::mul(a.im, b.im)),
            R::add(R::mul(a.re, b.im), R::mul(a.im, b.re))};
  }
  static constexpr S div(const S &a, const S &b) {
    return base::zip(a, b, [](T x, T y) { return x / y; });
  }

  static constexpr MS eq(const S &a, const S &b) {
    return complex_mask(RM::land(R::eq(a.re, b.re), R::eq(a.im, b.im)));
  }
  static constexpr MS ne(const S &a, const S &b) {
    return complex_mask(RM::lor(R::ne(a.re, b.re), R::ne(a.im, b.im)));
  }
  static constexpr S select(const MS &k, const S &a, const S &b) {
    const auto rk = real_mask(k);
    return {R::select(rk, a.re, b.re), R::select(rk, a.im, b.im)};
  }

  template <class Op> static constexpr T reduce(const S &v, Op op) {
    if constexpr (std::is_same_v<Op, op_add>)
      return T(R::reduce(v.re, op), R::reduce(v.im, op));
    else
      return base::reduce(v, op);
  }
};

} // namespace nx::simd::detail
