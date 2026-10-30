#include "core/foundation/core/foundation.h"
#include "core/foundation/simd/gemm_kernel.h"
#include "core/foundation/simd/linalg.h"
#include "core/foundation/threading/thread_pool.h"

#include <algorithm>
#include <cstring>

#if NX_ARCH_X64
#if defined(NX_COMPILER_MSVC) && !defined(NX_COMPILER_CLANG)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace nx::simd::linalg {

namespace gemm_detail {

namespace {

using V = vec<float>;
constexpr int W = int(V::size());
// SSE has 16 registers: 4 x 2 accumulators leave room for the operands.
// NEON's 32 fit 6 x 2.
#if NX_ARCH_AARCH64
constexpr int base_mr = 6;
#else
constexpr int base_mr = 4;
#endif
constexpr int base_nr = 2 * W;
static_assert(base_mr <= 6);

template <bool Fused>
NX_FORCE_INLINE V madd(const V &a, const V &b, const V &c) {
#if defined(NX_SIMD_HAS_FMA)
  if constexpr (Fused)
    return simd::fma(a, b, c);
  else
#endif
    return c + a * b;
}

NX_FORCE_INLINE V load(const float *p) { return unchecked_load<V>(p, W); }

// Named accumulators: MSVC keeps an array of them in memory.
template <bool Fused>
void base_micro(size_t kc, const float *ap, const float *bp, float *c,
                size_t ldc, bool load_c) {
  constexpr int mr = base_mr;
  V c00, c01, c10, c11, c20, c21, c30, c31, c40, c41, c50, c51;
#define NX_GEMM_ROWS(X) X(0) X(1) X(2) X(3) X(4) X(5)
#define NX_GEMM_INIT(r)                                                        \
  if constexpr (r < mr) {                                                      \
    c##r##0 = load_c ? load(c + r * ldc) : V(0.f);                             \
    c##r##1 = load_c ? load(c + r * ldc + W) : V(0.f);                         \
  }
  NX_GEMM_ROWS(NX_GEMM_INIT)
  for (size_t p = 0; p < kc; ++p, ap += mr, bp += base_nr) {
    const V b0 = load(bp), b1 = load(bp + W);
#define NX_GEMM_STEP(r)                                                        \
  if constexpr (r < mr) {                                                      \
    const V a(ap[r]);                                                          \
    c##r##0 = madd<Fused>(a, b0, c##r##0);                                     \
    c##r##1 = madd<Fused>(a, b1, c##r##1);                                     \
  }
    NX_GEMM_ROWS(NX_GEMM_STEP)
  }
#define NX_GEMM_STORE(r)                                                       \
  if constexpr (r < mr) {                                                      \
    unchecked_store(c##r##0, c + r * ldc, W);                                  \
    unchecked_store(c##r##1, c + r * ldc + W, W);                              \
  }
  NX_GEMM_ROWS(NX_GEMM_STORE)
#undef NX_GEMM_STORE
#undef NX_GEMM_STEP
#undef NX_GEMM_INIT
#undef NX_GEMM_ROWS
}

void base_run(size_t kc, const float *ap, const float *bp, float *c, size_t ldc,
              bool load_c, bool fused) {
  if (fused)
    base_micro<true>(kc, ap, bp, c, ldc, load_c);
  else
    base_micro<false>(kc, ap, bp, c, ldc, load_c);
}

constexpr kernel baseline{base_mr, base_nr, base_run,
#if defined(NX_SIMD_HAS_FMA)
                          true
#else
                          false
#endif
};

#if NX_ARCH_X64
bool cpu_has_avx2_fma() {
  unsigned r[4] = {};
#if defined(NX_COMPILER_MSVC) && !defined(NX_COMPILER_CLANG)
  __cpuid(reinterpret_cast<int *>(r), 0);
  if (r[0] < 7)
    return false;
  __cpuid(reinterpret_cast<int *>(r), 1);
  const unsigned ecx = r[2];
  __cpuidex(reinterpret_cast<int *>(r), 7, 0);
  const unsigned ebx7 = r[1];
  const bool osxsave = (ecx >> 27) & 1;
  const unsigned long long xcr0 = osxsave ? _xgetbv(0) : 0;
#else
  if (__get_cpuid_max(0, nullptr) < 7)
    return false;
  __get_cpuid(1, &r[0], &r[1], &r[2], &r[3]);
  const unsigned ecx = r[2];
  __cpuid_count(7, 0, r[0], r[1], r[2], r[3]);
  const unsigned ebx7 = r[1];
  const bool osxsave = (ecx >> 27) & 1;
  unsigned lo = 0, hi = 0;
  if (osxsave)
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  const unsigned long long xcr0 =
      (static_cast<unsigned long long>(hi) << 32) | lo;
#endif
  const bool fma = (ecx >> 12) & 1, avx = (ecx >> 28) & 1;
  const bool avx2 = (ebx7 >> 5) & 1;
  // The OS saves the YMM registers.
  return fma && avx && avx2 && osxsave && (xcr0 & 6) == 6;
}
#endif

const kernel &best() {
#if NX_ARCH_X64
  static const kernel k = cpu_has_avx2_fma() ? avx2_fma_kernel() : baseline;
#else
  static const kernel k = baseline;
#endif
  return k;
}

// Cache blocking: an A block of mc x kc floats stays in L2, a B block of
// kc x nc in L3, a kc x nr sliver of it in L1.
constexpr size_t kc_max = 256;
constexpr size_t mc_max = 96;
constexpr size_t nc_max = 2048;
static_assert(mc_max % 4 == 0 && mc_max % 6 == 0 && nc_max % 16 == 0);

/// Rows [0, mc) of A as mr-row slivers, column p of a sliver contiguous;
/// rows past mc are 0.
void pack_a(const float *a, size_t lda, size_t mc, size_t kc, int mr,
            float *out) {
  for (size_t i = 0; i < mc; i += size_t(mr), out += size_t(mr) * kc) {
    const size_t rows = std::min(size_t(mr), mc - i);
    // Row by row, so the reads run along A's rows.
    for (size_t r = 0; r < rows; ++r) {
      const float *row = a + (i + r) * lda;
      for (size_t p = 0; p < kc; ++p)
        out[p * size_t(mr) + r] = row[p];
    }
    for (size_t r = rows; r < size_t(mr); ++r)
      for (size_t p = 0; p < kc; ++p)
        out[p * size_t(mr) + r] = 0.f;
  }
}

/// Columns [0, nc) of B as nr-column slivers, row p of a sliver contiguous;
/// columns past nc are 0.
void pack_b(const float *b, size_t ldb, size_t kc, size_t nc, int nr,
            float *out) {
  for (size_t j = 0; j < nc; j += size_t(nr)) {
    const size_t cols = std::min(size_t(nr), nc - j);
    for (size_t p = 0; p < kc; ++p) {
      std::memcpy(out, b + p * ldb + j, cols * sizeof(float));
      std::fill(out + cols, out + nr, 0.f);
      out += nr;
    }
  }
}

struct problem {
  size_t m, n, k;
  const float *a;
  size_t lda;
  const float *b;
  size_t ldb;
  float *c;
  size_t ldc;
  bool accumulate, fused;
};

/// The mc x nc block of C at (ic, jc) for depth [pc, pc + kc), from the packed
/// B block bp.
void block(const kernel &kn, const problem &pr, size_t ic, size_t jc, size_t pc,
           size_t kc, size_t nc, const float *bp, float *ap) {
  const size_t mc = std::min(mc_max, pr.m - ic);
  pack_a(pr.a + ic * pr.lda + pc, pr.lda, mc, kc, kn.mr, ap);
  const bool load_c = pr.accumulate || pc > 0;
  const bool fused = pr.fused && kn.fuses;
  alignas(64) float edge[6 * 16];
  for (size_t jr = 0; jr < nc; jr += size_t(kn.nr)) {
    const size_t cols = std::min(size_t(kn.nr), nc - jr);
    for (size_t ir = 0; ir < mc; ir += size_t(kn.mr)) {
      const size_t rows = std::min(size_t(kn.mr), mc - ir);
      float *c = pr.c + (ic + ir) * pr.ldc + jc + jr;
      const float *a = ap + ir * kc, *b = bp + jr * kc;
      if (rows == size_t(kn.mr) && cols == size_t(kn.nr)) {
        kn.run(kc, a, b, c, pr.ldc, load_c, fused);
        continue;
      }
      // An edge tile runs on a full-size copy; only its real part returns.
      std::fill(std::begin(edge), std::end(edge), 0.f);
      if (load_c)
        for (size_t r = 0; r < rows; ++r)
          std::memcpy(edge + r * kn.nr, c + r * pr.ldc, cols * sizeof(float));
      kn.run(kc, a, b, edge, size_t(kn.nr), load_c, fused);
      for (size_t r = 0; r < rows; ++r)
        std::memcpy(c + r * pr.ldc, edge + r * kn.nr, cols * sizeof(float));
    }
  }
}

void run(const kernel &kn, const problem &pr, thread_pool *pool) {
  if (pr.k == 0) {
    if (!pr.accumulate)
      for (size_t i = 0; i < pr.m; ++i)
        std::fill(pr.c + i * pr.ldc, pr.c + i * pr.ldc + pr.n, 0.f);
    return;
  }
  const size_t blocks = (pr.m + mc_max - 1) / mc_max;
  // Below about 2^26 multiply-adds a fork and join costs more than it saves.
  const bool parallel = pool && blocks > 1 &&
                        double(pr.m) * double(pr.n) * double(pr.k) >= 0x1p26;
  nx::vector<float> bp(kc_max * ((std::min(nc_max, pr.n) + 15) / 16 * 16));
  nx::vector<float> ap(mc_max * kc_max);
  for (size_t jc = 0; jc < pr.n; jc += nc_max) {
    const size_t nc = std::min(nc_max, pr.n - jc);
    for (size_t pc = 0; pc < pr.k; pc += kc_max) {
      const size_t kc = std::min(kc_max, pr.k - pc);
      pack_b(pr.b + pc * pr.ldb + jc, pr.ldb, kc, nc, kn.nr, bp.data());
      // Each block of rows is one task's alone, and depth stays in order, so
      // every element sums the same way however many workers run.
      if (parallel) {
        pool->parallel_for(0, blocks, 1, [&](usize blk) {
          nx::vector<float> mine(mc_max * kc_max);
          block(kn, pr, blk * mc_max, jc, pc, kc, nc, bp.data(), mine.data());
        });
      } else {
        for (size_t blk = 0; blk < blocks; ++blk)
          block(kn, pr, blk * mc_max, jc, pc, kc, nc, bp.data(), ap.data());
      }
    }
  }
}

} // namespace

} // namespace gemm_detail

void gemm(size_t m, size_t n, size_t k, const float *a, size_t lda,
          const float *b, size_t ldb, float *c, size_t ldc,
          const gemm_options &options) {
  NX_ASSERT(lda >= k && ldb >= n && ldc >= n, "gemm: a stride below its row");
  const gemm_detail::problem pr{
      m, n, k, a, lda, b, ldb, c, ldc, options.accumulate, options.fused};
  const gemm_detail::kernel &kn = options.kernel == gemm_kernel::baseline
                                      ? gemm_detail::baseline
                                      : gemm_detail::best();
  gemm_detail::run(kn, pr, options.pool);
}

} // namespace nx::simd::linalg
