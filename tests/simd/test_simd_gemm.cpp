#include "simd/simd_test_util.h"

#include "core/foundation/simd/linalg.h"
#include "core/foundation/threading/thread_pool.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace simd_test;
namespace la = nx::simd::linalg;

namespace {

struct shape {
  size_t m, n, k, lda_pad, ldb_pad, ldc_pad;
};

// Past every block size (kc 256, mc 96, nc 2048) and tile edge.
constexpr shape shapes[] = {
    {0, 5, 3, 0, 0, 0},    {5, 0, 3, 0, 0, 0},     {5, 3, 0, 0, 0, 0},
    {1, 1, 1, 0, 0, 0},    {7, 9, 5, 2, 3, 1},     {6, 16, 1, 0, 0, 0},
    {4, 8, 2, 0, 0, 0},    {13, 35, 300, 1, 2, 3}, {100, 40, 20, 0, 0, 5},
    {3, 2100, 7, 0, 1, 0}, {97, 17, 513, 4, 0, 0}};

constexpr float sentinel = 1234.5f;

std::vector<float> values(size_t n, uint32_t seed) {
  std::vector<float> v(n);
  for (float &e : v) {
    seed = seed * 1664525u + 1013904223u;
    e = float(int32_t(seed >> 8) - (1 << 23)) / float(1 << 22);
  }
  return v;
}

struct problem {
  shape s;
  size_t lda, ldb, ldc;
  std::vector<float> a, b, c0;

  explicit problem(const shape &sh, uint32_t seed)
      : s(sh), lda(sh.k + sh.lda_pad), ldb(sh.n + sh.ldb_pad),
        ldc(sh.n + sh.ldc_pad), a(values(sh.m * lda, seed)),
        b(values(sh.k * ldb, seed + 1)), c0(values(sh.m * ldc, seed + 2)) {
    for (size_t i = 0; i < s.m; ++i)
      for (size_t j = s.n; j < ldc; ++j)
        c0[i * ldc + j] = sentinel;
  }

  std::vector<float> run(const la::gemm_options &o) const {
    std::vector<float> c = c0;
    la::gemm(s.m, s.n, s.k, a.data(), lda, b.data(), ldb, c.data(), ldc, o);
    return c;
  }

  // The documented order on scalars, one statement per operation.
  std::vector<float> reference(bool accumulate, bool fused) const {
    std::vector<float> c = c0;
    for (size_t i = 0; i < s.m; ++i)
      for (size_t j = 0; j < s.n; ++j) {
        float acc = accumulate ? c0[i * ldc + j] : 0.f;
        for (size_t p = 0; p < s.k; ++p) {
          if (fused) {
            acc = std::fma(a[i * lda + p], b[p * ldb + j], acc);
          } else {
            const float t = a[i * lda + p] * b[p * ldb + j];
            acc = acc + t;
          }
        }
        c[i * ldc + j] = acc;
      }
    return c;
  }
};

bool same_bits(const std::vector<float> &x, const std::vector<float> &y) {
  return x.size() == y.size() &&
         (x.empty() || std::memcmp(x.data(), y.data(), x.size() * 4) == 0);
}

} // namespace

TEST_CASE("simd gemm: every kernel sums in the documented order") {
  uint32_t seed = 1;
  for (const shape &s : shapes) {
    const problem p(s, seed += 10);
    for (bool accumulate : {false, true}) {
      const auto want = p.reference(accumulate, false);
      for (la::gemm_kernel kernel :
           {la::gemm_kernel::best, la::gemm_kernel::baseline}) {
        const auto got = p.run({.accumulate = accumulate, .kernel = kernel});
        if (!same_bits(got, want))
          std::printf("%zu x %zu x %zu, accumulate %d, kernel %d\n", s.m, s.n,
                      s.k, int(accumulate), int(kernel));
        CHECK(same_bits(got, want));
      }
    }
  }
}

TEST_CASE("simd gemm: fused sums are the fma chain or the plain one") {
  uint32_t seed = 3;
  for (const shape &s : shapes) {
    const problem p(s, seed += 10);
    const auto fma = p.reference(false, true),
               plain = p.reference(false, false);
    for (la::gemm_kernel kernel :
         {la::gemm_kernel::best, la::gemm_kernel::baseline}) {
      const auto got = p.run({.fused = true, .kernel = kernel});
      CHECK((same_bits(got, fma) || same_bits(got, plain)));
    }
  }
}

TEST_CASE("simd gemm: a pool splits the rows without changing a bit") {
  nx::thread_pool::config cfg;
  cfg.thread_count = 4;
  cfg.name = "nx-gemm-test";
  nx::thread_pool pool(cfg);
  // Past the 2^26 multiply-adds below which gemm stays on the caller.
  const problem p(shape{400, 420, 400, 1, 0, 2}, 77);
  for (bool fused : {false, true})
    CHECK(same_bits(p.run({.fused = fused, .pool = &pool}),
                    p.run({.fused = fused})));
}

TEST_CASE("simd gemm: every backend computes the same bits") {
  uint64_t h = 1469598103934665603ull;
  uint32_t seed = 5;
  for (const shape &s : shapes) {
    const auto c = problem(s, seed += 10).run({});
    const auto *bytes = reinterpret_cast<const unsigned char *>(c.data());
    for (size_t i = 0; i < c.size() * 4; ++i)
      h = (h ^ bytes[i]) * 1099511628211ull;
  }
  if (h != 0xdd42147e9ebb885dull)
    std::printf("gemm hashes to %016llx\n", (unsigned long long)h);
  CHECK(h == 0xdd42147e9ebb885dull);
}
