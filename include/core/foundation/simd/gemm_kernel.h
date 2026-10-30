#pragma once

#include <cstddef>

// The micro-kernel gemm.cpp drives. A source built for another instruction
// set includes only this, never nx::simd, so that no inline function compiled
// for that set can stand in for the baseline one at link time.
namespace nx::simd::linalg::gemm_detail {

/// C[0..mr)[0..nr) = (load_c ? C : 0) + sum over p < kc of a_p b_p^T, adding
/// one p at a time, where a_p = ap[p mr ..] and b_p = bp[p nr ..]. Unless
/// fused, every product and sum rounds on its own.
using micro_kernel = void (*)(size_t kc, const float *ap, const float *bp,
                              float *c, size_t ldc, bool load_c, bool fused);

struct kernel {
  int mr, nr;
  micro_kernel run;
  /// Whether `fused` changes anything: the CPU fuses multiply-adds.
  bool fuses;
};

#if defined(_M_X64) || defined(__x86_64__)
kernel avx2_fma_kernel();
#endif

} // namespace nx::simd::linalg::gemm_detail
