#pragma once

#include "core/foundation/core/config.h"

#include <cmath>
#include <cstdint>

// f32x4: four floats in a register, with the operations the math types use.
// NX_MATH_FORCE_SCALAR builds it on plain arrays, the tests' reference.
#if !defined(NX_MATH_FORCE_SCALAR)
#if defined(NX_ARCH_X64) || defined(__SSE2__)
#define NX_MATH_SSE 1
#include <emmintrin.h>
#elif defined(NX_ARCH_AARCH64) && (defined(__GNUC__) || defined(__clang__))
#define NX_MATH_NEON 1
#include <arm_neon.h>
#endif
#endif

// clang fuses a * b + c into one rounding wherever FMA exists (AVX2, every
// AArch64); MSVC and ISO-mode GCC do not. Off inside these headers, so every
// compiler and backend computes the same results.
#if defined(__clang__)
#define NX_MATH_BEGIN                                                          \
  _Pragma("float_control(push)") _Pragma("clang fp contract(off)")
#define NX_MATH_END _Pragma("float_control(pop)")
#else
#define NX_MATH_BEGIN
#define NX_MATH_END
#endif

NX_MATH_BEGIN

namespace nx::math {

#if defined(NX_MATH_SSE)
using f32x4 = __m128;
#elif defined(NX_MATH_NEON)
using f32x4 = float32x4_t;
#else
struct f32x4 {
  float v[4];
};
#endif

namespace x4 {

#if defined(NX_MATH_SSE)

NX_FORCE_INLINE f32x4 load(const float *p) { return _mm_loadu_ps(p); }
NX_FORCE_INLINE void store(float *p, f32x4 a) { _mm_storeu_ps(p, a); }
/// x, y, z and a zero w, reading only three floats.
NX_FORCE_INLINE f32x4 load3(const float *p) {
  return _mm_movelh_ps(
      _mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(p))),
      _mm_load_ss(p + 2));
}
NX_FORCE_INLINE void store3(float *p, f32x4 a) {
  _mm_storel_epi64(reinterpret_cast<__m128i *>(p), _mm_castps_si128(a));
  _mm_store_ss(p + 2, _mm_movehl_ps(a, a));
}
NX_FORCE_INLINE f32x4 set(float x, float y, float z, float w) {
  return _mm_setr_ps(x, y, z, w);
}
NX_FORCE_INLINE f32x4 splat(float s) { return _mm_set1_ps(s); }
NX_FORCE_INLINE f32x4 add(f32x4 a, f32x4 b) { return _mm_add_ps(a, b); }
NX_FORCE_INLINE f32x4 sub(f32x4 a, f32x4 b) { return _mm_sub_ps(a, b); }
NX_FORCE_INLINE f32x4 mul(f32x4 a, f32x4 b) { return _mm_mul_ps(a, b); }
NX_FORCE_INLINE f32x4 div(f32x4 a, f32x4 b) { return _mm_div_ps(a, b); }
NX_FORCE_INLINE f32x4 sqrt(f32x4 a) { return _mm_sqrt_ps(a); }
/// a < b ? a : b per lane, NaN included.
NX_FORCE_INLINE f32x4 min(f32x4 a, f32x4 b) { return _mm_min_ps(a, b); }
/// a > b ? a : b per lane, NaN included.
NX_FORCE_INLINE f32x4 max(f32x4 a, f32x4 b) { return _mm_max_ps(a, b); }
template <int I> NX_FORCE_INLINE float get(f32x4 a) {
  if constexpr (I == 0)
    return _mm_cvtss_f32(a);
  else
    return _mm_cvtss_f32(_mm_shuffle_ps(a, a, I));
}
/// (a[I0], a[I1], a[I2], a[I3]).
template <int I0, int I1, int I2, int I3>
NX_FORCE_INLINE f32x4 shuffle(f32x4 a) {
  return _mm_shuffle_ps(a, a, _MM_SHUFFLE(I3, I2, I1, I0));
}
/// (a[I0], a[I1], b[J2], b[J3]).
template <int I0, int I1, int J2, int J3>
NX_FORCE_INLINE f32x4 shuffle(f32x4 a, f32x4 b) {
  return _mm_shuffle_ps(a, b, _MM_SHUFFLE(J3, J2, I1, I0));
}
template <bool N0, bool N1, bool N2, bool N3>
NX_FORCE_INLINE f32x4 negate(f32x4 a) {
  return _mm_xor_ps(a, _mm_setr_ps(N0 ? -0.f : 0.f, N1 ? -0.f : 0.f,
                                   N2 ? -0.f : 0.f, N3 ? -0.f : 0.f));
}
NX_FORCE_INLINE void transpose(f32x4 &r0, f32x4 &r1, f32x4 &r2, f32x4 &r3) {
  _MM_TRANSPOSE4_PS(r0, r1, r2, r3);
}

