#include "simd/simd_test_util.h"

#include "core/foundation/simd/linalg.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace simd_test;
namespace la = nx::simd::linalg;

namespace {

constexpr size_t sizes[] = {0, 1, 2, 15, 16, 17, 31, 32, 33, 100, 1000, 4099};

template <class T> std::vector<T> values(size_t n, uint32_t seed) {
  std::vector<T> v(n);
  for (T &e : v) {
    seed = seed * 1664525u + 1013904223u;
    e = T(int32_t(seed >> 8) - (1 << 23)) / T(1 << 22);
  }
  return v;
}

template <class T>
bool same_bits(const std::vector<T> &a, const std::vector<T> &b) {
  return a.size() == b.size() &&
         (a.empty() ||
          std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

// The documented order on plain scalars, one statement per operation so that
// no compiler fuses a multiply into an add.
template <class T, class F> T lane_order(size_t n, F term) {
  T lane[16] = {};
  for (size_t i = 0; i < n; ++i) {
    const T t = term(i);
    lane[i % 16] = lane[i % 16] + t;
  }
  for (int w = 8; w >= 1; w /= 2)
    for (int k = 0; k < w; ++k)
      lane[k] = lane[k] + lane[k + w];
  return lane[0];
}

template <class T> void check_reductions() {
  for (size_t n : sizes) {
    const auto a = values<T>(n, 1 + uint32_t(n)),
               b = values<T>(n, 7 + uint32_t(n));
    const T d = lane_order<T>(n, [&](size_t i) { return a[i] * b[i]; });
    CHECK(same(la::dot(a, b), d));
    CHECK(same(la::sum(a), lane_order<T>(n, [&](size_t i) { return a[i]; })));
    CHECK(same(la::sum_squares(a),
               lane_order<T>(n, [&](size_t i) { return a[i] * a[i]; })));
    long double exact = 0, magnitude = 0;
    for (size_t i = 0; i < n; ++i) {
      exact += (long double)a[i] * b[i];
      magnitude += std::fabs((long double)a[i] * b[i]);
    }
    const double bound =
        double(n) * std::numeric_limits<T>::epsilon() * double(magnitude);
    CHECK(std::fabs(double(la::dot(a, b)) - double(exact)) <= bound);
  }
}

template <class T> void check_max() {
  constexpr T inf = std::numeric_limits<T>::infinity();
  CHECK(la::max(std::span<const T>()) == -inf);
  for (size_t n : sizes) {
    if (n == 0)
      continue;
    auto a = values<T>(n, 3 + uint32_t(n));
    T want = -inf;
    for (T v : a)
      want = std::max(want, v);
    CHECK(la::max(a) == want);
    // All negative: the zeros that pad a partial block must not win.
    for (T &v : a)
      v = -std::fabs(v) - T(1);
    want = -inf;
    for (T v : a)
      want = std::max(want, v);
    CHECK(la::max(a) == want);
    a[n - 1] = std::numeric_limits<T>::quiet_NaN();
    CHECK(std::isnan(la::max(a)));
    a[n - 1] = inf;
    CHECK(la::max(a) == inf);
  }
}

template <class T> void check_elementwise() {
  for (size_t n : sizes) {
    const auto a = values<T>(n, 11 + uint32_t(n)),
               b = values<T>(n, 13 + uint32_t(n));
    const T s = T(0.375);
    std::vector<T> got(n), want(n);
    la::add(a, b, got);
    for (size_t i = 0; i < n; ++i)
      want[i] = a[i] + b[i];
    CHECK(same_bits(got, want));
    la::mul(a, b, got);
    for (size_t i = 0; i < n; ++i)
      want[i] = a[i] * b[i];
    CHECK(same_bits(got, want));
    la::scale(s, a, got);
    for (size_t i = 0; i < n; ++i)
      want[i] = s * a[i];
    CHECK(same_bits(got, want));
    got = b;
    la::axpy(s, a, got);
    for (size_t i = 0; i < n; ++i) {
      const T p = s * a[i];
      want[i] = b[i] + p;
    }
    CHECK(same_bits(got, want));
    // In place: the output is the input.
    got = a;
    la::scale(s, got, got);
    for (size_t i = 0; i < n; ++i)
      want[i] = s * a[i];
    CHECK(same_bits(got, want));
    got = a;
    la::add(got, b, got);
    for (size_t i = 0; i < n; ++i)
      want[i] = a[i] + b[i];
    CHECK(same_bits(got, want));
  }
}

template <class T> void check_gemv() {
  for (size_t rows : {size_t(0), size_t(1), size_t(3), size_t(4), size_t(9)})
    for (size_t cols : {size_t(1), size_t(16), size_t(17), size_t(100)}) {
      const auto w = values<T>(rows * cols, uint32_t(rows * 31 + cols));
      const auto x = values<T>(cols, uint32_t(cols + 5));
      const auto b = values<T>(rows, uint32_t(rows + 9));
      std::vector<T> y(rows), plain(rows);
      la::gemv(w, x, b, y);
      la::gemv(w, x, std::span<const T>(), plain);
      for (size_t r = 0; r < rows; ++r) {
        const T d = la::dot(std::span<const T>(w).subspan(r * cols, cols), x);
        CHECK(same(plain[r], d));
        CHECK(same(y[r], d + b[r]));
      }
      const auto xt = values<T>(rows, uint32_t(rows + 17));
      std::vector<T> yt(cols), want(cols, T(0));
      la::gemv_t(w, xt, yt);
      for (size_t r = 0; r < rows; ++r)
        for (size_t c = 0; c < cols; ++c) {
          const T p = xt[r] * w[r * cols + c];
          want[c] = want[c] + p;
        }
      CHECK(same_bits(yt, want));
    }
}

uint64_t fnv(uint64_t h, const void *p, size_t bytes) {
  const auto *c = static_cast<const unsigned char *>(p);
  for (size_t i = 0; i < bytes; ++i)
    h = (h ^ c[i]) * 1099511628211ull;
  return h;
}

} // namespace

TEST_CASE("simd linalg: sums run in the documented order") {
  check_reductions<float>();
  check_reductions<double>();
}

TEST_CASE("simd linalg: max, NaN and the partial block") {
  check_max<float>();
  check_max<double>();
}

TEST_CASE("simd linalg: element-wise kernels, in place too") {
  check_elementwise<float>();
  check_elementwise<double>();
}

TEST_CASE("simd linalg: gemv rows are dot products, gemv_t adds rows") {
  check_gemv<float>();
  check_gemv<double>();
}

TEST_CASE("simd linalg: every backend computes the same bits") {
  uint64_t h = 1469598103934665603ull;
  for (size_t n : sizes) {
    const auto a = values<float>(n, 21 + uint32_t(n)),
               b = values<float>(n, 23 + uint32_t(n));
    const float r[] = {la::dot(a, b), la::sum(a), la::sum_squares(a)};
    h = fnv(h, r, sizeof r);
    const auto w = values<float>(n * 7, uint32_t(n + 29));
    std::vector<float> y(7), yt(n);
    la::gemv(w, a, std::span<const float>(), y);
    la::gemv_t(w, values<float>(7, 31), yt);
    h = fnv(h, y.data(), y.size() * sizeof(float));
    h = fnv(h, yt.data(), yt.size() * sizeof(float));
  }
  if (h != 0xae72efe31a1abaabull)
    std::printf("linalg hashes to %016llx\n", (unsigned long long)h);
  CHECK(h == 0xae72efe31a1abaabull);
}
