#include "simd/simd_test_util.h"

#include "core/foundation/simd/nn.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

using namespace simd_test;
namespace nn = nx::simd::nn;
using nn::precision;

namespace {

constexpr size_t sizes[] = {0,  1,  2,  3,  5,  7,   8,   9,   15,   16,  17,
                            23, 31, 32, 33, 40, 255, 256, 257, 1000, 4099};
constexpr precision tiers[] = {precision::exact, precision::fast};
constexpr float eps = std::numeric_limits<float>::epsilon();
constexpr float inf = std::numeric_limits<float>::infinity();
constexpr float qnan = std::numeric_limits<float>::quiet_NaN();

// Uniform in [-scale, scale).
std::vector<float> values(size_t n, uint32_t seed, float scale) {
  std::vector<float> v(n);
  for (float &e : v) {
    seed = seed * 1664525u + 1013904223u;
    e = scale * float(int32_t(seed >> 8) - (1 << 23)) / float(1 << 23);
  }
  return v;
}

bool same_bits(const std::vector<float> &a, const std::vector<float> &b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * 4) == 0);
}
bool same_bits(float a, float b) { return std::memcmp(&a, &b, 4) == 0; }

// linalg's 16-lane order on plain scalars.
template <class F> float lane_order(size_t n, F term) {
  float lane[16] = {};
  for (size_t i = 0; i < n; ++i) {
    const float t = term(i);
    lane[i % 16] = lane[i % 16] + t;
  }
  for (int w = 8; w >= 1; w /= 2)
    for (int k = 0; k < w; ++k)
      lane[k] = lane[k] + lane[k + w];
  return lane[0];
}

// One element through the kernel the tier names.
float exp_of(float x, precision p) {
  const sd::vec<float> v(x);
  return (p == precision::exact ? sd::exp(v) : sd::fast_exp(v))[0];
}
float log_of(float x) { return sd::log(sd::vec<float>(x))[0]; }

float max_of(const std::vector<float> &x) {
  float m = -inf;
  for (float v : x)
    m = v > m ? v : m;
  return m;
}

using kernel = void (*)(std::span<const float>, std::span<float>, precision);

std::vector<float> run(kernel k, const std::vector<float> &x, precision p) {
  std::vector<float> out(x.size(), qnan);
  k(x, out, p);
  return out;
}

void report(const char *what, size_t n, size_t i, double got, double want) {
  std::printf("%s n %zu at %zu: %.9g, want %.9g\n", what, n, i, got, want);
}

} // namespace

TEST_CASE("simd nn: softmax and logsumexp sum in the documented order") {
  for (precision p : tiers)
    for (size_t n : sizes) {
      const auto x = values(n, 11 + uint32_t(n), 16.f);
      if (n == 0) {
        CHECK(nn::logsumexp(x, p) == -inf);
        continue;
      }
      const float m = max_of(x);
      std::vector<float> e(n);
      for (size_t i = 0; i < n; ++i)
        e[i] = exp_of(x[i] - m, p);
      const float s = lane_order(n, [&](size_t i) { return e[i]; });
      std::vector<float> soft(n), logsoft(n);
      const float l = log_of(s);
      for (size_t i = 0; i < n; ++i) {
        soft[i] = e[i] / s;
        const float d = x[i] - m;
        logsoft[i] = d - l;
      }
      CHECK(same_bits(run(nn::softmax, x, p), soft));
      CHECK(same_bits(run(nn::log_softmax, x, p), logsoft));
      CHECK(same_bits(nn::logsumexp(x, p), m + l));
    }
}