#elif defined(NX_MATH_NEON)

NX_FORCE_INLINE f32x4 load(const float *p) { return vld1q_f32(p); }
NX_FORCE_INLINE void store(float *p, f32x4 a) { vst1q_f32(p, a); }
NX_FORCE_INLINE f32x4 load3(const float *p) {
  return vcombine_f32(vld1_f32(p), vset_lane_f32(p[2], vdup_n_f32(0.f), 0));
}
NX_FORCE_INLINE void store3(float *p, f32x4 a) {
  vst1_f32(p, vget_low_f32(a));
  vst1q_lane_f32(p + 2, a, 2);
}
NX_FORCE_INLINE f32x4 set(float x, float y, float z, float w) {
  const float v[4] = {x, y, z, w};
  return vld1q_f32(v);
}
NX_FORCE_INLINE f32x4 splat(float s) { return vdupq_n_f32(s); }
NX_FORCE_INLINE f32x4 add(f32x4 a, f32x4 b) { return vaddq_f32(a, b); }
NX_FORCE_INLINE f32x4 sub(f32x4 a, f32x4 b) { return vsubq_f32(a, b); }
NX_FORCE_INLINE f32x4 mul(f32x4 a, f32x4 b) { return vmulq_f32(a, b); }
NX_FORCE_INLINE f32x4 div(f32x4 a, f32x4 b) { return vdivq_f32(a, b); }
NX_FORCE_INLINE f32x4 sqrt(f32x4 a) { return vsqrtq_f32(a); }
// fmin/fmax would return the number beside a NaN; SSE's rule keeps every
// backend equal.
NX_FORCE_INLINE f32x4 min(f32x4 a, f32x4 b) {
  return vbslq_f32(vcltq_f32(a, b), a, b);
}
NX_FORCE_INLINE f32x4 max(f32x4 a, f32x4 b) {
  return vbslq_f32(vcgtq_f32(a, b), a, b);
}
template <int I> NX_FORCE_INLINE float get(f32x4 a) {
  return vgetq_lane_f32(a, I);
}
template <int I0, int I1, int I2, int I3>
NX_FORCE_INLINE f32x4 shuffle(f32x4 a) {
  return __builtin_shufflevector(a, a, I0, I1, I2, I3);
}
template <int I0, int I1, int J2, int J3>
NX_FORCE_INLINE f32x4 shuffle(f32x4 a, f32x4 b) {
  return __builtin_shufflevector(a, b, I0, I1, J2 + 4, J3 + 4);
}
template <bool N0, bool N1, bool N2, bool N3>
NX_FORCE_INLINE f32x4 negate(f32x4 a) {
  const uint32_t m[4] = {N0 ? 0x80000000u : 0u, N1 ? 0x80000000u : 0u,
                         N2 ? 0x80000000u : 0u, N3 ? 0x80000000u : 0u};
  return vreinterpretq_f32_u32(
      veorq_u32(vreinterpretq_u32_f32(a), vld1q_u32(m)));
}
NX_FORCE_INLINE void transpose(f32x4 &r0, f32x4 &r1, f32x4 &r2, f32x4 &r3) {
  const f32x4 t0 = __builtin_shufflevector(r0, r1, 0, 4, 1, 5);
  const f32x4 t1 = __builtin_shufflevector(r2, r3, 0, 4, 1, 5);
  const f32x4 t2 = __builtin_shufflevector(r0, r1, 2, 6, 3, 7);
  const f32x4 t3 = __builtin_shufflevector(r2, r3, 2, 6, 3, 7);
  r0 = __builtin_shufflevector(t0, t1, 0, 1, 4, 5);
  r1 = __builtin_shufflevector(t0, t1, 2, 3, 6, 7);
  r2 = __builtin_shufflevector(t2, t3, 0, 1, 4, 5);
  r3 = __builtin_shufflevector(t2, t3, 2, 3, 6, 7);
}

