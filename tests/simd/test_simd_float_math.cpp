// The Windows headers define near, far and small; the simd headers must
// survive being included after them.
#if defined(_WIN32)
#undef WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "core/foundation/simd/simd.h"

#undef near
#undef far
#undef small

#include "simd/simd_test_util.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>

using namespace simd_test;

namespace {

using V = sd::vec<float>;
constexpr float inf = std::numeric_limits<float>::infinity();
constexpr float qnan = std::numeric_limits<float>::quiet_NaN();

struct function {
  const char *name;
  V (*f)(const V &);
  double (*ref)(double);
  double bound;
};

double sigmoid_ref(double x) { return 1.0 / (1.0 + std::exp(-x)); }

#define FN(n) [](const V &v) { return sd::n(v); }
// The bounds are the exhaustive maxima over all floats, rounded up.
const function accurate[] = {
    {"exp", FN(exp), [](double x) { return std::exp(x); }, 1.0},
    {"exp2", FN(exp2), [](double x) { return std::exp2(x); }, 1.5},
    {"expm1", FN(expm1), [](double x) { return std::expm1(x); }, 1.5},
    {"log", FN(log), [](double x) { return std::log(x); }, 1.0},
    {"log2", FN(log2), [](double x) { return std::log2(x); }, 2.0},
    {"log1p", FN(log1p), [](double x) { return std::log1p(x); }, 1.5},
    {"tanh", FN(tanh), [](double x) { return std::tanh(x); }, 1.5},
    {"erf", FN(erf), [](double x) { return std::erf(x); }, 1.5},
    {"sigmoid", FN(sigmoid), sigmoid_ref, 2.5},
};
const function fast[] = {
    {"fast_exp", FN(fast_exp), [](double x) { return std::exp(x); }, 1e-5},
    {"fast_tanh", FN(fast_tanh), [](double x) { return std::tanh(x); }, 1e-5},
    {"fast_sigmoid", FN(fast_sigmoid), sigmoid_ref, 1e-5},
    {"fast_erf", FN(fast_erf), [](double x) { return std::erf(x); }, 3e-5},
};
#undef FN

double ulp_of(double ref) {
  const float f = static_cast<float>(std::fabs(ref));
  return double(std::nextafter(f, inf)) - double(f);
}

/// Every 9973rd float bit pattern, all signs and exponents, in the same
/// order at every width; each(in, count) sees count valid lanes.
constexpr uint64_t sweep_size = (uint64_t(1) << 32) / 9973;
template <class F> void sweep(F &&each) {
  alignas(64) float in[V::size()];
  for (uint64_t j = 0; j < sweep_size; j += V::size()) {
    for (int k = 0; k < int(V::size()); ++k)
      in[k] = std::bit_cast<float>(uint32_t((j + uint64_t(k)) * 9973));
    each(in, int(std::min<uint64_t>(V::size(), sweep_size - j)));
  }
}

/// The error of y against e in ulps, or relative; specials must match. The
/// relative bound covers normal results only: the fast forms may flush a
/// subnormal one to 0 and overflow within the bound of FLT_MAX.
bool within(float x, float y, double e, double bound, bool relative) {
  constexpr double lowest = std::numeric_limits<float>::min();
  constexpr double highest = std::numeric_limits<float>::max();
  if (relative &&
      (std::fabs(e) < lowest || std::fabs(e) > highest / (1 + bound)))
    return true;
  if (!std::isfinite(e) ||
      std::fabs(e) > double(std::numeric_limits<float>::max()) ||
      !std::isfinite(y)) {
    const float ef = static_cast<float>(e);
    if ((std::isnan(ef) && std::isnan(y)) || ef == y)
      return true;
    std::printf("  x = %.9g: %.9g, want %.9g\n", x, y, e);
    return false;
  }
  if (e == 0) {
    if (y == 0)
      return true;
    std::printf("  x = %.9g: %.9g, want 0\n", x, y);
    return false;
  }
  const double err = relative ? std::fabs((double(y) - e) / e)
                              : std::fabs(double(y) - e) / ulp_of(e);
  if (err <= bound)
    return true;
  std::printf("  x = %.9g: %.9g, want %.9g (%.3g %s)\n", x, y, e, err,
              relative ? "relative" : "ulp");
  return false;
}

int check_sweep(const function &fn, bool relative) {
  int failures = 0;
  sweep([&](const float *in, int count) {
    alignas(64) float out[V::size()];
    sd::unchecked_store(fn.f(sd::unchecked_load<V>(in, V::size())), out,
                        V::size());
    for (int k = 0; k < count; ++k)
      if (failures < 5 &&
          !within(in[k], out[k], fn.ref(double(in[k])), fn.bound, relative) &&
          ++failures == 1)
        std::printf("%s:\n", fn.name);
  });
  return failures;
}

float one(const function &fn, float x) { return fn.f(V(x))[0]; }

/// f at width N lane by lane equals the native-width g.
template <int N, class F>
bool same_as_native(const char *name, F f, V (*g)(const V &)) {
  bool ok = true;
  int round = 0;
  sweep([&](const float *in, int count) {
    if (!ok || round++ % 64 != 0)
      return;
    const sd::vec<float, N> w([&](int i) { return in[i % count]; });
    const auto got = f(w);
    for (int i = 0; i < N && ok; ++i)
      if (!same(got[i], g(V(w[i]))[0])) {
        std::printf("%s width %d: x = %.9g\n", name, N, w[i]);
        ok = false;
      }
  });
  return ok;
}

#define CHECK_WIDTHS(n)                                                        \
  do {                                                                         \
    const auto f = [](const auto &v) { return sd::n(v); };                     \
    V (*g)(const V &) = [](const V &v) { return sd::n(v); };                   \
    CHECK(same_as_native<1>(#n, f, g));                                        \
    CHECK(same_as_native<3>(#n, f, g));                                        \
    CHECK(same_as_native<8>(#n, f, g));                                        \
    CHECK(same_as_native<16>(#n, f, g));                                       \
  } while (false)

} // namespace

TEST_CASE("simd float math: within the bound over the whole float range") {
  for (const function &fn : accurate)
    CHECK(check_sweep(fn, false) == 0);
}

TEST_CASE("simd float math: fast forms within their relative bound") {
  for (const function &fn : fast)
    CHECK(check_sweep(fn, true) == 0);
}

TEST_CASE("simd float math: special values") {
  const function &exp = accurate[0], &exp2 = accurate[1], &expm1 = accurate[2];
  const function &log = accurate[3], &log2 = accurate[4], &log1p = accurate[5];
  const function &tanh = accurate[6], &erf = accurate[7], &sig = accurate[8];
  for (const function &fn : accurate)
    CHECK(std::isnan(one(fn, qnan)));
  for (const function &fn : fast)
    CHECK(std::isnan(one(fn, qnan)));

  CHECK(one(exp, 0.f) == 1.f);
  CHECK(one(exp, inf) == inf);
  CHECK(one(exp, -inf) == 0.f);
  CHECK(one(exp, 88.8f) == inf);
  CHECK(one(exp, 88.72f) < inf);
  CHECK(one(exp, -103.98f) == 0.f);
  CHECK(one(exp, -103.f) > 0.f);
  CHECK(one(exp2, 10.f) == 1024.f);
  CHECK(one(exp2, -149.f) == 0x1p-149f);
  CHECK(one(exp2, 128.f) == inf);
  CHECK(one(exp2, -inf) == 0.f);
  CHECK(std::signbit(one(expm1, -0.f)));
  CHECK(one(expm1, -inf) == -1.f);
  CHECK(one(expm1, 1e-30f) == 1e-30f);
  CHECK(one(expm1, 88.7f) < inf);
  CHECK(one(expm1, inf) == inf);

  CHECK(one(log, 1.f) == 0.f);
  CHECK(one(log, 0.f) == -inf);
  CHECK(one(log, -0.f) == -inf);
  CHECK(one(log, inf) == inf);
  CHECK(std::isnan(one(log, -1.f)));
  CHECK(one(log, 0x1p-149f) ==
        nxtest::Approx(std::log(0x1p-149)).epsilon(1e-6));
  CHECK(one(log2, 0x1p-140f) == -140.f);
  CHECK(one(log2, 0x1p100f) == 100.f);
  CHECK(std::signbit(one(log1p, -0.f)));
  CHECK(one(log1p, -1.f) == -inf);
  CHECK(std::isnan(one(log1p, -2.f)));
  CHECK(one(log1p, inf) == inf);
  CHECK(one(log1p, 1e-30f) == 1e-30f);

  CHECK(std::signbit(one(tanh, -0.f)));
  CHECK(one(tanh, inf) == 1.f);
  CHECK(one(tanh, -inf) == -1.f);
  CHECK(one(tanh, 1e-30f) == 1e-30f);
  CHECK(std::signbit(one(erf, -0.f)));
  CHECK(one(erf, 3.9192061f) == 1.f);
  CHECK(one(erf, -inf) == -1.f);
  CHECK(one(erf, 1e-30f) ==
        nxtest::Approx(1e-30 * 1.1283791670955126).epsilon(1e-6));
  CHECK(one(sig, 0.f) == 0.5f);
  CHECK(one(sig, inf) == 1.f);
  CHECK(one(sig, -inf) == 0.f);
  CHECK(one(sig, -200.f) == 0.f);

  CHECK(one(fast[0], -inf) == 0.f);
  CHECK(one(fast[0], inf) == inf);
  CHECK(one(fast[1], -inf) == -1.f);
  CHECK(one(fast[2], -inf) == 0.f);
  CHECK(one(fast[3], inf) == 1.f);
}

TEST_CASE("simd float math: every width gives the native width's bits") {
  CHECK_WIDTHS(exp);
  CHECK_WIDTHS(expm1);
  CHECK_WIDTHS(log1p);
  CHECK_WIDTHS(tanh);
  CHECK_WIDTHS(erf);
}

TEST_CASE("simd float math: double still runs <cmath>") {
  rng<double> r(77);
  const sd::vec<double, 4> x([&](int) { return r.any(); });
  const auto p = sd::fabs(x);
  check_lanes("exp", sd::exp(x), [&](int i) { return std::exp(x[i]); });
  check_lanes("log", sd::log(p), [&](int i) { return std::log(p[i]); });
  check_lanes("tanh", sd::tanh(x), [&](int i) { return std::tanh(x[i]); });
  check_lanes("erf", sd::erf(x), [&](int i) { return std::erf(x[i]); });
}

// Only +, -, *, / and exact conversions: a different hash means a backend that
// rounds in another order or fuses a multiply-add.
TEST_CASE("simd float math: every backend computes the same bits") {
  constexpr uint64_t expected[std::size(accurate)] = {
      0x046a55e3fdb6ee6eull, 0x932e05af45b2a36bull, 0x5e8f188d29d1f9bfull,
      0x789ef2617e709a72ull, 0xbe18e42dfaac65d2ull, 0x9eb2fc2a2a7490b0ull,
      0x9566424d1ab1c13cull, 0x334a4281c3cdc115ull, 0xcf2670695b9740fbull};
  for (size_t i = 0; i < std::size(accurate); ++i) {
    const function &fn = accurate[i];
    uint64_t h = 1469598103934665603ull;
    sweep([&](const float *in, int count) {
      const V y = fn.f(sd::unchecked_load<V>(in, V::size()));
      for (int k = 0; k < count; ++k) {
        // Every NaN is the same NaN to this hash.
        const uint32_t v =
            y[k] != y[k] ? 0x7fc00000u : std::bit_cast<uint32_t>(y[k]);
        h = (h ^ v) * 1099511628211ull;
      }
    });
    if (h != expected[i])
      std::printf("%s hashes to %016llx\n", fn.name, (unsigned long long)h);
    CHECK(h == expected[i]);
  }
}