TEST_CASE("simd nn: softmax and logsumexp stay near the exact values") {
  for (precision p : tiers)
    for (size_t n : sizes) {
      if (n == 0)
        continue;
      const auto x = values(n, 13 + uint32_t(n), 16.f);
      double m = -INFINITY, s = 0;
      for (float v : x)
        m = std::fmax(m, v);
      for (float v : x)
        s += std::exp(double(v) - m);
      const double lse = m + std::log(s);
      // The sum's error grows with each lane's n / 16 additions.
      const double grow =
          (16.0 + double(n) / 16) * eps + (p == precision::fast ? 1e-5 : 0.0);
      const auto soft = run(nn::softmax, x, p);
      const auto logsoft = run(nn::log_softmax, x, p);
      for (size_t i = 0; i < n; ++i) {
        const double want = std::exp(double(x[i]) - m) / s;
        if (std::fabs(soft[i] - want) > grow * want)
          report("softmax", n, i, soft[i], want);
        CHECK(std::fabs(soft[i] - want) <= grow * want);
        const double lwant = double(x[i]) - lse;
        const double tol = grow * (1 + std::fabs(lwant) + std::fabs(lse));
        CHECK(std::fabs(logsoft[i] - lwant) <= tol);
      }
      CHECK(std::fabs(nn::logsumexp(x, p) - lse) <=
            grow * (1 + std::fabs(lse)));
    }
}

TEST_CASE("simd nn: softmax at infinities, NaN and large inputs") {
  for (precision p : tiers) {
    const std::vector<float> masked = {-inf, 0.f, -inf, 1.f};
    const auto soft = run(nn::softmax, masked, p);
    CHECK(soft[0] == 0.f);
    CHECK(soft[2] == 0.f);
    CHECK(std::fabs(soft[1] + soft[3] - 1.f) <= 2 * eps);
    const auto logsoft = run(nn::log_softmax, masked, p);
    CHECK(logsoft[0] == -inf);
    CHECK(std::isfinite(logsoft[1]));

    const std::vector<float> huge = {1000.f, 1000.f, 1000.f, 1000.f};
    for (float v : run(nn::softmax, huge, p))
      CHECK(v == 0.25f);
    CHECK(std::fabs(nn::logsumexp(huge, p) - (1000.f + std::log(4.f))) <=
          1000.f * eps);

    for (const std::vector<float> &bad :
         {std::vector<float>{-inf, -inf, -inf}, std::vector<float>{inf, 0.f},
          std::vector<float>{0.f, qnan, 1.f}}) {
      for (float v : run(nn::softmax, bad, p))
        CHECK(std::isnan(v));
      for (float v : run(nn::log_softmax, bad, p))
        CHECK(std::isnan(v));
    }
    CHECK(nn::logsumexp(std::vector<float>{-inf, -inf}, p) == -inf);
    CHECK(nn::logsumexp(std::vector<float>{inf, 0.f}, p) == inf);
    CHECK(std::isnan(nn::logsumexp(std::vector<float>{0.f, qnan}, p)));
  }
}

TEST_CASE("simd nn: norms sum in the documented order") {
  for (size_t n : sizes) {
    const auto x = values(n, 17 + uint32_t(n), 3.f);
    const auto g = values(n, 19 + uint32_t(n), 2.f);
    const auto b = values(n, 23 + uint32_t(n), 1.f);
    const float e = 1e-5f;
    std::vector<float> out(n, qnan), want(n);

    const float ss = lane_order(n, [&](size_t i) { return x[i] * x[i]; });
    const float r = 1.f / std::sqrt(ss / float(n) + e);
    nn::rmsnorm(x, g, e, out);
    for (size_t i = 0; i < n; ++i) {
      const float t = x[i] * r;
      want[i] = t * g[i];
    }
    CHECK(same_bits(out, want));
    nn::rmsnorm(x, {}, e, out);
    for (size_t i = 0; i < n; ++i)
      want[i] = x[i] * r;
    CHECK(same_bits(out, want));

    const nn::moments mo = nn::layernorm(x, g, b, e, out);
    if (n == 0) {
      CHECK(mo.mean == 0.f);
      CHECK(mo.rstd == 0.f);
      continue;
    }
    const float mean = lane_order(n, [&](size_t i) { return x[i]; }) / float(n);
    const float var = lane_order(n,
                                 [&](size_t i) {
                                   const float d = x[i] - mean;
                                   return d * d;
                                 }) /
                      float(n);
    const float rstd = 1.f / std::sqrt(var + e);
    CHECK(same_bits(mo.mean, mean));
    CHECK(same_bits(mo.rstd, rstd));
    std::vector<float> plain(n), scaled(n), shifted(n), both(n);
    for (size_t i = 0; i < n; ++i) {
      const float d = x[i] - mean;
      plain[i] = d * rstd;
      scaled[i] = plain[i] * g[i];
      shifted[i] = plain[i] + b[i];
      both[i] = scaled[i] + b[i];
    }
    CHECK(same_bits(out, both));
    nn::layernorm(x, {}, {}, e, out);
    CHECK(same_bits(out, plain));
    nn::layernorm(x, g, {}, e, out);
    CHECK(same_bits(out, scaled));
    nn::layernorm(x, {}, b, e, out);
    CHECK(same_bits(out, shifted));
  }
}