#else

NX_FORCE_INLINE f32x4 load(const float *p) {
  return {{p[0], p[1], p[2], p[3]}};
}
NX_FORCE_INLINE void store(float *p, f32x4 a) {
  p[0] = a.v[0], p[1] = a.v[1], p[2] = a.v[2], p[3] = a.v[3];
}
NX_FORCE_INLINE f32x4 load3(const float *p) {
  return {{p[0], p[1], p[2], 0.f}};
}
NX_FORCE_INLINE void store3(float *p, f32x4 a) {
  p[0] = a.v[0], p[1] = a.v[1], p[2] = a.v[2];
}
NX_FORCE_INLINE f32x4 set(float x, float y, float z, float w) {
  return {{x, y, z, w}};
}
NX_FORCE_INLINE f32x4 splat(float s) { return {{s, s, s, s}}; }
NX_FORCE_INLINE f32x4 add(f32x4 a, f32x4 b) {
  return {{a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2], a.v[3] + b.v[3]}};
}
NX_FORCE_INLINE f32x4 sub(f32x4 a, f32x4 b) {
  return {{a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2], a.v[3] - b.v[3]}};
}
NX_FORCE_INLINE f32x4 mul(f32x4 a, f32x4 b) {
  return {{a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2], a.v[3] * b.v[3]}};
}
NX_FORCE_INLINE f32x4 div(f32x4 a, f32x4 b) {
  return {{a.v[0] / b.v[0], a.v[1] / b.v[1], a.v[2] / b.v[2], a.v[3] / b.v[3]}};
}
NX_FORCE_INLINE f32x4 sqrt(f32x4 a) {
  return {{std::sqrt(a.v[0]), std::sqrt(a.v[1]), std::sqrt(a.v[2]),
           std::sqrt(a.v[3])}};
}
NX_FORCE_INLINE f32x4 min(f32x4 a, f32x4 b) {
  f32x4 r;
  for (int i = 0; i < 4; ++i)
    r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i];
  return r;
}
NX_FORCE_INLINE f32x4 max(f32x4 a, f32x4 b) {
  f32x4 r;
  for (int i = 0; i < 4; ++i)
    r.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i];
  return r;
}
template <int I> NX_FORCE_INLINE float get(f32x4 a) { return a.v[I]; }
template <int I0, int I1, int I2, int I3>
NX_FORCE_INLINE f32x4 shuffle(f32x4 a) {
  return {{a.v[I0], a.v[I1], a.v[I2], a.v[I3]}};
}
template <int I0, int I1, int J2, int J3>
NX_FORCE_INLINE f32x4 shuffle(f32x4 a, f32x4 b) {
  return {{a.v[I0], a.v[I1], b.v[J2], b.v[J3]}};
}
template <bool N0, bool N1, bool N2, bool N3>
NX_FORCE_INLINE f32x4 negate(f32x4 a) {
  return {{N0 ? -a.v[0] : a.v[0], N1 ? -a.v[1] : a.v[1], N2 ? -a.v[2] : a.v[2],
           N3 ? -a.v[3] : a.v[3]}};
}
NX_FORCE_INLINE void transpose(f32x4 &r0, f32x4 &r1, f32x4 &r2, f32x4 &r3) {
  const f32x4 a = r0, b = r1, c = r2, d = r3;
  r0 = {{a.v[0], b.v[0], c.v[0], d.v[0]}};
  r1 = {{a.v[1], b.v[1], c.v[1], d.v[1]}};
  r2 = {{a.v[2], b.v[2], c.v[2], d.v[2]}};
  r3 = {{a.v[3], b.v[3], c.v[3], d.v[3]}};
}

#endif

template <int I> NX_FORCE_INLINE f32x4 splat(f32x4 a) {
  return shuffle<I, I, I, I>(a);
}

/// (a.x * b.x + a.y * b.y) + (a.z * b.z + a.w * b.w) in every lane.
NX_FORCE_INLINE f32x4 dot4(f32x4 a, f32x4 b) {
  const f32x4 m = mul(a, b);
  const f32x4 s = add(m, shuffle<1, 0, 3, 2>(m));
  return add(s, shuffle<2, 3, 0, 1>(s));
}

} // namespace x4
} // namespace nx::math

NX_MATH_END
