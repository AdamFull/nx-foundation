#pragma once

#include "core/foundation/simd/algorithm.h"
#include "core/foundation/simd/float_math.h"

#include <cmath>
#include <version>

namespace nx::simd {

namespace detail {

template <class V>
using deduced_vec_t =
    decltype(std::declval<const V &>() + std::declval<const V &>());

template <class V>
concept math_floating_point = requires {
  typename deduced_vec_t<V>;
} && simd_floating_point<std::remove_cvref_t<deduced_vec_t<V>>>;

template <class... Vs> struct first_math_floating_point {
  using type = void;
};
template <class V, class... Rest> struct first_math_floating_point<V, Rest...> {
  using type =
      std::conditional_t<math_floating_point<V>, V,
                         typename first_math_floating_point<Rest...>::type>;
};

template <class... Vs> consteval auto math_common_pick() {
  using First = typename first_math_floating_point<Vs...>::type;
  if constexpr (std::is_void_v<First>) {
    return static_cast<void *>(nullptr);
  } else {
    using D = deduced_vec_t<First>;
    if constexpr ((std::convertible_to<const Vs &, D> && ...))
      return std::type_identity<D>{};
    else
      return static_cast<void *>(nullptr);
  }
}

template <class... Vs>
concept math_common =
    !std::is_same_v<decltype(math_common_pick<Vs...>()), void *>;

template <class... Vs>
using math_common_t = typename decltype(math_common_pick<Vs...>())::type;

template <class D, class F, class... Args>
D map_elements(F f, const Args &...args) {
  return vec_generate<D>([&](int i) { return f(args[i]...); });
}

template <class D> using int_vec = rebind_t<int, D>;

} // namespace detail

#define NX_SIMD_MATH_SCALAR(name)                                              \
  template <class V>                                                           \
    requires detail::math_floating_point<V>                                    \
  detail::deduced_vec_t<V> name(const V &x) {                                  \
    using D = detail::deduced_vec_t<V>;                                        \
    const D d = x;                                                             \
    return detail::map_elements<D>(                                            \
        [](typename D::value_type e) { return std::name(e); }, d);             \
  }
NX_SIMD_MATH_SCALAR(acos)
NX_SIMD_MATH_SCALAR(asin)
NX_SIMD_MATH_SCALAR(atan)
NX_SIMD_MATH_SCALAR(cos)
NX_SIMD_MATH_SCALAR(sin)
NX_SIMD_MATH_SCALAR(tan)
NX_SIMD_MATH_SCALAR(acosh)
NX_SIMD_MATH_SCALAR(asinh)
NX_SIMD_MATH_SCALAR(atanh)
NX_SIMD_MATH_SCALAR(cosh)
NX_SIMD_MATH_SCALAR(sinh)
NX_SIMD_MATH_SCALAR(log10)
NX_SIMD_MATH_SCALAR(logb)
NX_SIMD_MATH_SCALAR(cbrt)
NX_SIMD_MATH_SCALAR(erfc)
NX_SIMD_MATH_SCALAR(lgamma)
NX_SIMD_MATH_SCALAR(tgamma)
#undef NX_SIMD_MATH_SCALAR

// float vectors run detail::float_math's register-wide kernels: within a few
// ulp of <cmath>, and the same bits on every backend. double runs <cmath>.
#define NX_SIMD_MATH_FLOAT(name)                                               \
  template <class V>                                                           \
    requires detail::math_floating_point<V>                                    \
  detail::deduced_vec_t<V> name(const V &x) {                                  \
    using D = detail::deduced_vec_t<V>;                                        \
    const D d = x;                                                             \
    if constexpr (std::is_same_v<typename D::value_type, float>)               \
      return detail::float_math::name(d);                                      \
    else                                                                       \
      return detail::map_elements<D>(                                          \
          [](typename D::value_type e) { return std::name(e); }, d);           \
  }
NX_SIMD_MATH_FLOAT(exp)
NX_SIMD_MATH_FLOAT(exp2)
NX_SIMD_MATH_FLOAT(expm1)
NX_SIMD_MATH_FLOAT(log)
NX_SIMD_MATH_FLOAT(log2)
NX_SIMD_MATH_FLOAT(log1p)
NX_SIMD_MATH_FLOAT(tanh)
NX_SIMD_MATH_FLOAT(erf)
#undef NX_SIMD_MATH_FLOAT

// Extensions for float vectors. sigmoid(x) = 1 / (1 + e^-x) is as accurate as
// exp. The fast_ forms trade accuracy for speed (relative error at most 1e-5,
// fast_erf 3e-5) and use FMA where the build has it, so their last bits
// depend on the target.
#define NX_SIMD_MATH_FLOAT_ONLY(name)                                          \
  template <class V>                                                           \
    requires detail::math_floating_point<V> &&                                 \
             std::is_same_v<typename detail::deduced_vec_t<V>::value_type,     \
                            float>                                             \
  detail::deduced_vec_t<V> name(const V &x) {                                  \
    return detail::float_math::name(detail::deduced_vec_t<V>(x));              \
  }
NX_SIMD_MATH_FLOAT_ONLY(sigmoid)
NX_SIMD_MATH_FLOAT_ONLY(fast_exp)
NX_SIMD_MATH_FLOAT_ONLY(fast_tanh)
NX_SIMD_MATH_FLOAT_ONLY(fast_sigmoid)
NX_SIMD_MATH_FLOAT_ONLY(fast_erf)
#undef NX_SIMD_MATH_FLOAT_ONLY

#define NX_SIMD_MATH_REGISTER(name, op)                                        \
  template <class V>                                                           \
    requires detail::math_floating_point<V>                                    \
  detail::deduced_vec_t<V> name(const V &x) {                                  \
    using D = detail::deduced_vec_t<V>;                                        \
    const D d = x;                                                             \
    return detail::access::make<D>(                                            \
        detail::vops<D>::op(detail::access::data(d)));                         \
  }
NX_SIMD_MATH_REGISTER(sqrt, sqrt)
NX_SIMD_MATH_REGISTER(fabs, abs)
NX_SIMD_MATH_REGISTER(abs, abs)
NX_SIMD_MATH_REGISTER(floor, floor)
NX_SIMD_MATH_REGISTER(ceil, ceil)
NX_SIMD_MATH_REGISTER(trunc, trunc)
NX_SIMD_MATH_REGISTER(round, round)
NX_SIMD_MATH_REGISTER(rint, rint)
NX_SIMD_MATH_REGISTER(nearbyint, rint)
#undef NX_SIMD_MATH_REGISTER

template <class T, class Abi>
  requires std::signed_integral<T>
constexpr basic_vec<T, Abi> abs(const basic_vec<T, Abi> &x) noexcept {
  if (std::is_constant_evaluated())
    return detail::vec_generate<basic_vec<T, Abi>>(
        [&](int i) { return static_cast<T>(x[i] < 0 ? -x[i] : x[i]); });
  return detail::access::make<basic_vec<T, Abi>>(
      detail::vops<basic_vec<T, Abi>>::abs(detail::access::data(x)));
}

template <class V>
  requires detail::math_floating_point<V>
detail::deduced_vec_t<V> frexp(const V &value,
                               detail::int_vec<detail::deduced_vec_t<V>> *exp) {
  using D = detail::deduced_vec_t<V>;
  const D d = value;
  int e[D::size()];
  const D r =
      detail::vec_generate<D>([&](int i) { return std::frexp(d[i], &e[i]); });
  *exp = detail::vec_generate<detail::int_vec<D>>([&](int i) { return e[i]; });
  return r;
}

#define NX_SIMD_MATH_TO(name, R)                                               \
  template <class V>                                                           \
    requires detail::math_floating_point<V>                                    \
  rebind_t<R, detail::deduced_vec_t<V>> name(const V &x) {                     \
    using D = detail::deduced_vec_t<V>;                                        \
    const D d = x;                                                             \
    return detail::vec_generate<rebind_t<R, D>>(                               \
        [&](int i) { return static_cast<R>(std::name(d[i])); });               \
  }
NX_SIMD_MATH_TO(ilogb, int)
NX_SIMD_MATH_TO(lrint, long)
NX_SIMD_MATH_TO(llrint, long long)
NX_SIMD_MATH_TO(lround, long)
NX_SIMD_MATH_TO(llround, long long)
NX_SIMD_MATH_TO(fpclassify, int)
#undef NX_SIMD_MATH_TO

template <class V>
  requires detail::math_floating_point<V>
typename detail::deduced_vec_t<V>::mask_type isnan(const V &x) {
  using D = detail::deduced_vec_t<V>;
  const D d = x;
  return d != d;
}
template <class V>
  requires detail::math_floating_point<V>
typename detail::deduced_vec_t<V>::mask_type isinf(const V &x) {
  using D = detail::deduced_vec_t<V>;
  using T = typename D::value_type;
  return fabs(x) == D(std::numeric_limits<T>::infinity());
}
template <class V>
  requires detail::math_floating_point<V>
typename detail::deduced_vec_t<V>::mask_type isfinite(const V &x) {
  using D = detail::deduced_vec_t<V>;
  using T = typename D::value_type;
  return fabs(x) <= D(std::numeric_limits<T>::max());
}
template <class V>
  requires detail::math_floating_point<V>
typename detail::deduced_vec_t<V>::mask_type isnormal(const V &x) {
  using D = detail::deduced_vec_t<V>;
  using T = typename D::value_type;
  const D a = fabs(x);
  return a >= D(std::numeric_limits<T>::min()) &&
         a <= D(std::numeric_limits<T>::max());
}
template <class V>
  requires detail::math_floating_point<V>
typename detail::deduced_vec_t<V>::mask_type signbit(const V &x) {
  using D = detail::deduced_vec_t<V>;
  const D d = x;
  return detail::make_mask<typename D::mask_type>(
      [&](int i) { return std::signbit(d[i]); });
}

template <class V>
  requires detail::math_floating_point<V>
detail::deduced_vec_t<V>
ldexp(const V &x, const detail::int_vec<detail::deduced_vec_t<V>> &exp) {
  using D = detail::deduced_vec_t<V>;
  const D d = x;
  return detail::vec_generate<D>(
      [&](int i) { return std::ldexp(d[i], exp[i]); });
}
template <class V>
  requires detail::math_floating_point<V>
detail::deduced_vec_t<V>
scalbn(const V &x, const detail::int_vec<detail::deduced_vec_t<V>> &n) {
  using D = detail::deduced_vec_t<V>;
  const D d = x;
  return detail::vec_generate<D>(
      [&](int i) { return std::scalbn(d[i], n[i]); });
}
template <class V>
  requires detail::math_floating_point<V>
detail::deduced_vec_t<V>
scalbln(const V &x, const rebind_t<long, detail::deduced_vec_t<V>> &n) {
  using D = detail::deduced_vec_t<V>;
  const D d = x;
  return detail::vec_generate<D>(
      [&](int i) { return std::scalbln(d[i], n[i]); });
}

template <class T, class Abi>
  requires std::floating_point<T>
basic_vec<T, Abi> modf(const std::type_identity_t<basic_vec<T, Abi>> &value,
                       basic_vec<T, Abi> *iptr) {
  T whole[basic_vec<T, Abi>::size()];
  const auto r = detail::vec_generate<basic_vec<T, Abi>>(
      [&](int i) { return std::modf(value[i], &whole[i]); });
  *iptr =
      detail::vec_generate<basic_vec<T, Abi>>([&](int i) { return whole[i]; });
  return r;
}

#define NX_SIMD_MATH_BINARY(name)                                              \
  template <class V0, class V1>                                                \
    requires detail::math_common<V0, V1>                                       \
  detail::math_common_t<V0, V1> name(const V0 &x, const V1 &y) {               \
    using D = detail::math_common_t<V0, V1>;                                   \
    const D a = x;                                                             \
    const D b = y;                                                             \
    return detail::map_elements<D>(                                            \
        [](typename D::value_type p, typename D::value_type q) {               \
          return std::name(p, q);                                              \
        },                                                                     \
        a, b);                                                                 \
  }
NX_SIMD_MATH_BINARY(pow)
NX_SIMD_MATH_BINARY(fmod)
NX_SIMD_MATH_BINARY(remainder)
NX_SIMD_MATH_BINARY(copysign)
NX_SIMD_MATH_BINARY(nextafter)
NX_SIMD_MATH_BINARY(fdim)
NX_SIMD_MATH_BINARY(hypot)
NX_SIMD_MATH_BINARY(atan2)
#undef NX_SIMD_MATH_BINARY

template <class V0, class V1>
  requires detail::math_common<V0, V1>
detail::math_common_t<V0, V1> fmax(const V0 &x, const V1 &y) {
  using D = detail::math_common_t<V0, V1>;
  const D a = x;
  const D b = y;
  return detail::access::make<D>(
      detail::vops<D>::fmax(detail::access::data(a), detail::access::data(b)));
}
template <class V0, class V1>
  requires detail::math_common<V0, V1>
detail::math_common_t<V0, V1> fmin(const V0 &x, const V1 &y) {
  using D = detail::math_common_t<V0, V1>;
  const D a = x;
  const D b = y;
  return detail::access::make<D>(
      detail::vops<D>::fmin(detail::access::data(a), detail::access::data(b)));
}

#define NX_SIMD_MATH_COMPARE(name, expr)                                       \
  template <class V0, class V1>                                                \
    requires detail::math_common<V0, V1>                                       \
  typename detail::math_common_t<V0, V1>::mask_type name(const V0 &x,          \
                                                         const V1 &y) {        \
    using D = detail::math_common_t<V0, V1>;                                   \
    const D a = x;                                                             \
    const D b = y;                                                             \
    return expr;                                                               \
  }
NX_SIMD_MATH_COMPARE(isgreater, a > b)
NX_SIMD_MATH_COMPARE(isgreaterequal, a >= b)
NX_SIMD_MATH_COMPARE(isless, a < b)
NX_SIMD_MATH_COMPARE(islessequal, a <= b)
NX_SIMD_MATH_COMPARE(islessgreater, (a < b) || (b < a))
NX_SIMD_MATH_COMPARE(isunordered, (a != a) || (b != b))
#undef NX_SIMD_MATH_COMPARE

template <class V0, class V1>
  requires detail::math_common<V0, V1>
detail::math_common_t<V0, V1>
remquo(const V0 &x, const V1 &y,
       detail::int_vec<detail::math_common_t<V0, V1>> *quo) {
  using D = detail::math_common_t<V0, V1>;
  const D a = x;
  const D b = y;
  int q[D::size()];
  const D r = detail::vec_generate<D>(
      [&](int i) { return std::remquo(a[i], b[i], &q[i]); });
  *quo = detail::vec_generate<detail::int_vec<D>>([&](int i) { return q[i]; });
  return r;
}

template <class V0, class V1, class V2>
  requires detail::math_common<V0, V1, V2>
detail::math_common_t<V0, V1, V2> hypot(const V0 &x, const V1 &y, const V2 &z) {
  using D = detail::math_common_t<V0, V1, V2>;
  const D a = x;
  const D b = y;
  const D c = z;
  return detail::map_elements<D>(
      [](auto p, auto q, auto r) { return std::hypot(p, q, r); }, a, b, c);
}
template <class V0, class V1, class V2>
  requires detail::math_common<V0, V1, V2>
detail::math_common_t<V0, V1, V2> fma(const V0 &x, const V1 &y, const V2 &z) {
  using D = detail::math_common_t<V0, V1, V2>;
  const D a = x;
  const D b = y;
  const D c = z;
  return detail::access::make<D>(detail::vops<D>::fma(detail::access::data(a),
                                                      detail::access::data(b),
                                                      detail::access::data(c)));
}
template <class V0, class V1, class V2>
  requires detail::math_common<V0, V1, V2>
detail::math_common_t<V0, V1, V2> lerp(const V0 &x, const V1 &y,
                                       const V2 &t) noexcept {
  using D = detail::math_common_t<V0, V1, V2>;
  const D a = x;
  const D b = y;
  const D c = t;
  return detail::map_elements<D>(
      [](auto p, auto q, auto r) { return std::lerp(p, q, r); }, a, b, c);
}

#if defined(__cpp_lib_math_special_functions)

#define NX_SIMD_SPECIAL_UNARY(name)                                            \
  template <class V>                                                           \
    requires detail::math_floating_point<V>                                    \
  detail::deduced_vec_t<V> name(const V &x) {                                  \
    using D = detail::deduced_vec_t<V>;                                        \
    const D d = x;                                                             \
    return detail::map_elements<D>(                                            \
        [](typename D::value_type e) { return std::name(e); }, d);             \
  }
NX_SIMD_SPECIAL_UNARY(comp_ellint_1)
NX_SIMD_SPECIAL_UNARY(comp_ellint_2)
NX_SIMD_SPECIAL_UNARY(expint)
NX_SIMD_SPECIAL_UNARY(riemann_zeta)
#undef NX_SIMD_SPECIAL_UNARY

#define NX_SIMD_SPECIAL_BINARY(name)                                           \
  template <class V0, class V1>                                                \
    requires detail::math_common<V0, V1>                                       \
  detail::math_common_t<V0, V1> name(const V0 &x, const V1 &y) {               \
    using D = detail::math_common_t<V0, V1>;                                   \
    const D a = x;                                                             \
    const D b = y;                                                             \
    return detail::map_elements<D>(                                            \
        [](typename D::value_type p, typename D::value_type q) {               \
          return std::name(p, q);                                              \
        },                                                                     \
        a, b);                                                                 \
  }
NX_SIMD_SPECIAL_BINARY(beta)
NX_SIMD_SPECIAL_BINARY(comp_ellint_3)
NX_SIMD_SPECIAL_BINARY(cyl_bessel_i)
NX_SIMD_SPECIAL_BINARY(cyl_bessel_j)
NX_SIMD_SPECIAL_BINARY(cyl_bessel_k)
NX_SIMD_SPECIAL_BINARY(cyl_neumann)
NX_SIMD_SPECIAL_BINARY(ellint_1)
NX_SIMD_SPECIAL_BINARY(ellint_2)
#undef NX_SIMD_SPECIAL_BINARY

template <class V0, class V1, class V2>
  requires detail::math_common<V0, V1, V2>
detail::math_common_t<V0, V1, V2> ellint_3(const V0 &k, const V1 &nu,
                                           const V2 &phi) {
  using D = detail::math_common_t<V0, V1, V2>;
  const D a = k;
  const D b = nu;
  const D c = phi;
  return detail::map_elements<D>(
      [](auto p, auto q, auto r) { return std::ellint_3(p, q, r); }, a, b, c);
}

#define NX_SIMD_SPECIAL_INDEXED(name)                                          \
  template <class V>                                                           \
    requires detail::math_floating_point<V>                                    \
  detail::deduced_vec_t<V> name(                                               \
      const rebind_t<unsigned, detail::deduced_vec_t<V>> &n, const V &x) {     \
    using D = detail::deduced_vec_t<V>;                                        \
    const D d = x;                                                             \
    return detail::vec_generate<D>(                                            \
        [&](int i) { return std::name(n[i], d[i]); });                         \
  }
NX_SIMD_SPECIAL_INDEXED(hermite)
NX_SIMD_SPECIAL_INDEXED(laguerre)
NX_SIMD_SPECIAL_INDEXED(legendre)
NX_SIMD_SPECIAL_INDEXED(sph_bessel)
NX_SIMD_SPECIAL_INDEXED(sph_neumann)
#undef NX_SIMD_SPECIAL_INDEXED

#define NX_SIMD_SPECIAL_INDEXED2(name)                                         \
  template <class V>                                                           \
    requires detail::math_floating_point<V>                                    \
  detail::deduced_vec_t<V> name(                                               \
      const rebind_t<unsigned, detail::deduced_vec_t<V>> &n,                   \
      const rebind_t<unsigned, detail::deduced_vec_t<V>> &m, const V &x) {     \
    using D = detail::deduced_vec_t<V>;                                        \
    const D d = x;                                                             \
    return detail::vec_generate<D>(                                            \
        [&](int i) { return std::name(n[i], m[i], d[i]); });                   \
  }
NX_SIMD_SPECIAL_INDEXED2(assoc_laguerre)
NX_SIMD_SPECIAL_INDEXED2(assoc_legendre)
NX_SIMD_SPECIAL_INDEXED2(sph_legendre)
#undef NX_SIMD_SPECIAL_INDEXED2

#endif

} // namespace nx::simd