TEST_CASE("simd nn: norms stay near the exact values") {
  for (size_t n : sizes) {
    if (n == 0)
      continue;
    // Offset, so that the variance sums from the mean, not from 0.
    auto x = values(n, 29 + uint32_t(n), 1.f);
    for (float &v : x)
      v += 100.f;
    const auto g = values(n, 31 + uint32_t(n), 2.f);
    const auto b = values(n, 37 + uint32_t(n), 1.f);
    const float e = 1e-6f;
    double mean = 0, ss = 0;
    for (float v : x) {
      mean += v;
      ss += double(v) * v;
    }
    mean /= double(n);
    double var = 0;
    for (float v : x)
      var += (v - mean) * (v - mean);
    var /= double(n);
    const double rstd = 1 / std::sqrt(var + e);
    const double rms = 1 / std::sqrt(ss / double(n) + e);
    const double grow = (16.0 + double(n) / 16) * eps;

    std::vector<float> out(n);
    const nn::moments mo = nn::layernorm(x, g, b, e, out);
    // The mean's error shifts every x[i] - mean by as much.
    const double em = grow * std::fabs(mean);
    CHECK(std::fabs(mo.mean - mean) <= em);
    CHECK(std::fabs(mo.rstd - rstd) <= grow * rstd);
    for (size_t i = 0; i < n; ++i) {
      const double want = (x[i] - mean) * rstd * g[i] + b[i];
      const double tol = 4 * eps * (std::fabs(want) + std::fabs(b[i])) +
                         (grow * std::fabs((x[i] - mean) * rstd) + em * rstd) *
                             std::fabs(g[i]);
      if (std::fabs(out[i] - want) > tol)
        report("layernorm", n, i, out[i], want);
      CHECK(std::fabs(out[i] - want) <= tol);
    }

    nn::rmsnorm(x, g, e, out);
    for (size_t i = 0; i < n; ++i) {
      const double want = x[i] * rms * g[i];
      CHECK(std::fabs(out[i] - want) <= (grow + 4 * eps) * std::fabs(want));
    }
  }
}

TEST_CASE("simd nn: a constant row normalizes to 0") {
  const std::vector<float> x(37, 5.f);
  std::vector<float> out(37, qnan);
  const nn::moments mo = nn::layernorm(x, {}, {}, 1e-5f, out);
  CHECK(mo.mean == 5.f);
  CHECK(mo.rstd == 1.f / std::sqrt(1e-5f));
  for (float v : out)
    CHECK(v == 0.f);
}

