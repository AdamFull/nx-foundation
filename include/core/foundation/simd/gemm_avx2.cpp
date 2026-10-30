// Built with AVX2 and FMA enabled, and called only once the CPU is known to
// have them. Raw intrinsics only: see gemm_kernel.h.
#include "core/foundation/simd/gemm_kernel.h"

#include <immintrin.h>

namespace nx::simd::linalg::gemm_detail {

namespace {

constexpr int mr = 6;
constexpr int nr = 16;

template <bool Fused> __m256 madd(__m256 a, __m256 b, __m256 c) {
  if constexpr (Fused)
    return _mm256_fmadd_ps(a, b, c);
  else
    return _mm256_add_ps(c, _mm256_mul_ps(a, b));
}

template <bool Fused>
void micro(size_t kc, const float *ap, const float *bp, float *c, size_t ldc,
           bool load_c) {
  __m256 c00, c01, c10, c11, c20, c21, c30, c31, c40, c41, c50, c51;
  if (load_c) {
    c00 = _mm256_loadu_ps(c), c01 = _mm256_loadu_ps(c + 8);
    c10 = _mm256_loadu_ps(c + ldc), c11 = _mm256_loadu_ps(c + ldc + 8);
    c20 = _mm256_loadu_ps(c + 2 * ldc), c21 = _mm256_loadu_ps(c + 2 * ldc + 8);
    c30 = _mm256_loadu_ps(c + 3 * ldc), c31 = _mm256_loadu_ps(c + 3 * ldc + 8);
    c40 = _mm256_loadu_ps(c + 4 * ldc), c41 = _mm256_loadu_ps(c + 4 * ldc + 8);
    c50 = _mm256_loadu_ps(c + 5 * ldc), c51 = _mm256_loadu_ps(c + 5 * ldc + 8);
  } else {
    c00 = c01 = c10 = c11 = c20 = c21 = _mm256_setzero_ps();
    c30 = c31 = c40 = c41 = c50 = c51 = _mm256_setzero_ps();
  }
  for (size_t p = 0; p < kc; ++p, ap += mr, bp += nr) {
    const __m256 b0 = _mm256_loadu_ps(bp), b1 = _mm256_loadu_ps(bp + 8);
    __m256 a = _mm256_broadcast_ss(ap);
    c00 = madd<Fused>(a, b0, c00), c01 = madd<Fused>(a, b1, c01);
    a = _mm256_broadcast_ss(ap + 1);
    c10 = madd<Fused>(a, b0, c10), c11 = madd<Fused>(a, b1, c11);
    a = _mm256_broadcast_ss(ap + 2);
    c20 = madd<Fused>(a, b0, c20), c21 = madd<Fused>(a, b1, c21);
    a = _mm256_broadcast_ss(ap + 3);
    c30 = madd<Fused>(a, b0, c30), c31 = madd<Fused>(a, b1, c31);
    a = _mm256_broadcast_ss(ap + 4);
    c40 = madd<Fused>(a, b0, c40), c41 = madd<Fused>(a, b1, c41);
    a = _mm256_broadcast_ss(ap + 5);
    c50 = madd<Fused>(a, b0, c50), c51 = madd<Fused>(a, b1, c51);
  }
  _mm256_storeu_ps(c, c00), _mm256_storeu_ps(c + 8, c01);
  _mm256_storeu_ps(c + ldc, c10), _mm256_storeu_ps(c + ldc + 8, c11);
  _mm256_storeu_ps(c + 2 * ldc, c20), _mm256_storeu_ps(c + 2 * ldc + 8, c21);
  _mm256_storeu_ps(c + 3 * ldc, c30), _mm256_storeu_ps(c + 3 * ldc + 8, c31);
  _mm256_storeu_ps(c + 4 * ldc, c40), _mm256_storeu_ps(c + 4 * ldc + 8, c41);
  _mm256_storeu_ps(c + 5 * ldc, c50), _mm256_storeu_ps(c + 5 * ldc + 8, c51);
}

void run(size_t kc, const float *ap, const float *bp, float *c, size_t ldc,
         bool load_c, bool fused) {
  if (fused)
    micro<true>(kc, ap, bp, c, ldc, load_c);
  else
    micro<false>(kc, ap, bp, c, ldc, load_c);
}

} // namespace

kernel avx2_fma_kernel() { return {mr, nr, run, true}; }

} // namespace nx::simd::linalg::gemm_detail
