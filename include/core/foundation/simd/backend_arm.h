#pragma once

#include "core/foundation/simd/backend_generic.h"

#if defined(NX_SIMD_VECTOR_EXT) && defined(__aarch64__) && defined(__ARM_NEON)
#define NX_SIMD_NEON 1
#include <arm_neon.h>
#if defined(__ARM_FEATURE_SVE)
#define NX_SIMD_SVE 1
#include <arm_neon_sve_bridge.h>
#include <arm_sve.h>
#if defined(__ARM_FEATURE_SVE_BITS) && __ARM_FEATURE_SVE_BITS > 128
#define NX_SIMD_SVE_BYTES (__ARM_FEATURE_SVE_BITS / 8)
#endif
#endif
#endif

#if defined(NX_SIMD_NEON)

// The vector backend stores GNU vectors; these work on the same bits through
// the NEON (and SVE) register types, on 16-byte vectors.
namespace nx::simd::detail::neon {

template <class E> struct reg;
template <> struct reg<i8> {
  using q = int8x16_t;
};
template <> struct reg<u8> {
  using q = uint8x16_t;
};
template <> struct reg<i16> {
  using q = int16x8_t;
};
template <> struct reg<u16> {
  using q = uint16x8_t;
};
template <> struct reg<i32> {
  using q = int32x4_t;
};
template <> struct reg<u32> {
  using q = uint32x4_t;
};
template <> struct reg<i64> {
  using q = int64x2_t;
};
template <> struct reg<u64> {
  using q = uint64x2_t;
};
template <> struct reg<float> {
  using q = float32x4_t;
};
template <> struct reg<double> {
  using q = float64x2_t;
};
template <class E> using q_t = typename reg<E>::q;

template <class E, class V> NX_FORCE_INLINE q_t<E> q(V v) { return (q_t<E>)v; }
template <class V> NX_FORCE_INLINE uint8x16_t bytes(V v) {
  return (uint8x16_t)v;
}

template <class E>
inline constexpr bool has_minmax_across =
    sizeof(E) < 8 || std::is_floating_point_v<E>;

template <class E> NX_FORCE_INLINE E add_across(q_t<E> v) {
  if constexpr (std::is_same_v<E, i8>)
    return vaddvq_s8(v);
  else if constexpr (std::is_same_v<E, u8>)
    return vaddvq_u8(v);
  else if constexpr (std::is_same_v<E, i16>)
    return vaddvq_s16(v);
  else if constexpr (std::is_same_v<E, u16>)
    return vaddvq_u16(v);
  else if constexpr (std::is_same_v<E, i32>)
    return vaddvq_s32(v);
  else if constexpr (std::is_same_v<E, u32>)
    return vaddvq_u32(v);
  else if constexpr (std::is_same_v<E, i64>)
    return vaddvq_s64(v);
  else if constexpr (std::is_same_v<E, u64>)
    return vaddvq_u64(v);
  else if constexpr (std::is_same_v<E, float>)
    return vaddvq_f32(v);
  else
    return vaddvq_f64(v);
}

template <class E> NX_FORCE_INLINE E min_across(q_t<E> v) {
  if constexpr (std::is_same_v<E, i8>)
    return vminvq_s8(v);
  else if constexpr (std::is_same_v<E, u8>)
    return vminvq_u8(v);
  else if constexpr (std::is_same_v<E, i16>)
    return vminvq_s16(v);
  else if constexpr (std::is_same_v<E, u16>)
    return vminvq_u16(v);
  else if constexpr (std::is_same_v<E, i32>)
    return vminvq_s32(v);
  else if constexpr (std::is_same_v<E, u32>)
    return vminvq_u32(v);
  else if constexpr (std::is_same_v<E, float>)
    return vminvq_f32(v);
  else
    return vminvq_f64(v);
}

template <class E> NX_FORCE_INLINE E max_across(q_t<E> v) {
  if constexpr (std::is_same_v<E, i8>)
    return vmaxvq_s8(v);
  else if constexpr (std::is_same_v<E, u8>)
    return vmaxvq_u8(v);
  else if constexpr (std::is_same_v<E, i16>)
    return vmaxvq_s16(v);
  else if constexpr (std::is_same_v<E, u16>)
    return vmaxvq_u16(v);
  else if constexpr (std::is_same_v<E, i32>)
    return vmaxvq_s32(v);
  else if constexpr (std::is_same_v<E, u32>)
    return vmaxvq_u32(v);
  else if constexpr (std::is_same_v<E, float>)
    return vmaxvq_f32(v);
  else
    return vmaxvq_f64(v);
}

// Mask lanes are all ones or all zeros, so 32-bit lanes answer for any width.
template <class S> NX_FORCE_INLINE bool all_of(S m) {
  return vminvq_u32((uint32x4_t)m) == 0xffffffffu;
}
template <class S> NX_FORCE_INLINE bool any_of(S m) {
  return vmaxvq_u32((uint32x4_t)m) != 0;
}

template <size_t B, class S> NX_FORCE_INLINE S from_bits(u64 bits) {
  if constexpr (B == 1) {
    const uint8x16_t lanes =
        vcombine_u8(vdup_n_u8(u8(bits)), vdup_n_u8(u8(bits >> 8)));
    return (S)vtstq_u8(lanes, uint8x16_t{1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4,
                                         8, 16, 32, 64, 128});
  } else if constexpr (B == 2) {
    return (S)vtstq_u16(vdupq_n_u16(u16(bits & 0xff)),
                        uint16x8_t{1, 2, 4, 8, 16, 32, 64, 128});
  } else if constexpr (B == 4) {
    return (S)vtstq_u32(vdupq_n_u32(u32(bits & 0xf)), uint32x4_t{1, 2, 4, 8});
  } else {
    return (S)vtstq_u64(vdupq_n_u64(bits & 3), uint64x2_t{1, 2});
  }
}

template <class E, class S> NX_FORCE_INLINE S sqrt(S a) {
  if constexpr (std::is_same_v<E, float>)
    return (S)vsqrtq_f32(q<E>(a));
  else
    return (S)vsqrtq_f64(q<E>(a));
}
template <class E, class S> NX_FORCE_INLINE S floor(S a) {
  if constexpr (std::is_same_v<E, float>)
    return (S)vrndmq_f32(q<E>(a));
  else
    return (S)vrndmq_f64(q<E>(a));
}
template <class E, class S> NX_FORCE_INLINE S ceil(S a) {
  if constexpr (std::is_same_v<E, float>)
    return (S)vrndpq_f32(q<E>(a));
  else
    return (S)vrndpq_f64(q<E>(a));
}
template <class E, class S> NX_FORCE_INLINE S trunc(S a) {
  if constexpr (std::is_same_v<E, float>)
    return (S)vrndq_f32(q<E>(a));
  else
    return (S)vrndq_f64(q<E>(a));
}
template <class E, class S> NX_FORCE_INLINE S round(S a) {
  if constexpr (std::is_same_v<E, float>)
    return (S)vrndaq_f32(q<E>(a));
  else
    return (S)vrndaq_f64(q<E>(a));
}
template <class E, class S> NX_FORCE_INLINE S rint(S a) {
  if constexpr (std::is_same_v<E, float>)
    return (S)vrndxq_f32(q<E>(a));
  else
    return (S)vrndxq_f64(q<E>(a));
}
template <class E, class S> NX_FORCE_INLINE S fma(S a, S b, S c) {
  if constexpr (std::is_same_v<E, float>)
    return (S)vfmaq_f32(q<E>(c), q<E>(a), q<E>(b));
  else
    return (S)vfmaq_f64(q<E>(c), q<E>(a), q<E>(b));
}
template <class E, class S> NX_FORCE_INLINE S fmin(S a, S b) {
  if constexpr (std::is_same_v<E, float>)
    return (S)vminnmq_f32(q<E>(a), q<E>(b));
  else
    return (S)vminnmq_f64(q<E>(a), q<E>(b));
}
template <class E, class S> NX_FORCE_INLINE S fmax(S a, S b) {
  if constexpr (std::is_same_v<E, float>)
    return (S)vmaxnmq_f32(q<E>(a), q<E>(b));
  else
    return (S)vmaxnmq_f64(q<E>(a), q<E>(b));
}

// Counting works on bytes and widens pairwise up to the lane size.
template <size_t L, class S> NX_FORCE_INLINE S popcount(S a) {
  const uint8x16_t c = vcntq_u8(bytes(a));
  if constexpr (L == 1)
    return (S)c;
  else if constexpr (L == 2)
    return (S)vpaddlq_u8(c);
  else if constexpr (L == 4)
    return (S)vpaddlq_u16(vpaddlq_u8(c));
  else
    return (S)vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(c)));
}