TEST_CASE("simd nn: activations stay near the exact values") {
  for (precision p : tiers) {
    const double fast = p == precision::fast ? 3e-5 : 0.0;
    for (size_t n : sizes) {
      const auto x = values(n, 41 + uint32_t(n), 8.f);
      const auto u = values(n, 43 + uint32_t(n), 2.f);
      const auto gelu = run(nn::gelu, x, p);
      const auto gelu_tanh = run(nn::gelu_tanh, x, p);
      const auto silu = run(nn::silu, x, p);
      std::vector<float> swiglu(n, qnan);
      nn::swiglu(x, u, swiglu, p);
      for (size_t i = 0; i < n; ++i) {
        const double v = x[i];
        // 1 + erf and 1 + tanh lose their relative precision as they near 0,
        // which leaves an error of a few eps times v / 2.
        const double g = 0.5 * v * (1 + std::erf(v / std::sqrt(2.0)));
        const double gt = 0.5 * v *
                          (1 + std::tanh(std::sqrt(2 / 3.14159265358979323846) *
                                         (v + 0.044715 * v * v * v)));
        const double s = v / (1 + std::exp(-v));
        const double near_zero = (4 * eps + fast) * std::fabs(v);
        if (std::fabs(gelu[i] - g) > 4 * eps * std::fabs(g) + near_zero)
          report("gelu", n, i, gelu[i], g);
        CHECK(std::fabs(gelu[i] - g) <= 4 * eps * std::fabs(g) + near_zero);
        CHECK(std::fabs(gelu_tanh[i] - gt) <=
              4 * eps * std::fabs(gt) + near_zero);
        CHECK(std::fabs(silu[i] - s) <= (6 * eps + fast) * std::fabs(s));
        CHECK(std::fabs(swiglu[i] - s * u[i]) <=
              (8 * eps + fast) * std::fabs(s * u[i]));
      }
    }
  }
}

TEST_CASE("simd nn: in place gives the same bits, fast differs") {
  for (size_t n : {size_t(1), size_t(33), size_t(1000)}) {
    const auto x = values(n, 47 + uint32_t(n), 6.f);
    for (kernel k : {kernel(nn::softmax), kernel(nn::log_softmax),
                     kernel(nn::gelu), kernel(nn::gelu_tanh), kernel(nn::silu)})
      for (precision p : tiers) {
        auto in_place = x;
        k(in_place, in_place, p);
        CHECK(same_bits(in_place, run(k, x, p)));
        // log_softmax's tier reaches one log of a sum, which often rounds
        // the same.
        if (n == 1000 && k != kernel(nn::log_softmax))
          CHECK(!same_bits(run(k, x, precision::exact),
                           run(k, x, precision::fast)));
      }
    auto in_place = x;
    nn::layernorm(in_place, {}, {}, 1e-5f, in_place);
    std::vector<float> out(n);
    nn::layernorm(x, {}, {}, 1e-5f, out);
    CHECK(same_bits(in_place, out));
    in_place = x;
    nn::rmsnorm(in_place, {}, 1e-5f, in_place);
    nn::rmsnorm(x, {}, 1e-5f, out);
    CHECK(same_bits(in_place, out));
  }
}

TEST_CASE("simd nn: every backend computes the same bits") {
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&](const void *p, size_t bytes) {
    const auto *c = static_cast<const unsigned char *>(p);
    for (size_t i = 0; i < bytes; ++i)
      h = (h ^ c[i]) * 1099511628211ull;
  };
  const auto mix_all = [&](const std::vector<float> &v) {
    mix(v.data(), v.size() * 4);
  };
  for (size_t n : sizes) {
    const auto x = values(n, 53 + uint32_t(n), 8.f);
    const auto g = values(n, 59 + uint32_t(n), 2.f);
    for (kernel k : {kernel(nn::softmax), kernel(nn::log_softmax),
                     kernel(nn::gelu), kernel(nn::gelu_tanh), kernel(nn::silu)})
      mix_all(run(k, x, precision::exact));
    const float l = nn::logsumexp(x);
    mix(&l, 4);
    std::vector<float> out(n);
    nn::swiglu(x, g, out);
    mix_all(out);
    nn::rmsnorm(x, g, 1e-5f, out);
    mix_all(out);
    const nn::moments mo = nn::layernorm(x, g, g, 1e-5f, out);
    mix_all(out);
    mix(&mo.mean, 4);
    mix(&mo.rstd, 4);
  }
  if (h != 0x0f43229e628cb18full)
    std::printf("nn hashes to %016llx\n", (unsigned long long)h);
  CHECK(h == 0x0f43229e628cb18full);
}
