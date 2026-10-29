#include "simd/simd_test_util.h"

#include <array>
#include <bit>
#include <cmath>
#include <complex>

using namespace simd_test;

namespace {

template <class T> T special(int k) {
  constexpr T values[] = {T(0),
                          T(-0.0),
                          std::numeric_limits<T>::infinity(),
                          -std::numeric_limits<T>::infinity(),
                          std::numeric_limits<T>::quiet_NaN(),
                          std::numeric_limits<T>::denorm_min(),
                          std::numeric_limits<T>::min(),
                          std::numeric_limits<T>::max(),
                          T(0.5),
                          T(-2.5),
                          T(1),
                          T(-1)};
  return values[k % int(std::size(values))];
}

// Mostly ordinary values across magnitudes, with a special value in every
// fourth lane.
template <class V> V math_input(rng<typename V::value_type> &r, int round) {
  using T = typename V::value_type;
  int next_special = round * 5;
  return V([&](int i) {
    if ((i + round) % 4 == 3)
      return special<T>(next_special++);
    return r.any() * T(64);
  });
}

template <class V> V unit_input(rng<typename V::value_type> &r) {
  using T = typename V::value_type;
  return V([&](int) { return T(r.any() / T(4096)); });
}

#define CHECK_UNARY(fn, input)                                                 \
  check_lanes(#fn, sd::fn(input), [&](int i) { return std::fn(input[i]); })

// float runs the register-wide kernels that test_simd_float_math.cpp bounds.
#define CHECK_DOUBLE_UNARY(fn, input)                                          \
  if constexpr (std::is_same_v<T, double>)                                     \
  CHECK_UNARY(fn, input)

template <class T, int N> void math_unary() {
  using V = sd::vec<T, N>;
  rng<T> r(N * 101 + sizeof(T));
  for (int round = 0; round < 6; ++round) {
    const V x = math_input<V>(r, round);
    const V u = unit_input<V>(r);
    const V p = sd::fabs(x);
    const V big = p + T(1);

    CHECK_UNARY(acos, u);
    CHECK_UNARY(asin, u);
    CHECK_UNARY(atan, x);
    CHECK_UNARY(cos, x);
    CHECK_UNARY(sin, x);
    CHECK_UNARY(tan, x);
    CHECK_UNARY(acosh, big);
    CHECK_UNARY(asinh, x);
    CHECK_UNARY(atanh, u);
    CHECK_UNARY(cosh, u);
    CHECK_UNARY(sinh, u);
    CHECK_DOUBLE_UNARY(tanh, x);
    CHECK_DOUBLE_UNARY(exp, u);
    CHECK_DOUBLE_UNARY(exp2, u);
    CHECK_DOUBLE_UNARY(expm1, u);
    CHECK_DOUBLE_UNARY(log, p);
    CHECK_UNARY(log10, p);
    CHECK_DOUBLE_UNARY(log1p, p);
    CHECK_DOUBLE_UNARY(log2, p);
    CHECK_UNARY(logb, x);
    CHECK_UNARY(sqrt, x);
    CHECK_UNARY(cbrt, x);
    CHECK_DOUBLE_UNARY(erf, x);
    CHECK_UNARY(erfc, x);
    CHECK_UNARY(lgamma, p);
    CHECK_UNARY(tgamma, u);
    CHECK_UNARY(ceil, x);
    CHECK_UNARY(floor, x);
    CHECK_UNARY(nearbyint, x);
    CHECK_UNARY(rint, x);
    CHECK_UNARY(round, x);
    CHECK_UNARY(trunc, x);
    CHECK_UNARY(fabs, x);
    CHECK_UNARY(abs, x);

    const V halves([&](int i) { return T(i) - T(N) / 2 + T(0.5); });
    CHECK_UNARY(round, halves);
    CHECK_UNARY(rint, halves);
    CHECK_UNARY(trunc, halves);
    CHECK_UNARY(floor, halves);
    CHECK_UNARY(ceil, halves);

    const V finite = sd::select(sd::isfinite(x), x, V(T(3)));
    const V moderate = sd::select(sd::fabs(finite) < T(1e9), finite, V(T(7)));
    check_lanes("ilogb",
                sd::ilogb(sd::select(x == T(0) || sd::isnan(x), V(T(1)), x)),
                [&](int i) {
                  const T v = x[i] == T(0) || x[i] != x[i] ? T(1) : x[i];
                  return std::ilogb(v);
                });
    check_lanes("lrint", sd::lrint(moderate),
                [&](int i) { return std::lrint(moderate[i]); });
    check_lanes("llrint", sd::llrint(moderate),
                [&](int i) { return std::llrint(moderate[i]); });
    check_lanes("lround", sd::lround(moderate),
                [&](int i) { return std::lround(moderate[i]); });
    check_lanes("llround", sd::llround(moderate),
                [&](int i) { return std::llround(moderate[i]); });
    check_lanes("fpclassify", sd::fpclassify(x),
                [&](int i) { return std::fpclassify(x[i]); });

    check_mask("isnan", sd::isnan(x), [&](int i) { return std::isnan(x[i]); });
    check_mask("isinf", sd::isinf(x), [&](int i) { return std::isinf(x[i]); });
    check_mask("isfinite", sd::isfinite(x),
               [&](int i) { return std::isfinite(x[i]); });
    check_mask("isnormal", sd::isnormal(x),
               [&](int i) { return std::isnormal(x[i]); });
    check_mask("signbit", sd::signbit(x),
               [&](int i) { return std::signbit(x[i]); });

    sd::rebind_t<int, V> exponents;
    const V mantissas = sd::frexp(finite, &exponents);
    for (int i = 0; i < N; ++i) {
      int e = 0;
      const T m = std::frexp(finite[i], &e);
      CHECK(same(mantissas[i], m));
      CHECK(exponents[i] == e);
    }
    const sd::rebind_t<int, V> shift([](int i) { return i % 7 - 3; });
    check_lanes("ldexp", sd::ldexp(finite, shift),
                [&](int i) { return std::ldexp(finite[i], shift[i]); });
    check_lanes("scalbn", sd::scalbn(finite, shift),
                [&](int i) { return std::scalbn(finite[i], shift[i]); });
    check_lanes("scalbln", sd::scalbln(finite, sd::rebind_t<long, V>(shift)),
                [&](int i) { return std::scalbln(finite[i], long(shift[i])); });
    V whole;
    const V fraction = sd::modf(x, &whole);
    for (int i = 0; i < N; ++i) {
      T w;
      CHECK(same(fraction[i], std::modf(x[i], &w)));
      CHECK(same(whole[i], w));
    }
  }
}

template <class T, int N> void math_binary() {
  using V = sd::vec<T, N>;
  rng<T> r(N * 37 + sizeof(T));
  for (int round = 0; round < 6; ++round) {
    const V x = math_input<V>(r, round);
    const V y = math_input<V>(r, round + 2);
    const V u = unit_input<V>(r) + T(2);

#define CHECK_BINARY(fn, a, b)                                                 \
  check_lanes(#fn, sd::fn(a, b), [&](int i) { return std::fn(a[i], b[i]); })
    CHECK_BINARY(pow, u, x);
    CHECK_BINARY(fmod, x, y);
    CHECK_BINARY(remainder, x, y);
    CHECK_BINARY(copysign, x, y);
    CHECK_BINARY(nextafter, x, y);
    CHECK_BINARY(fdim, x, y);
    CHECK_BINARY(hypot, x, y);
    CHECK_BINARY(atan2, x, y);
#undef CHECK_BINARY

    check_lanes("pow scalar exponent", sd::pow(u, T(2)),
                [&](int i) { return std::pow(u[i], T(2)); });
    check_lanes("pow scalar base", sd::pow(T(2), u),
                [&](int i) { return std::pow(T(2), u[i]); });

    // fmin and fmax may return either zero when both are zeros.
    const V fmin = sd::fmin(x, y);
    const V fmax = sd::fmax(x, y);
    for (int i = 0; i < N; ++i) {
      const T lo = std::fmin(x[i], y[i]);
      const T hi = std::fmax(x[i], y[i]);
      CHECK((same(fmin[i], lo) || (fmin[i] == T(0) && lo == T(0))));
      CHECK((same(fmax[i], hi) || (fmax[i] == T(0) && hi == T(0))));
    }

    check_mask("isgreater", sd::isgreater(x, y),
               [&](int i) { return std::isgreater(x[i], y[i]); });
    check_mask("isgreaterequal", sd::isgreaterequal(x, y),
               [&](int i) { return std::isgreaterequal(x[i], y[i]); });
    check_mask("isless", sd::isless(x, y),
               [&](int i) { return std::isless(x[i], y[i]); });
    check_mask("islessequal", sd::islessequal(x, y),
               [&](int i) { return std::islessequal(x[i], y[i]); });
    check_mask("islessgreater", sd::islessgreater(x, y),
               [&](int i) { return std::islessgreater(x[i], y[i]); });
    check_mask("isunordered", sd::isunordered(x, y),
               [&](int i) { return std::isunordered(x[i], y[i]); });

    sd::rebind_t<int, V> quotient;
    const V rem = sd::remquo(x, y, &quotient);
    for (int i = 0; i < N; ++i) {
      int q = 0;
      const T expected = std::remquo(x[i], y[i], &q);
      CHECK(same(rem[i], expected));
      if (expected == expected)
        CHECK((quotient[i] & 7) == (q & 7));
    }

    const V z = math_input<V>(r, round + 1);
    check_lanes("fma", sd::fma(x, y, z),
                [&](int i) { return std::fma(x[i], y[i], z[i]); });
    check_lanes("fma scalar", sd::fma(x, T(3), z),
                [&](int i) { return std::fma(x[i], T(3), z[i]); });
    check_lanes("hypot3", sd::hypot(x, y, z),
                [&](int i) { return std::hypot(x[i], y[i], z[i]); });
    check_lanes("lerp", sd::lerp(x, y, u - T(2)),
                [&](int i) { return std::lerp(x[i], y[i], u[i] - T(2)); });
  }

#if defined(__cpp_lib_math_special_functions)
  const V a([](int i) { return T(0.25) + T(i % 5) * T(0.125); });
  const V b([](int i) { return T(1.5) + T(i % 3) * T(0.5); });
  const sd::rebind_t<unsigned, V> n([](int i) { return unsigned(i % 4); });
  const sd::rebind_t<unsigned, V> m([](int i) { return unsigned(i % 2); });
  check_lanes("beta", sd::beta(b, b + T(1)),
              [&](int i) { return std::beta(b[i], b[i] + T(1)); });
  check_lanes("comp_ellint_1", sd::comp_ellint_1(a),
              [&](int i) { return std::comp_ellint_1(a[i]); });
  check_lanes("comp_ellint_2", sd::comp_ellint_2(a),
              [&](int i) { return std::comp_ellint_2(a[i]); });
  check_lanes("comp_ellint_3", sd::comp_ellint_3(a, a),
              [&](int i) { return std::comp_ellint_3(a[i], a[i]); });
  check_lanes("cyl_bessel_i", sd::cyl_bessel_i(a, b),
              [&](int i) { return std::cyl_bessel_i(a[i], b[i]); });
  check_lanes("cyl_bessel_j", sd::cyl_bessel_j(a, b),
              [&](int i) { return std::cyl_bessel_j(a[i], b[i]); });
  check_lanes("cyl_bessel_k", sd::cyl_bessel_k(a, b),
              [&](int i) { return std::cyl_bessel_k(a[i], b[i]); });
  check_lanes("cyl_neumann", sd::cyl_neumann(a, b),
              [&](int i) { return std::cyl_neumann(a[i], b[i]); });
  check_lanes("ellint_1", sd::ellint_1(a, b),
              [&](int i) { return std::ellint_1(a[i], b[i]); });
  check_lanes("ellint_2", sd::ellint_2(a, b),
              [&](int i) { return std::ellint_2(a[i], b[i]); });
  check_lanes("ellint_3", sd::ellint_3(a, a, b),
              [&](int i) { return std::ellint_3(a[i], a[i], b[i]); });
  check_lanes("expint", sd::expint(b),
              [&](int i) { return std::expint(b[i]); });
  check_lanes("riemann_zeta", sd::riemann_zeta(b),
              [&](int i) { return std::riemann_zeta(b[i]); });
  check_lanes("hermite", sd::hermite(n, a),
              [&](int i) { return std::hermite(n[i], a[i]); });
  check_lanes("laguerre", sd::laguerre(n, a),
              [&](int i) { return std::laguerre(n[i], a[i]); });
  check_lanes("legendre", sd::legendre(n, a),
              [&](int i) { return std::legendre(n[i], a[i]); });
  check_lanes("sph_bessel", sd::sph_bessel(n, b),
              [&](int i) { return std::sph_bessel(n[i], b[i]); });
  check_lanes("sph_neumann", sd::sph_neumann(n, b),
              [&](int i) { return std::sph_neumann(n[i], b[i]); });
  check_lanes("assoc_laguerre", sd::assoc_laguerre(n, m, a),
              [&](int i) { return std::assoc_laguerre(n[i], m[i], a[i]); });
  check_lanes("assoc_legendre", sd::assoc_legendre(n, m, a),
              [&](int i) { return std::assoc_legendre(n[i], m[i], a[i]); });
  check_lanes("sph_legendre", sd::sph_legendre(n, m, a),
              [&](int i) { return std::sph_legendre(n[i], m[i], a[i]); });
#endif
}

template <class T> void math_widths() {
  math_unary<T, 1>();
  math_unary<T, 3>();
  math_unary<T, 4>();
  math_unary<T, 8>();
  math_unary<T, 16>();
  math_unary<T, sd::vec<T>::size()>();
  math_binary<T, 1>();
  math_binary<T, 3>();
  math_binary<T, 4>();
  math_binary<T, 8>();
  math_binary<T, sd::vec<T>::size()>();
}

template <class T, int N> void integer_abs() {
  using V = sd::vec<T, N>;
  rng<T> r(N + sizeof(T));
  const V x([&](int i) {
    return i == 0 ? static_cast<T>(std::numeric_limits<T>::lowest() + 1)
                  : r.any();
  });
  check_lanes("integer abs", sd::abs(x),
              [&](int i) { return static_cast<T>(x[i] < 0 ? -x[i] : x[i]); });
}

} // namespace

TEST_CASE("simd math: functions match <cmath> lane by lane") {
  math_widths<float>();
  math_widths<double>();
}

TEST_CASE("simd math: integer abs") {
  integer_abs<signed char, 16>();
  integer_abs<short, 8>();
  integer_abs<int, 4>();
  integer_abs<int, 8>();
  integer_abs<long long, 4>();
  integer_abs<long long, 3>();
}