template <size_t L, class S> NX_FORCE_INLINE S countl_zero(S a) {
  if constexpr (L == 1) {
    return (S)vclzq_u8(bytes(a));
  } else if constexpr (L == 2) {
    return (S)vclzq_u16((uint16x8_t)a);
  } else if constexpr (L == 4) {
    return (S)vclzq_u32((uint32x4_t)a);
  } else {
    const uint64x2_t halves = (uint64x2_t)vclzq_u32((uint32x4_t)a);
    const uint64x2_t high = vshrq_n_u64(halves, 32);
    const uint64x2_t low = vandq_u64(halves, vdupq_n_u64(0xffffffffu));
    return (S)vbslq_u64(vceqq_u64(high, vdupq_n_u64(32)), vaddq_u64(high, low),
                        high);
  }
}

template <size_t L, class S> NX_FORCE_INLINE S byteswap(S a) {
  if constexpr (L == 1)
    return a;
  else if constexpr (L == 2)
    return (S)vrev16q_u8(bytes(a));
  else if constexpr (L == 4)
    return (S)vrev32q_u8(bytes(a));
  else
    return (S)vrev64q_u8(bytes(a));
}

template <size_t L, class S> NX_FORCE_INLINE S bit_reverse(S a) {
  return byteswap<L>((S)vrbitq_u8(bytes(a)));
}

} // namespace nx::simd::detail::neon

