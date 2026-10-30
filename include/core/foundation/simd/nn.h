#pragma once

#include "core/foundation/simd/linalg.h"
#include "core/foundation/simd/math.h"

#include <cmath>
#include <limits>
#include <span>
#include <type_traits>

// Softmax, normalization and activation kernels over float spans. Sums run in
// linalg's 16-lane order and precision::exact computes with
// simd/float_math.h's kernels, so exact results are the same bits on every
// backend. out may be the same span as an input, or disjoint from it.
namespace nx::simd::nn {

enum class precision {
  /// exp, tanh, erf and sigmoid within 2.4 ulp.
  exact,
  /// Their fast_ forms: relative error at most 3e-5, and the last bits depend
  /// on the target's FMA.
  fast,
};

/// What layernorm normalized with: (x - mean) rstd, before gamma and beta.
struct moments {
  float mean, rstd;
};

namespace impl {

using V = vec<float>;
namespace la = linalg::impl;

template <precision P> NX_FORCE_INLINE V exp(const V &x) {
  if constexpr (P == precision::exact)
    return simd::exp(x);
  else
    return simd::fast_exp(x);
}
template <precision P> NX_FORCE_INLINE V tanh(const V &x) {
  if constexpr (P == precision::exact)
    return simd::tanh(x);
  else
    return simd::fast_tanh(x);
}
template <precision P> NX_FORCE_INLINE V erf(const V &x) {
  if constexpr (P == precision::exact)
    return simd::erf(x);
  else
    return simd::fast_erf(x);
}
template <precision P> NX_FORCE_INLINE V sigmoid(const V &x) {
  if constexpr (P == precision::exact)
    return simd::sigmoid(x);
  else
    return simd::fast_sigmoid(x);
}

template <class F> NX_FORCE_INLINE decltype(auto) dispatch(precision p, F f) {
  if (p == precision::exact)
    return f(std::integral_constant<precision, precision::exact>{});
  return f(std::integral_constant<precision, precision::fast>{});
}

// float_math's log, not <cmath>'s, which differs between C libraries.
inline float log(float x) { return simd::log(V(x))[0]; }
// One instruction where MSVC calls the C library's sqrtf.
inline float sqrt(float x) { return simd::sqrt(V(x))[0]; }

/// The sum of e^(x[i] - m) for a finite m.
template <precision P> float sum_exp(std::span<const float> x, float m) {
  const V mv(m);
  return la::accumulate<float>(
      x.size(), -std::numeric_limits<float>::infinity(),
      [mv](const V &v) { return exp<P>(v - mv); }, x.data());
}

template <precision P>
void softmax(std::span<const float> x, std::span<float> out) {
  NX_ASSERT(x.size() == out.size(), "softmax: spans of different sizes");
  if (x.empty())
    return;
  const V m(linalg::max(x));
  la::map<float>(out, [m](const V &v) { return exp<P>(v - m); }, x);
  const V s(linalg::sum(std::span<const float>(out)));
  la::map<float>(
      out, [s](const V &v) { return v / s; }, std::span<const float>(out));
}

template <precision P> float logsumexp(std::span<const float> x) {
  const float m = linalg::max(x);
  if (std::isinf(m))
    return m;
  return m + log(sum_exp<P>(x, m));
}

template <precision P>
void log_softmax(std::span<const float> x, std::span<float> out) {
  NX_ASSERT(x.size() == out.size(), "log_softmax: spans of different sizes");
  if (x.empty())
    return;
  const float m = linalg::max(x);
  const V mv(m), l(std::isinf(m) ? std::numeric_limits<float>::quiet_NaN()
                                 : log(sum_exp<P>(x, m)));
  la::map<float>(out, [mv, l](const V &v) { return (v - mv) - l; }, x);
}

template <precision P>
void gelu(std::span<const float> x, std::span<float> out) {
  const V k(0.707106781f);
  la::map<float>(
      out, [k](const V &v) { return 0.5f * v * (1.f + erf<P>(v * k)); }, x);
}

template <precision P>
void gelu_tanh(std::span<const float> x, std::span<float> out) {
  const V c(0.797884561f), k(0.044715f);
  la::map<float>(
      out,
      [c, k](const V &v) {
        return 0.5f * v * (1.f + tanh<P>(c * (v + k * (v * v * v))));
      },
      x);
}

template <precision P>
void silu(std::span<const float> x, std::span<float> out) {
  la::map<float>(out, [](const V &v) { return v * sigmoid<P>(v); }, x);
}

template <precision P>
void swiglu(std::span<const float> gate, std::span<const float> up,
            std::span<float> out) {
  la::map<float>(
      out, [](const V &g, const V &u) { return g * sigmoid<P>(g) * u; }, gate,
      up);
}

} // namespace impl

/// e^(x[i] - max) / sum of them. A -inf element gets 0; a NaN, a +inf or a
/// span of -inf alone makes every element NaN.
inline void softmax(std::span<const float> x, std::span<float> out,
                    precision p = precision::exact) {
  impl::dispatch(p, [&](auto t) { impl::softmax<decltype(t)::value>(x, out); });
}

/// log of the sum of e^x[i]: max + log(sum of e^(x[i] - max)). -inf for an
/// empty span or one of -inf alone, +inf with any +inf, NaN with any NaN.
inline float logsumexp(std::span<const float> x,
                       precision p = precision::exact) {
  return impl::dispatch(
      p, [&](auto t) { return impl::logsumexp<decltype(t)::value>(x); });
}

/// (x[i] - max) - log(sum of e^(x[i] - max)), NaN everywhere where softmax
/// is.
inline void log_softmax(std::span<const float> x, std::span<float> out,
                        precision p = precision::exact) {
  impl::dispatch(
      p, [&](auto t) { impl::log_softmax<decltype(t)::value>(x, out); });
}

/// x[i] / sqrt(mean of x^2 + eps) gamma[i], multiplied in that order; gamma
/// may be empty.
inline void rmsnorm(std::span<const float> x, std::span<const float> gamma,
                    float eps, std::span<float> out) {
  using impl::V;
  NX_ASSERT(x.size() == out.size(), "rmsnorm: spans of different sizes");
  NX_ASSERT(gamma.empty() || gamma.size() == x.size(),
            "rmsnorm: gamma is neither empty nor x's size");
  if (x.empty())
    return;
  const V r(1.f / impl::sqrt(linalg::sum_squares(x) / float(x.size()) + eps));
  if (gamma.empty())
    impl::la::map<float>(out, [r](const V &v) { return v * r; }, x);
  else
    impl::la::map<float>(
        out, [r](const V &v, const V &g) { return v * r * g; }, x, gamma);
}

/// (x[i] - mean) rstd gamma[i] + beta[i], with rstd = 1 / sqrt(var + eps) and
/// the variance summed as (x[i] - mean)^2 after the mean. gamma and beta may
/// each be empty; an empty x does nothing and returns zeros.
inline moments layernorm(std::span<const float> x, std::span<const float> gamma,
                         std::span<const float> beta, float eps,
                         std::span<float> out) {
  using impl::V;
  NX_ASSERT(x.size() == out.size(), "layernorm: spans of different sizes");
  NX_ASSERT(gamma.empty() || gamma.size() == x.size(),
            "layernorm: gamma is neither empty nor x's size");
  NX_ASSERT(beta.empty() || beta.size() == x.size(),
            "layernorm: beta is neither empty nor x's size");
  if (x.empty())
    return {};
  const float n = float(x.size());
  const float mean = linalg::sum(x) / n;
  const V m(mean);
  // The lanes past the end load the mean, so they add 0.
  const float var = impl::la::accumulate<float>(
                        x.size(), mean,
                        [m](const V &v) {
                          const V d = v - m;
                          return d * d;
                        },
                        x.data()) /
                    n;
  const float rstd = 1.f / impl::sqrt(var + eps);
  const V r(rstd);
  if (gamma.empty() && beta.empty())
    impl::la::map<float>(out, [m, r](const V &v) { return (v - m) * r; }, x);
  else if (beta.empty())
    impl::la::map<float>(
        out, [m, r](const V &v, const V &g) { return (v - m) * r * g; }, x,
        gamma);
  else if (gamma.empty())
    impl::la::map<float>(
        out, [m, r](const V &v, const V &b) { return (v - m) * r + b; }, x,
        beta);
  else
    impl::la::map<float>(
        out,
        [m, r](const V &v, const V &g, const V &b) {
          return (v - m) * r * g + b;
        },
        x, gamma, beta);
  return {mean, rstd};
}

/// x/2 (1 + erf(x / sqrt 2)).
inline void gelu(std::span<const float> x, std::span<float> out,
                 precision p = precision::exact) {
  impl::dispatch(p, [&](auto t) { impl::gelu<decltype(t)::value>(x, out); });
}

/// x/2 (1 + tanh(sqrt(2 / pi) (x + 0.044715 x^3))), GPT-2's form.
inline void gelu_tanh(std::span<const float> x, std::span<float> out,
                      precision p = precision::exact) {
  impl::dispatch(p,
                 [&](auto t) { impl::gelu_tanh<decltype(t)::value>(x, out); });
}

/// x sigmoid(x).
inline void silu(std::span<const float> x, std::span<float> out,
                 precision p = precision::exact) {
  impl::dispatch(p, [&](auto t) { impl::silu<decltype(t)::value>(x, out); });
}

/// silu(gate[i]) up[i].
inline void swiglu(std::span<const float> gate, std::span<const float> up,
                   std::span<float> out, precision p = precision::exact) {
  impl::dispatch(
      p, [&](auto t) { impl::swiglu<decltype(t)::value>(gate, up, out); });
}

} // namespace nx::simd::nn
