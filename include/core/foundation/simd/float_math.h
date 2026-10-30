#pragma once

#include "core/foundation/simd/algorithm.h"

#include <bit>
#include <climits>
#include <limits>
#include <utility>

// Register-wide float exp, log, tanh and erf: Cody-Waite reduction, minimax
// polynomials and exponent arithmetic in plain +, -, * and /, so every backend
// rounds the same. The coefficients come from tools/fit_float_math.py.
#if defined(__FMA__) || defined(__ARM_FEATURE_FMA) || defined(NX_SIMD_X86_AVX2)
#define NX_SIMD_HAS_FMA 1
#endif

namespace nx::simd::detail::float_math {

inline constexpr float expm1_p[] = {5.000000000e-01f, 1.666666567e-01f,
                                    4.166648909e-02f, 8.333542384e-03f,
                                    1.393279177e-03f, 1.976560598e-04f};
inline constexpr float fast_exp_p[] = {5.000523329e-01f, 1.675451398e-01f,
                                       4.127341881e-02f};
inline constexpr float exp2_q[] = {6.931471825e-01f, 2.402264774e-01f,
                                   5.550359562e-02f, 9.618503042e-03f,
                                   1.339099719e-03f, 1.532784954e-04f};
inline constexpr float log_l[] = {
    3.333333135e-01f, -2.500083745e-01f, 2.000128329e-01f, -1.662277281e-01f,
    1.419987381e-01f, -1.316428930e-01f, 1.277806461e-01f, -7.644607127e-02f};
inline constexpr float tanh_t[] = {-3.333328068e-01f, 1.333141029e-01f,
                                   -5.373715237e-02f, 2.063091844e-02f,
                                   -5.695990287e-03f};
inline constexpr float erf_h[] = {
    1.283791661e-01f, -3.761262894e-01f, 1.128362268e-01f, -2.685535327e-02f,
    5.190869793e-03f, -8.024979616e-04f, 7.858472964e-05f};
// erfc(x) e^(x^2) in powers of x - 2.4.
inline constexpr float erfc_g[] = {
    2.184986621e-01f, -7.958561182e-02f, 2.749600448e-02f, -9.055160917e-03f,
    2.878715983e-03f, -8.951344062e-04f, 2.066103771e-04f, -9.688394493e-05f,
    4.097647616e-05f, 2.012441473e-05f,  9.904800208e-06f};
inline constexpr float fast_tanh_n[] = {9.999940991e-01f, 1.205685437e-01f,
                                        2.065520035e-03f, 3.018696361e-06f};
inline constexpr float fast_tanh_d[] = {1.000000000e+00f, 4.538640082e-01f,
                                        2.006170712e-02f, 1.202589410e-04f};
inline constexpr float fast_erf_n[] = {1.128357649e+00f, 1.667339504e-01f,
                                       4.121128470e-02f, 6.297886139e-04f};
inline constexpr float fast_erf_d[] = {1.000000000e+00f, 4.808435738e-01f,
                                       9.733493626e-02f, 8.322303183e-03f};

inline constexpr float log2e = 1.44269504088896341f;
// ln 2 = ln2_hi + ln2_lo, ln2_hi with 9 significant bits, so n * ln2_hi is
// exact for every exponent.
inline constexpr float ln2_hi = 0.693359375f;
inline constexpr float ln2_lo = -2.12194440e-4f;
// x + 1.5 * 2^23 - 1.5 * 2^23 rounds x to the nearest integer, ties to even.
inline constexpr float round_shift = 12582912.f;
inline constexpr float erf_one = 3.9192061f;
inline constexpr float tanh_one = 9.010914f;

template <class D> using ints = rebind_t<int, D>;

template <class D> NX_FORCE_INLINE ints<D> bits(const D &x) {
  return std::bit_cast<ints<D>>(x);
}
template <class D> NX_FORCE_INLINE D from_bits(const ints<D> &i) {
  return std::bit_cast<D>(i);
}

// The steps unroll in folds: MSVC keeps a loop over the coefficients.
template <class D, size_t K, size_t... k>
NX_FORCE_INLINE D horner(const D &x, const float (&c)[K],
                         std::index_sequence<k...>) {
  D p = c[K - 1];
  ((p = p * x + c[K - 2 - k]), ...);
  return p;
}
template <class D, size_t K>
NX_FORCE_INLINE D horner(const D &x, const float (&c)[K]) {
  return horner(x, c, std::make_index_sequence<K - 1>{});
}
template <class D, size_t K, size_t... k>
NX_FORCE_INLINE D horner_fast(const D &x, const float (&c)[K],
                              std::index_sequence<k...>) {
  D p = c[K - 1];
  ((p = access::make<D>(vops<D>::fma(access::data(p), access::data(x),
                                     access::data(D(c[K - 2 - k]))))),
   ...);
  return p;
}
template <class D, size_t K>
NX_FORCE_INLINE D horner_fast(const D &x, const float (&c)[K]) {
#if defined(NX_SIMD_HAS_FMA)
  return horner_fast(x, c, std::make_index_sequence<K - 1>{});
#else
  return horner(x, c);
#endif
}

/// 2^n for n in [-126, 127].
template <class D> NX_FORCE_INLINE D pow2i(const ints<D> &n) {
  return from_bits<D>((n + 127) << 23);
}
/// x >= 0 with the sign of s.
template <class D> NX_FORCE_INLINE D with_sign(const D &x, const D &s) {
  return from_bits<D>(bits(x) | (bits(s) & ints<D>(INT_MIN)));
}
template <class D> NX_FORCE_INLINE D magnitude(const D &x) {
  return from_bits<D>(bits(x) & ints<D>(INT_MAX));
}

/// n = round(x / ln 2), r = x - n ln 2, with |r| <= ln 2 / 2.
template <class D> NX_FORCE_INLINE void reduce(const D &x, D &r, ints<D> &n) {
  const D t = x * log2e + round_shift;
  const D nf = t - round_shift;
  n = bits(t) - bits(D(round_shift));
  r = (x - nf * ln2_hi) - nf * ln2_lo;
}

// The clamps keep a NaN: min and max return their first operand beside one.

template <class D> D exp(const D &x) {
  // -104 and 89 lie past the underflow to 0 and the overflow to infinity.
  const D xc = min(max(x, D(-104.f)), D(89.f));
  D r;
  ints<D> n;
  reduce(xc, r, n);
  const D e = horner(r, expm1_p) * (r * r) + r + 1.f;
  // Two factors reach 2^-150 and 2^128 with the one rounding of the second.
  const ints<D> n1 = n >> 1;
  return e * pow2i<D>(n1) * pow2i<D>(n - n1);
}

template <class D> D fast_exp(const D &x) {
  const D xc = min(max(x, D(-104.f)), D(89.f));
  D r;
  ints<D> n;
  reduce(xc, r, n);
  const D e = horner_fast(r, fast_exp_p) * (r * r) + r + 1.f;
  const ints<D> n1 = n >> 1;
  return e * pow2i<D>(n1) * pow2i<D>(n - n1);
}

template <class D> D exp2(const D &x) {
  const D xc = min(max(x, D(-151.f)), D(129.f));
  const D t = xc + round_shift;
  const D nf = t - round_shift;
  const ints<D> n = bits(t) - bits(D(round_shift));
  const D f = xc - nf;
  const D e = horner(f, exp2_q) * f + 1.f;
  const ints<D> n1 = n >> 1;
  return e * pow2i<D>(n1) * pow2i<D>(n - n1);
}

template <class D> D expm1(const D &x) {
  // expm1 is -1 in float below -17.4; -87 keeps 2^n normal.
  const D xc = min(max(x, D(-87.f)), D(89.f));
  D r;
  ints<D> n;
  reduce(xc, r, n);
  const D em1 = horner(r, expm1_p) * (r * r) + r;
  // 2^n em1 + (2^n - 1) keeps the precision near zero; from 2^25 on the -1
  // no longer shows, and two factors carry the scale up to the overflow. The
  // two agree there, so the switch at x = 44 (2^63.5) changes no bit.
  const D s = pow2i<D>(min(n, ints<D>(127)));
  const D scaled = em1 * s + (s - 1.f);
  const ints<D> n1 = n >> 1;
  const D split = (em1 + 1.f) * pow2i<D>(n1) * pow2i<D>(n - n1);
  const D v = select(xc > D(44.f), split, scaled);
  return select(x != x || x == 0.f, x, v);
}

/// x = m 2^e with m in [sqrt(1/2), sqrt(2)); f = m - 1.
template <class D> NX_FORCE_INLINE void split_log(const D &x, D &f, D &e) {
  using I = ints<D>;
  const auto tiny = x < D(0x1p-126f);
  const D xs = select(tiny, x * 0x1p23f, x);
  const I b = bits(xs);
  D m = from_bits<D>((b & I(0x007fffff)) | I(0x3f800000));
  const auto high = m > D(1.41421356f);
  m = select(high, m * 0.5f, m);
  f = m - 1.f;
  e = static_cast<D>(b >> 23) - select(tiny, D(150.f), D(127.f)) +
      select(high, D(1.f), D(0.f));
}

/// log(1 + f) - f, for f from split_log.
template <class D> NX_FORCE_INLINE D log1p_tail(const D &f) {
  const D f2 = f * f;
  return horner(f, log_l) * f * f2 - f2 * 0.5f;
}

inline constexpr float inf = std::numeric_limits<float>::infinity();

template <class D> D log_special(const D &x, const D &v) {
  D r = select(x == D(inf), x, v);
  r = select(x == 0.f, D(-inf), r);
  return select(x < 0.f || x != x, D(std::numeric_limits<float>::quiet_NaN()),
                r);
}

/// log(x) before the special values.
template <class D> NX_FORCE_INLINE D log_finite(const D &x) {
  D f, e;
  split_log(x, f, e);
  const D f2 = f * f;
  D y = horner(f, log_l) * f * f2;
  y = y + e * ln2_lo;
  y = y - f2 * 0.5f;
  return f + y + e * ln2_hi;
}

template <class D> D log(const D &x) { return log_special(x, log_finite(x)); }

template <class D> D log2(const D &x) {
  D f, e;
  split_log(x, f, e);
  return log_special(x, (f + log1p_tail(f)) * log2e + e);
}

template <class D> D log1p(const D &x) {
  const D u = x + 1.f;
  // u rounds 1 + x; c restores what the rounding dropped.
  const D c = (x - (u - 1.f)) / u;
  D v = log_special(u, log_finite(u)) + c;
  v = select(x == -1.f, D(-inf), v);
  return select(x == D(inf) || x == 0.f, x, v);
}

template <class D> D tanh(const D &x) {
  const D a = magnitude(x);
  const D a2 = a * a;
  const D inner = a + horner(a2, tanh_t) * a2 * a;
  const D outer = 1.f - 2.f / (exp(a + a) + 1.f);
  return with_sign(select(a < D(0.625f), inner, outer), x);
}

template <class D> D sigmoid(const D &x) {
  const D e = exp(-magnitude(x));
  const D d = e + 1.f;
  return select(x >= 0.f, 1.f / d, e / d);
}

template <class D> D erf(const D &x) {
  const D a = magnitude(x);
  const D a2 = a * a;
  const D inner = a + a * horner(a2, erf_h);
  const D outer = 1.f - exp(-a2) * horner(a - 2.4f, erfc_g);
  D v = select(a < D(0.875f), inner, outer);
  v = select(a >= D(erf_one), D(1.f), v);
  return with_sign(v, x);
}

template <class D> D fast_tanh(const D &x) {
  const D a = min(magnitude(x), D(tanh_one));
  const D a2 = a * a;
  const D v = a * horner_fast(a2, fast_tanh_n) / horner_fast(a2, fast_tanh_d);
  return with_sign(min(v, D(1.f)), x);
}

template <class D> D fast_sigmoid(const D &x) {
  return 1.f / (fast_exp(-x) + 1.f);
}

template <class D> D fast_erf(const D &x) {
  const D a = min(magnitude(x), D(erf_one));
  const D a2 = a * a;
  const D v = a * horner_fast(a2, fast_erf_n) / horner_fast(a2, fast_erf_d);
  return with_sign(min(v, D(1.f)), x);
}

} // namespace nx::simd::detail::float_math