#endif

#if defined(NX_SIMD_SVE)

// SVE works on the vector backend's registers in two shapes: a 16-byte vector
// is the low part of an SVE register (the NEON bridge), and with a fixed
// vector length a whole-register vector is a fixed-length SVE value.
namespace nx::simd::detail::sve {

#if defined(NX_SIMD_SVE_BYTES)
#define NX_SIMD_SVE_FIXED(type, name)                                          \
  typedef type name                                                            \
      __attribute__((arm_sve_vector_bits(__ARM_FEATURE_SVE_BITS)));
NX_SIMD_SVE_FIXED(svint8_t, fixed_s8)
NX_SIMD_SVE_FIXED(svuint8_t, fixed_u8)
NX_SIMD_SVE_FIXED(svint16_t, fixed_s16)
NX_SIMD_SVE_FIXED(svuint16_t, fixed_u16)
NX_SIMD_SVE_FIXED(svint32_t, fixed_s32)
NX_SIMD_SVE_FIXED(svuint32_t, fixed_u32)
NX_SIMD_SVE_FIXED(svint64_t, fixed_s64)
NX_SIMD_SVE_FIXED(svuint64_t, fixed_u64)
NX_SIMD_SVE_FIXED(svfloat32_t, fixed_f32)
NX_SIMD_SVE_FIXED(svfloat64_t, fixed_f64)
#undef NX_SIMD_SVE_FIXED
#define NX_SIMD_SVE_REG(E, sv, fixed, suffix, bits)                            \
  template <> struct reg<E> {                                                  \
    using type = sv;                                                           \
    typedef E __attribute__((may_alias)) memory;                               \
    using fixed_type = fixed;                                                  \
    static type undef() { return svundef_##suffix(); }                         \
    static svbool_t first(int n) { return svwhilelt_b##bits##_s32(0, n); }     \
  };
#else
#define NX_SIMD_SVE_REG(E, sv, fixed, suffix, bits)                            \
  template <> struct reg<E> {                                                  \
    using type = sv;                                                           \
    typedef E __attribute__((may_alias)) memory;                               \
    static type undef() { return svundef_##suffix(); }                         \
    static svbool_t first(int n) { return svwhilelt_b##bits##_s32(0, n); }     \
  };
#endif

template <class E> struct reg;
NX_SIMD_SVE_REG(i8, svint8_t, fixed_s8, s8, 8)
NX_SIMD_SVE_REG(u8, svuint8_t, fixed_u8, u8, 8)
NX_SIMD_SVE_REG(i16, svint16_t, fixed_s16, s16, 16)
NX_SIMD_SVE_REG(u16, svuint16_t, fixed_u16, u16, 16)
NX_SIMD_SVE_REG(i32, svint32_t, fixed_s32, s32, 32)
NX_SIMD_SVE_REG(u32, svuint32_t, fixed_u32, u32, 32)
NX_SIMD_SVE_REG(i64, svint64_t, fixed_s64, s64, 64)
NX_SIMD_SVE_REG(u64, svuint64_t, fixed_u64, u64, 64)
NX_SIMD_SVE_REG(float, svfloat32_t, fixed_f32, f32, 32)
NX_SIMD_SVE_REG(double, svfloat64_t, fixed_f64, f64, 64)
#undef NX_SIMD_SVE_REG

template <class E> using sv_t = typename reg<E>::type;

template <int Bytes>
inline constexpr bool fits = Bytes == 16
#if defined(NX_SIMD_SVE_BYTES)
                             || Bytes == NX_SIMD_SVE_BYTES
#endif
    ;

template <class E, class S> NX_FORCE_INLINE sv_t<E> in(S v) {
  if constexpr (sizeof(S) == 16) {
    return svset_neonq(reg<E>::undef(), neon::q<E>(v));
  } else {
#if defined(NX_SIMD_SVE_BYTES)
    typename reg<E>::fixed_type f;
    std::memcpy(&f, &v, sizeof v);
    return f;
#endif
  }
}

template <class S, class E> NX_FORCE_INLINE S out(sv_t<E> v) {
  if constexpr (sizeof(S) == 16) {
    return (S)svget_neonq(v);
  } else {
#if defined(NX_SIMD_SVE_BYTES)
    const typename reg<E>::fixed_type f = v;
    S r;
    std::memcpy(&r, &f, sizeof r);
    return r;
#endif
  }
}

template <class E, class S> NX_FORCE_INLINE svbool_t lanes(int n) {
  return reg<E>::first(
      n < int(sizeof(S) / sizeof(E)) ? n : int(sizeof(S) / sizeof(E)));
}

template <class E, class S> NX_FORCE_INLINE svbool_t all() {
  return lanes<E, S>(int(sizeof(S) / sizeof(E)));
}

// A mask register's -1/0 lanes as an SVE predicate.
template <class E, class S, class M> NX_FORCE_INLINE svbool_t active(M m) {
  using I = integer_from<sizeof(E)>;
  return svcmpne(all<E, S>(), in<I>(m), I(0));
}

// The intrinsics take the alias set of their pointer argument, and the data
// may be a distinct type of the same representation: long long under int64_t.
template <class E> NX_FORCE_INLINE auto *memory(E *p) {
  return reinterpret_cast<typename reg<E>::memory *>(p);
}
template <class E> NX_FORCE_INLINE const auto *memory(const E *p) {
  return reinterpret_cast<const typename reg<E>::memory *>(p);
}

template <class E, class S> NX_FORCE_INLINE S load_n(const E *p, int n) {
  return out<S, E>(svld1(lanes<E, S>(n), memory(p)));
}
template <class E, class S> NX_FORCE_INLINE void store_n(S v, E *p, int n) {
  svst1(lanes<E, S>(n), memory(p), in<E>(v));
}
template <class E, class S, class M>
NX_FORCE_INLINE S load_masked(const E *p, M m, int n) {
  return out<S, E>(
      svld1(svand_z(svptrue_b8(), lanes<E, S>(n), active<E, S>(m)), memory(p)));
}
template <class E, class S, class M>
NX_FORCE_INLINE void store_masked(S v, E *p, M m, int n) {
  svst1(svand_z(svptrue_b8(), lanes<E, S>(n), active<E, S>(m)), memory(p),
        in<E>(v));
}

// The active elements packed to the front, zeros after them.
template <class E, class S, class M> NX_FORCE_INLINE S compress(S v, M m) {
  return out<S, E>(svcompact(active<E, S>(m), in<E>(v)));
}

template <class E, class S, class I, class M>
NX_FORCE_INLINE S gather(const E *base, I indices, M m) {
  using X = integer_from<sizeof(E)>;
  return out<S, E>(
      svld1_gather_index(active<E, S>(m), memory(base), in<X>(indices)));
}
template <class E, class S, class I, class M>
NX_FORCE_INLINE void scatter(E *base, I indices, M m, S v) {
  using X = integer_from<sizeof(E)>;
  svst1_scatter_index(active<E, S>(m), memory(base), in<X>(indices), in<E>(v));
}

template <class E, class S, class Op> NX_FORCE_INLINE E reduce(S v, Op) {
  const sv_t<E> x = in<E>(v);
  const svbool_t pg = all<E, S>();
  if constexpr (std::is_same_v<Op, op_add>) {
    if constexpr (std::is_floating_point_v<E>)
      return svaddv(pg, x);
    else
      return static_cast<E>(svaddv(pg, x));
  } else if constexpr (std::is_same_v<Op, op_and>) {
    return svandv(pg, x);
  } else if constexpr (std::is_same_v<Op, op_or>) {
    return svorv(pg, x);
  } else if constexpr (std::is_same_v<Op, op_xor>) {
    return sveorv(pg, x);
  } else if constexpr (std::is_same_v<Op, op_min>) {
    return svminv(pg, x);
  } else {
    return svmaxv(pg, x);
  }
}

#if defined(__ARM_FEATURE_SVE2_BITPERM)
template <class E, class S> NX_FORCE_INLINE S bit_compress(S a, S m) {
  return out<S, E>(svbext(in<E>(a), in<E>(m)));
}
template <class E, class S> NX_FORCE_INLINE S bit_expand(S a, S m) {
  return out<S, E>(svbdep(in<E>(a), in<E>(m)));
}
#endif

} // namespace nx::simd::detail::sve

#endif
