#pragma once

#include "core/foundation/simd/backend_generic.h"

#if defined(NX_SIMD_X86)

namespace nx::simd::detail {

template <class E, int Bytes> struct x86_register;
template <> struct x86_register<float, 16> {
  using type = __m128;
};
template <> struct x86_register<double, 16> {
  using type = __m128d;
};
template <class E>
  requires std::is_integral_v<E>
struct x86_register<E, 16> {
  using type = __m128i;
};
#if defined(NX_SIMD_X86_AVX2)
template <> struct x86_register<float, 32> {
  using type = __m256;
};
template <> struct x86_register<double, 32> {
  using type = __m256d;
};
template <class E>
  requires std::is_integral_v<E>
struct x86_register<E, 32> {
  using type = __m256i;
};
#endif
template <class E, int Bytes>
using x86_register_t = typename x86_register<E, Bytes>::type;

// Register types are chosen by template argument: a register passed by value
// only as an overload tag becomes a stack temporary, and MSVC guards any
// function holding one with a /GS cookie.
namespace x86i {

template <class R> NX_FORCE_INLINE R load(const void *p) {
  if constexpr (std::is_same_v<R, __m128>)
    return _mm_loadu_ps(static_cast<const float *>(p));
  else if constexpr (std::is_same_v<R, __m128d>)
    return _mm_loadu_pd(static_cast<const double *>(p));
  else if constexpr (std::is_same_v<R, __m128i>)
    return _mm_loadu_si128(static_cast<const __m128i *>(p));
#if defined(NX_SIMD_X86_AVX2)
  else if constexpr (std::is_same_v<R, __m256>)
    return _mm256_loadu_ps(static_cast<const float *>(p));
  else if constexpr (std::is_same_v<R, __m256d>)
    return _mm256_loadu_pd(static_cast<const double *>(p));
  else
    return _mm256_loadu_si256(static_cast<const __m256i *>(p));
#endif
}

NX_FORCE_INLINE void store(void *p, __m128 v) {
  _mm_storeu_ps(static_cast<float *>(p), v);
}
NX_FORCE_INLINE void store(void *p, __m128d v) {
  _mm_storeu_pd(static_cast<double *>(p), v);
}
NX_FORCE_INLINE void store(void *p, __m128i v) {
  _mm_storeu_si128(static_cast<__m128i *>(p), v);
}
NX_FORCE_INLINE __m128i as_int(__m128 v) { return _mm_castps_si128(v); }
NX_FORCE_INLINE __m128i as_int(__m128d v) { return _mm_castpd_si128(v); }
NX_FORCE_INLINE __m128i as_int(__m128i v) { return v; }
NX_FORCE_INLINE __m128i iand(__m128i a, __m128i b) {
  return _mm_and_si128(a, b);
}
NX_FORCE_INLINE __m128i ior(__m128i a, __m128i b) { return _mm_or_si128(a, b); }
NX_FORCE_INLINE __m128i ixor(__m128i a, __m128i b) {
  return _mm_xor_si128(a, b);
}
NX_FORCE_INLINE __m128i iandnot(__m128i a, __m128i b) {
  return _mm_andnot_si128(a, b);
}
NX_FORCE_INLINE __m128i blend(__m128i k, __m128i a, __m128i b) {
#if defined(NX_SIMD_X86_SSE41)
  return _mm_blendv_epi8(b, a, k);
#else
  return _mm_or_si128(_mm_and_si128(k, a), _mm_andnot_si128(k, b));
#endif
}

#if defined(NX_SIMD_X86_AVX2)
NX_FORCE_INLINE void store(void *p, __m256 v) {
  _mm256_storeu_ps(static_cast<float *>(p), v);
}
NX_FORCE_INLINE void store(void *p, __m256d v) {
  _mm256_storeu_pd(static_cast<double *>(p), v);
}
NX_FORCE_INLINE void store(void *p, __m256i v) {
  _mm256_storeu_si256(static_cast<__m256i *>(p), v);
}
NX_FORCE_INLINE __m256i as_int(__m256 v) { return _mm256_castps_si256(v); }
NX_FORCE_INLINE __m256i as_int(__m256d v) { return _mm256_castpd_si256(v); }
NX_FORCE_INLINE __m256i as_int(__m256i v) { return v; }
NX_FORCE_INLINE __m256i iand(__m256i a, __m256i b) {
  return _mm256_and_si256(a, b);
}
NX_FORCE_INLINE __m256i ior(__m256i a, __m256i b) {
  return _mm256_or_si256(a, b);
}
NX_FORCE_INLINE __m256i ixor(__m256i a, __m256i b) {
  return _mm256_xor_si256(a, b);
}
NX_FORCE_INLINE __m256i iandnot(__m256i a, __m256i b) {
  return _mm256_andnot_si256(a, b);
}
NX_FORCE_INLINE __m256i blend(__m256i k, __m256i a, __m256i b) {
  return _mm256_blendv_epi8(b, a, k);
}
#endif

template <class R, class I> NX_FORCE_INLINE R as(I v) {
  if constexpr (std::is_same_v<R, I>)
    return v;
  else if constexpr (std::is_same_v<R, __m128>)
    return _mm_castsi128_ps(v);
  else if constexpr (std::is_same_v<R, __m128d>)
    return _mm_castsi128_pd(v);
#if defined(NX_SIMD_X86_AVX2)
  else if constexpr (std::is_same_v<R, __m256>)
    return _mm256_castsi256_ps(v);
  else
    return _mm256_castsi256_pd(v);
#endif
}

template <class R> NX_FORCE_INLINE R ones() {
  if constexpr (std::is_same_v<R, __m128i>)
    return _mm_set1_epi32(-1);
#if defined(NX_SIMD_X86_AVX2)
  else
    return _mm256_set1_epi32(-1);
#endif
}

template <class R> NX_FORCE_INLINE R zero() {
  if constexpr (std::is_same_v<R, __m128i>)
    return _mm_setzero_si128();
#if defined(NX_SIMD_X86_AVX2)
  else
    return _mm256_setzero_si256();
#endif
}

} // namespace x86i

// MSVC's register types are unions. Constant evaluation only ever reads and
// writes their first member (the floats, the doubles, or the bytes of an
// integer register), which is also what value-initialization activates.
namespace x86c {

template <class E, class R> constexpr E get(const R &r, int i) {
  if constexpr (std::is_same_v<R, __m128>)
    return r.m128_f32[i];
  else if constexpr (std::is_same_v<R, __m128d>)
    return r.m128d_f64[i];
#if defined(NX_SIMD_X86_AVX2)
  else if constexpr (std::is_same_v<R, __m256>)
    return r.m256_f32[i];
  else if constexpr (std::is_same_v<R, __m256d>)
    return r.m256d_f64[i];
#endif
  else {
    using U = std::make_unsigned_t<E>;
    U v = 0;
    for (int b = 0; b < int(sizeof(E)); ++b) {
      i8 byte;
      if constexpr (std::is_same_v<R, __m128i>)
        byte = r.m128i_i8[i * int(sizeof(E)) + b];
      else
        byte = r.m256i_i8[i * int(sizeof(E)) + b];
      v = static_cast<U>(v | (U(static_cast<u8>(byte)) << (8 * b)));
    }
    return static_cast<E>(v);
  }
}

template <class E, class R, int N> constexpr R make(const E (&a)[N]) {
  if constexpr (std::is_same_v<R, __m128>) {
    return [&]<int... I>(std::integer_sequence<int, I...>) {
      return __m128{.m128_f32 = {a[I]...}};
    }(std::make_integer_sequence<int, 4>{});
  } else if constexpr (std::is_same_v<R, __m128d>) {
    return __m128d{.m128d_f64 = {a[0], a[1]}};
#if defined(NX_SIMD_X86_AVX2)
  } else if constexpr (std::is_same_v<R, __m256>) {
    return [&]<int... I>(std::integer_sequence<int, I...>) {
      return __m256{.m256_f32 = {a[I]...}};
    }(std::make_integer_sequence<int, 8>{});
  } else if constexpr (std::is_same_v<R, __m256d>) {
    return __m256d{.m256d_f64 = {a[0], a[1], a[2], a[3]}};
#endif
  } else {
    constexpr int bytes = int(sizeof(R));
    const auto byte = [&](int k) {
      using U = std::make_unsigned_t<E>;
      const U v = static_cast<U>(a[k / int(sizeof(E))]);
      return static_cast<i8>(static_cast<u8>(v >> (8 * (k % int(sizeof(E))))));
    };
    return [&]<int... I>(std::integer_sequence<int, I...>) {
      if constexpr (bytes == 16)
        return __m128i{.m128i_i8 = {byte(I)...}};
      else
        return R{.m256i_i8 = {byte(I)...}};
    }(std::make_integer_sequence<int, bytes>{});
  }
}

// Runtime lanes travel through 64-bit words: MSVC guards any function that
// holds a char array, or a register spilled as its char-array union, with a
// /GS cookie.
template <class E, class R> NX_FORCE_INLINE E lane(const R &r, int i) {
  if constexpr (std::is_floating_point_v<E>) {
    alignas(sizeof(R)) E lanes[sizeof(R) / sizeof(E)];
    x86i::store(lanes, r);
    return lanes[i];
  } else {
    alignas(sizeof(R)) u64 words[sizeof(R) / 8];
    x86i::store(words, r);
    const int bit = i * int(sizeof(E)) * 8;
    return static_cast<E>(words[bit / 64] >> (bit % 64));
  }
}

template <class E, class R, class G> NX_FORCE_INLINE R build(G &g) {
  constexpr int n = int(sizeof(R) / sizeof(E));
  if constexpr (std::is_floating_point_v<E>) {
    alignas(sizeof(R)) E lanes[n];
    for (int i = 0; i < n; ++i)
      lanes[i] = g(i);
    return x86i::load<R>(lanes);
  } else {
    using U = std::make_unsigned_t<E>;
    alignas(sizeof(R)) u64 words[sizeof(R) / 8] = {};
    for (int i = 0; i < n; ++i) {
      const int bit = i * int(sizeof(E)) * 8;
      words[bit / 64] |= u64{static_cast<U>(g(i))} << (bit % 64);
    }
    return x86i::load<R>(words);
  }
}

template <class E, class R, class G>
NX_NO_INLINE constexpr R build_constant(G &g) {
  E lanes[sizeof(R) / sizeof(E)] = {};
  for (int i = 0; i < int(sizeof(R) / sizeof(E)); ++i)
    lanes[i] = g(i);
  return make<E, R>(lanes);
}

} // namespace x86c

template <size_t B, int Bytes>
  requires(B <= 8)
struct mask_ops<B, abi_x86<Bytes>>
    : mask_ops_base<mask_ops<B, abi_x86<Bytes>>, B, Bytes / int(B),
                    x86_register_t<i8, Bytes>> {
  using S = x86_register_t<i8, Bytes>;
  using L = integer_from<B>;
  static constexpr int N = Bytes / int(B);
  using base = mask_ops_base<mask_ops, B, N, S>;

  static constexpr S broadcast(bool b) {
    if (std::is_constant_evaluated())
      return generate([b](int) { return b; });
    return b ? x86i::ones<S>() : x86i::zero<S>();
  }
  static constexpr bool get(const S &a, int i) {
    if (std::is_constant_evaluated())
      return x86c::get<i8>(a, i * int(B)) != 0;
    return ((to_bits(a) >> i) & 1) != 0;
  }
  template <class G> static constexpr S generate(G &&g) {
    const auto lane = [&](int i) {
      return static_cast<bool>(g(i)) ? L(-1) : L(0);
    };
    if (std::is_constant_evaluated())
      return x86c::build_constant<L, S>(lane);
    return x86c::build<L, S>(lane);
  }
  static constexpr S land(const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::land(a, b);
    return x86i::iand(a, b);
  }
  static constexpr S lor(const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::lor(a, b);
    return x86i::ior(a, b);
  }
  static constexpr S lxor(const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::lxor(a, b);
    return x86i::ixor(a, b);
  }
  static constexpr S lnot(const S &a) {
    if (std::is_constant_evaluated())
      return base::lnot(a);
    return x86i::ixor(a, x86i::ones<S>());
  }
  static constexpr S select(const S &k, const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::select(k, a, b);
    return x86i::blend(k, a, b);
  }

  static constexpr u64 to_bits(const S &a) {
    if (std::is_constant_evaluated())
      return base::to_bits(a);
    if constexpr (Bytes == 16) {
      if constexpr (B == 1)
        return static_cast<u32>(_mm_movemask_epi8(a));
      else if constexpr (B == 2)
        return static_cast<u32>(
            _mm_movemask_epi8(_mm_packs_epi16(a, _mm_setzero_si128())));
      else if constexpr (B == 4)
        return static_cast<u32>(_mm_movemask_ps(_mm_castsi128_ps(a)));
      else
        return static_cast<u32>(_mm_movemask_pd(_mm_castsi128_pd(a)));
    } else {
#if defined(NX_SIMD_X86_AVX2)
      if constexpr (B == 1) {
        return static_cast<u32>(_mm256_movemask_epi8(a));
      } else if constexpr (B == 2) {
        const u32 m = static_cast<u32>(_mm256_movemask_epi8(
            _mm256_packs_epi16(a, _mm256_setzero_si256())));
        return (m & 0xffu) | ((m >> 8) & 0xff00u);
      } else if constexpr (B == 4) {
        return static_cast<u32>(_mm256_movemask_ps(_mm256_castsi256_ps(a)));
      } else {
        return static_cast<u32>(_mm256_movemask_pd(_mm256_castsi256_pd(a)));
      }
#else
      return base::to_bits(a);
#endif
    }
  }

  static constexpr S from_bits(u64 bits) {
    if (std::is_constant_evaluated())
      return base::from_bits(bits);
    if constexpr (Bytes == 16) {
      if constexpr (B == 1) {
        const __m128i bytes = _mm_set_epi64x(
            static_cast<i64>(((bits >> 8) & 0xff) * 0x0101010101010101ull),
            static_cast<i64>((bits & 0xff) * 0x0101010101010101ull));
        const __m128i lane = _mm_set1_epi64x(0x8040201008040201ll);
        return _mm_cmpeq_epi8(_mm_and_si128(bytes, lane), lane);
      } else if constexpr (B == 2) {
        const __m128i lane = _mm_setr_epi16(1, 2, 4, 8, 16, 32, 64, 128);
        return _mm_cmpeq_epi16(
            _mm_and_si128(_mm_set1_epi16(static_cast<i16>(bits)), lane), lane);
      } else if constexpr (B == 4) {
        const __m128i lane = _mm_setr_epi32(1, 2, 4, 8);
        return _mm_cmpeq_epi32(
            _mm_and_si128(_mm_set1_epi32(static_cast<i32>(bits)), lane), lane);
      } else {
        const __m128i lane = _mm_setr_epi32(1, 1, 2, 2);
        return _mm_cmpeq_epi32(
            _mm_and_si128(_mm_set1_epi32(static_cast<i32>(bits)), lane), lane);
      }
    } else {
#if defined(NX_SIMD_X86_AVX2)
      if constexpr (B == 1) {
        const __m256i bytes = _mm256_set_epi64x(
            static_cast<i64>(((bits >> 24) & 0xff) * 0x0101010101010101ull),
            static_cast<i64>(((bits >> 16) & 0xff) * 0x0101010101010101ull),
            static_cast<i64>(((bits >> 8) & 0xff) * 0x0101010101010101ull),
            static_cast<i64>((bits & 0xff) * 0x0101010101010101ull));
        const __m256i lane = _mm256_set1_epi64x(0x8040201008040201ll);
        return _mm256_cmpeq_epi8(_mm256_and_si256(bytes, lane), lane);
      } else if constexpr (B == 2) {
        const __m256i lane =
            _mm256_setr_epi16(1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048,
                              4096, 8192, 16384, -32768);
        return _mm256_cmpeq_epi16(
            _mm256_and_si256(_mm256_set1_epi16(static_cast<i16>(bits)), lane),
            lane);
      } else if constexpr (B == 4) {
        const __m256i lane = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);
        return _mm256_cmpeq_epi32(
            _mm256_and_si256(_mm256_set1_epi32(static_cast<i32>(bits)), lane),
            lane);
      } else {
        const __m256i lane = _mm256_setr_epi64x(1, 2, 4, 8);
        return _mm256_cmpeq_epi64(
            _mm256_and_si256(_mm256_set1_epi64x(static_cast<i64>(bits)), lane),
            lane);
      }
#else
      return base::from_bits(bits);
#endif
    }
  }
};

template <int Bytes>
struct mask_ops<16, abi_x86<Bytes>> : mask_ops_lanes<16, Bytes / 16> {};

template <class T, int Bytes>
  requires vectorizable_arithmetic<T>
struct vec_ops<T, abi_x86<Bytes>>
    : vec_ops_base<vec_ops<T, abi_x86<Bytes>>, T, Bytes / int(sizeof(T)),
                   x86_register_t<canonical_t<T>, Bytes>,
                   mask_ops<sizeof(T), abi_x86<Bytes>>> {
  using E = canonical_t<T>;
  using S = x86_register_t<E, Bytes>;
  using M = mask_ops<sizeof(T), abi_x86<Bytes>>;
  using MS = typename M::storage;
  static constexpr int N = Bytes / int(sizeof(T));
  static constexpr int L = int(sizeof(T));
  using base = vec_ops_base<vec_ops, T, N, S, M>;
  using base::reduce;
  static constexpr bool is_f32 = std::is_same_v<E, float>;
  static constexpr bool is_f64 = std::is_same_v<E, double>;
  static constexpr bool is_float = is_f32 || is_f64;
  static constexpr bool is_signed = std::is_signed_v<E>;
  static constexpr bool wide = Bytes == 32;

  static constexpr S broadcast(T x) {
    if (std::is_constant_evaluated())
      return generate([x](int) { return x; });
    const E e = static_cast<E>(x);
    if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
      if constexpr (is_f32)
        return _mm256_set1_ps(e);
      else if constexpr (is_f64)
        return _mm256_set1_pd(e);
      else if constexpr (L == 1)
        return _mm256_set1_epi8(static_cast<char>(e));
      else if constexpr (L == 2)
        return _mm256_set1_epi16(static_cast<short>(e));
      else if constexpr (L == 4)
        return _mm256_set1_epi32(static_cast<int>(e));
      else
        return _mm256_set1_epi64x(static_cast<long long>(e));
#endif
    } else {
      if constexpr (is_f32)
        return _mm_set1_ps(e);
      else if constexpr (is_f64)
        return _mm_set1_pd(e);
      else if constexpr (L == 1)
        return _mm_set1_epi8(static_cast<char>(e));
      else if constexpr (L == 2)
        return _mm_set1_epi16(static_cast<short>(e));
      else if constexpr (L == 4)
        return _mm_set1_epi32(static_cast<int>(e));
      else
        return _mm_set1_epi64x(static_cast<long long>(e));
    }
  }
  static constexpr T get(const S &a, int i) {
    if (std::is_constant_evaluated())
      return static_cast<T>(x86c::get<E>(a, i));
    return static_cast<T>(x86c::lane<E>(a, i));
  }
  template <class G> static constexpr S generate(G &&g) {
    const auto lane = [&](int i) {
      return static_cast<E>(static_cast<T>(g(i)));
    };
    if (std::is_constant_evaluated())
      return x86c::build_constant<E, S>(lane);
    return x86c::build<E, S>(lane);
  }

  template <class U> static constexpr S load(const U *p) {
    if constexpr (std::is_same_v<canonical_t<U>, E>) {
      if (!std::is_constant_evaluated())
        return x86i::load<S>(p);
    }
    return base::load(p);
  }
  template <class U> static constexpr void store(const S &v, U *p) {
    if constexpr (std::is_same_v<canonical_t<U>, E>) {
      if (!std::is_constant_evaluated())
        return x86i::store(p, v);
    }
    base::store(v, p);
  }
  template <class U> static constexpr S load_n(const U *p, int n) {
    if (std::is_constant_evaluated() || n <= 0)
      return base::load_n(p, n);
    if (n >= N)
      return load(p);
    U buffer[N] = {};
    std::memcpy(buffer, p, sizeof(U) * static_cast<size_t>(n));
    return load(buffer);
  }
  template <class U> static constexpr void store_n(const S &v, U *p, int n) {
    if (std::is_constant_evaluated() || n <= 0)
      return base::store_n(v, p, n);
    if (n >= N)
      return store(v, p);
    U buffer[N];
    store(v, buffer);
    std::memcpy(p, buffer, sizeof(U) * static_cast<size_t>(n));
  }

  template <class U, class UAbi>
  static constexpr S convert(const vec_storage<U, UAbi> &u) {
    if (!std::is_constant_evaluated()) {
      using UE = canonical_t<U>;
      constexpr bool same_reg = std::is_same_v<UAbi, abi_x86<Bytes>>;
      if constexpr (is_f32 && std::is_same_v<UE, i32> && same_reg) {
        if constexpr (wide)
          return _mm256_cvtepi32_ps(u);
        else
          return _mm_cvtepi32_ps(u);
      } else if constexpr (std::is_same_v<E, i32> &&
                           std::is_same_v<UE, float> && same_reg) {
        if constexpr (wide)
          return _mm256_cvttps_epi32(u);
        else
          return _mm_cvttps_epi32(u);
      }
#if defined(NX_SIMD_X86_AVX2)
      else if constexpr (wide && std::is_same_v<UAbi, abi_x86<16>>) {
        if constexpr (is_f64 && std::is_same_v<UE, float>)
          return _mm256_cvtps_pd(u);
        else if constexpr (is_f64 && std::is_same_v<UE, i32>)
          return _mm256_cvtepi32_pd(u);
        else if constexpr (!is_float && std::is_integral_v<UE> &&
                           sizeof(UE) * 2 == sizeof(E)) {
          if constexpr (sizeof(UE) == 1)
            return std::is_signed_v<UE> ? _mm256_cvtepi8_epi16(u)
                                        : _mm256_cvtepu8_epi16(u);
          else if constexpr (sizeof(UE) == 2)
            return std::is_signed_v<UE> ? _mm256_cvtepi16_epi32(u)
                                        : _mm256_cvtepu16_epi32(u);
          else
            return std::is_signed_v<UE> ? _mm256_cvtepi32_epi64(u)
                                        : _mm256_cvtepu32_epi64(u);
        }
      } else if constexpr (!wide && std::is_same_v<UAbi, abi_x86<32>>) {
        if constexpr (is_f32 && std::is_same_v<UE, double>)
          return _mm256_cvtpd_ps(u);
        else if constexpr (std::is_same_v<E, i32> && std::is_same_v<UE, double>)
          return _mm256_cvttpd_epi32(u);
      }
#endif
    }
    return base::template convert<U, UAbi>(u);
  }

  static constexpr S neg(const S &a) {
    if (std::is_constant_evaluated())
      return base::neg(a);
    if constexpr (is_float)
      return x86i::as<S>(
          x86i::ixor(x86i::as_int(a), x86i::as_int(broadcast(T(-0.0)))));
    else
      return sub(x86i::zero<S>(), a);
  }
  static constexpr S bit_not(const S &a) {
    if (std::is_constant_evaluated())
      return base::bit_not(a);
    return x86i::ixor(a, x86i::ones<S>());
  }

  static constexpr S add(const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::add(a, b);
    if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
      if constexpr (is_f32)
        return _mm256_add_ps(a, b);
      else if constexpr (is_f64)
        return _mm256_add_pd(a, b);
      else if constexpr (L == 1)
        return _mm256_add_epi8(a, b);
      else if constexpr (L == 2)
        return _mm256_add_epi16(a, b);
      else if constexpr (L == 4)
        return _mm256_add_epi32(a, b);
      else
        return _mm256_add_epi64(a, b);
#endif
    } else {
      if constexpr (is_f32)
        return _mm_add_ps(a, b);
      else if constexpr (is_f64)
        return _mm_add_pd(a, b);
      else if constexpr (L == 1)
        return _mm_add_epi8(a, b);
      else if constexpr (L == 2)
        return _mm_add_epi16(a, b);
      else if constexpr (L == 4)
        return _mm_add_epi32(a, b);
      else
        return _mm_add_epi64(a, b);
    }
  }
  static constexpr S sub(const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::sub(a, b);
    if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
      if constexpr (is_f32)
        return _mm256_sub_ps(a, b);
      else if constexpr (is_f64)
        return _mm256_sub_pd(a, b);
      else if constexpr (L == 1)
        return _mm256_sub_epi8(a, b);
      else if constexpr (L == 2)
        return _mm256_sub_epi16(a, b);
      else if constexpr (L == 4)
        return _mm256_sub_epi32(a, b);
      else
        return _mm256_sub_epi64(a, b);
#endif
    } else {
      if constexpr (is_f32)
        return _mm_sub_ps(a, b);
      else if constexpr (is_f64)
        return _mm_sub_pd(a, b);
      else if constexpr (L == 1)
        return _mm_sub_epi8(a, b);
      else if constexpr (L == 2)
        return _mm_sub_epi16(a, b);
      else if constexpr (L == 4)
        return _mm_sub_epi32(a, b);
      else
        return _mm_sub_epi64(a, b);
    }
  }
  static constexpr S mul(const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::mul(a, b);
    if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
      if constexpr (is_f32) {
        return _mm256_mul_ps(a, b);
      } else if constexpr (is_f64) {
        return _mm256_mul_pd(a, b);
      } else if constexpr (L == 1) {
        const __m256i even = _mm256_mullo_epi16(a, b);
        const __m256i odd = _mm256_mullo_epi16(_mm256_srli_epi16(a, 8),
                                               _mm256_srli_epi16(b, 8));
        return _mm256_or_si256(_mm256_slli_epi16(odd, 8),
                               _mm256_and_si256(even, _mm256_set1_epi16(0xff)));
      } else if constexpr (L == 2) {
        return _mm256_mullo_epi16(a, b);
      } else if constexpr (L == 4) {
        return _mm256_mullo_epi32(a, b);
      } else {
        const __m256i lo = _mm256_mul_epu32(a, b);
        const __m256i cross =
            _mm256_add_epi64(_mm256_mul_epu32(_mm256_srli_epi64(a, 32), b),
                             _mm256_mul_epu32(a, _mm256_srli_epi64(b, 32)));
        return _mm256_add_epi64(lo, _mm256_slli_epi64(cross, 32));
      }
#endif
    } else {
      if constexpr (is_f32) {
        return _mm_mul_ps(a, b);
      } else if constexpr (is_f64) {
        return _mm_mul_pd(a, b);
      } else if constexpr (L == 1) {
        const __m128i even = _mm_mullo_epi16(a, b);
        const __m128i odd =
            _mm_mullo_epi16(_mm_srli_epi16(a, 8), _mm_srli_epi16(b, 8));
        return _mm_or_si128(_mm_slli_epi16(odd, 8),
                            _mm_and_si128(even, _mm_set1_epi16(0xff)));
      } else if constexpr (L == 2) {
        return _mm_mullo_epi16(a, b);
      } else if constexpr (L == 4) {
#if defined(NX_SIMD_X86_SSE41)
        return _mm_mullo_epi32(a, b);
#else
        const __m128i even = _mm_mul_epu32(a, b);
        const __m128i odd =
            _mm_mul_epu32(_mm_srli_epi64(a, 32), _mm_srli_epi64(b, 32));
        return _mm_unpacklo_epi32(
            _mm_shuffle_epi32(even, _MM_SHUFFLE(0, 0, 2, 0)),
            _mm_shuffle_epi32(odd, _MM_SHUFFLE(0, 0, 2, 0)));
#endif
      } else {
        const __m128i lo = _mm_mul_epu32(a, b);
        const __m128i cross =
            _mm_add_epi64(_mm_mul_epu32(_mm_srli_epi64(a, 32), b),
                          _mm_mul_epu32(a, _mm_srli_epi64(b, 32)));
        return _mm_add_epi64(lo, _mm_slli_epi64(cross, 32));
      }
    }
  }
  static constexpr S div(const S &a, const S &b) {
    if constexpr (is_float) {
      if (!std::is_constant_evaluated()) {
        if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
          if constexpr (is_f32)
            return _mm256_div_ps(a, b);
          else
            return _mm256_div_pd(a, b);
#endif
        } else {
          if constexpr (is_f32)
            return _mm_div_ps(a, b);
          else
            return _mm_div_pd(a, b);
        }
      }
    }
    return base::div(a, b);
  }

  static constexpr S bit_and(const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::bit_and(a, b);
    return x86i::iand(a, b);
  }
  static constexpr S bit_or(const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::bit_or(a, b);
    return x86i::ior(a, b);
  }
  static constexpr S bit_xor(const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::bit_xor(a, b);
    return x86i::ixor(a, b);
  }

  static constexpr S shl(const S &a, const S &b) {
#if defined(NX_SIMD_X86_AVX2)
    if (!std::is_constant_evaluated()) {
      if constexpr (wide && L == 4)
        return _mm256_sllv_epi32(a, b);
      else if constexpr (wide && L == 8)
        return _mm256_sllv_epi64(a, b);
      else if constexpr (L == 4)
        return _mm_sllv_epi32(a, b);
      else if constexpr (L == 8)
        return _mm_sllv_epi64(a, b);
    }
#endif
    return base::shl(a, b);
  }
  static constexpr S shr(const S &a, const S &b) {
#if defined(NX_SIMD_X86_AVX2)
    if (!std::is_constant_evaluated()) {
      if constexpr (wide && L == 4 && is_signed)
        return _mm256_srav_epi32(a, b);
      else if constexpr (wide && L == 4)
        return _mm256_srlv_epi32(a, b);
      else if constexpr (wide && L == 8 && !is_signed)
        return _mm256_srlv_epi64(a, b);
      else if constexpr (L == 4 && is_signed)
        return _mm_srav_epi32(a, b);
      else if constexpr (L == 4)
        return _mm_srlv_epi32(a, b);
      else if constexpr (L == 8 && !is_signed)
        return _mm_srlv_epi64(a, b);
    }
#endif
    return base::shr(a, b);
  }
  static constexpr S shl_n(const S &a, int n) {
    if (std::is_constant_evaluated())
      return base::shl_n(a, n);
    const __m128i count = _mm_cvtsi32_si128(n);
    if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
      if constexpr (L == 1)
        return _mm256_and_si256(
            _mm256_sll_epi16(a, count),
            _mm256_set1_epi8(static_cast<char>(n > 7 ? 0 : 0xff << n)));
      else if constexpr (L == 2)
        return _mm256_sll_epi16(a, count);
      else if constexpr (L == 4)
        return _mm256_sll_epi32(a, count);
      else
        return _mm256_sll_epi64(a, count);
#endif
    } else {
      if constexpr (L == 1)
        return _mm_and_si128(
            _mm_sll_epi16(a, count),
            _mm_set1_epi8(static_cast<char>(n > 7 ? 0 : 0xff << n)));
      else if constexpr (L == 2)
        return _mm_sll_epi16(a, count);
      else if constexpr (L == 4)
        return _mm_sll_epi32(a, count);
      else
        return _mm_sll_epi64(a, count);
    }
  }
  static constexpr S shr_n(const S &a, int n) {
    if (std::is_constant_evaluated())
      return base::shr_n(a, n);
    const __m128i count = _mm_cvtsi32_si128(n);
    if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
      if constexpr (L == 1 && !is_signed)
        return _mm256_and_si256(
            _mm256_srl_epi16(a, count),
            _mm256_set1_epi8(static_cast<char>(n > 7 ? 0 : 0xff >> n)));
      else if constexpr (L == 2)
        return is_signed ? _mm256_sra_epi16(a, count)
                         : _mm256_srl_epi16(a, count);
      else if constexpr (L == 4)
        return is_signed ? _mm256_sra_epi32(a, count)
                         : _mm256_srl_epi32(a, count);
      else if constexpr (L == 8 && !is_signed)
        return _mm256_srl_epi64(a, count);
      else
        return base::shr_n(a, n);
#endif
    } else {
      if constexpr (L == 1 && !is_signed)
        return _mm_and_si128(
            _mm_srl_epi16(a, count),
            _mm_set1_epi8(static_cast<char>(n > 7 ? 0 : 0xff >> n)));
      else if constexpr (L == 2)
        return is_signed ? _mm_sra_epi16(a, count) : _mm_srl_epi16(a, count);
      else if constexpr (L == 4)
        return is_signed ? _mm_sra_epi32(a, count) : _mm_srl_epi32(a, count);
      else if constexpr (L == 8 && !is_signed)
        return _mm_srl_epi64(a, count);
      else
        return base::shr_n(a, n);
    }
  }

  // Unsigned order is signed order once the sign bits are flipped.
  static S flip_sign(const S &a) {
    if constexpr (L == 1)
      return x86i::ixor(a, broadcast(static_cast<T>(E(1) << 7)));
    else if constexpr (L == 2)
      return x86i::ixor(a, broadcast(static_cast<T>(E(1) << 15)));
    else if constexpr (L == 4)
      return x86i::ixor(a, broadcast(static_cast<T>(E(1) << 31)));
    else
      return x86i::ixor(a, broadcast(static_cast<T>(E(1) << 63)));
  }
  static constexpr bool has_signed_gt = L < 8 ||
#if defined(__AVX__)
                                        true;
#else
                                        false;
#endif
  static S signed_gt(const S &a, const S &b) {
    if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
      if constexpr (L == 1)
        return _mm256_cmpgt_epi8(a, b);
      else if constexpr (L == 2)
        return _mm256_cmpgt_epi16(a, b);
      else if constexpr (L == 4)
        return _mm256_cmpgt_epi32(a, b);
      else
        return _mm256_cmpgt_epi64(a, b);
#endif
    } else {
      if constexpr (L == 1)
        return _mm_cmpgt_epi8(a, b);
      else if constexpr (L == 2)
        return _mm_cmpgt_epi16(a, b);
      else if constexpr (L == 4)
        return _mm_cmpgt_epi32(a, b);
      else
        return _mm_cmpgt_epi64(a, b);
    }
  }

  static constexpr MS eq(const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::eq(a, b);
    if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
      if constexpr (is_f32)
        return _mm256_castps_si256(_mm256_cmp_ps(a, b, _CMP_EQ_OQ));
      else if constexpr (is_f64)
        return _mm256_castpd_si256(_mm256_cmp_pd(a, b, _CMP_EQ_OQ));
      else if constexpr (L == 1)
        return _mm256_cmpeq_epi8(a, b);
      else if constexpr (L == 2)
        return _mm256_cmpeq_epi16(a, b);
      else if constexpr (L == 4)
        return _mm256_cmpeq_epi32(a, b);
      else
        return _mm256_cmpeq_epi64(a, b);
#endif
    } else {
      if constexpr (is_f32) {
        return _mm_castps_si128(_mm_cmpeq_ps(a, b));
      } else if constexpr (is_f64) {
        return _mm_castpd_si128(_mm_cmpeq_pd(a, b));
      } else if constexpr (L == 1) {
        return _mm_cmpeq_epi8(a, b);
      } else if constexpr (L == 2) {
        return _mm_cmpeq_epi16(a, b);
      } else if constexpr (L == 4) {
        return _mm_cmpeq_epi32(a, b);
      } else {
#if defined(NX_SIMD_X86_SSE41)
        return _mm_cmpeq_epi64(a, b);
#else
        const __m128i halves = _mm_cmpeq_epi32(a, b);
        return _mm_and_si128(
            halves, _mm_shuffle_epi32(halves, _MM_SHUFFLE(2, 3, 0, 1)));
#endif
      }
    }
  }
  static constexpr MS ne(const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::ne(a, b);
    if constexpr (is_float) {
      if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
        if constexpr (is_f32)
          return _mm256_castps_si256(_mm256_cmp_ps(a, b, _CMP_NEQ_UQ));
        else
          return _mm256_castpd_si256(_mm256_cmp_pd(a, b, _CMP_NEQ_UQ));
#endif
      } else {
        if constexpr (is_f32)
          return _mm_castps_si128(_mm_cmpneq_ps(a, b));
        else
          return _mm_castpd_si128(_mm_cmpneq_pd(a, b));
      }
    } else {
      return x86i::ixor(eq(a, b), x86i::ones<MS>());
    }
  }
  static constexpr MS lt(const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::lt(a, b);
    if constexpr (is_float) {
      if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
        if constexpr (is_f32)
          return _mm256_castps_si256(_mm256_cmp_ps(a, b, _CMP_LT_OQ));
        else
          return _mm256_castpd_si256(_mm256_cmp_pd(a, b, _CMP_LT_OQ));
#endif
      } else {
        if constexpr (is_f32)
          return _mm_castps_si128(_mm_cmplt_ps(a, b));
        else
          return _mm_castpd_si128(_mm_cmplt_pd(a, b));
      }
    } else if constexpr (!has_signed_gt) {
      return base::lt(a, b);
    } else if constexpr (is_signed) {
      return signed_gt(b, a);
    } else {
      return signed_gt(flip_sign(b), flip_sign(a));
    }
  }
  static constexpr MS le(const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::le(a, b);
    if constexpr (is_float) {
      if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
        if constexpr (is_f32)
          return _mm256_castps_si256(_mm256_cmp_ps(a, b, _CMP_LE_OQ));
        else
          return _mm256_castpd_si256(_mm256_cmp_pd(a, b, _CMP_LE_OQ));
#endif
      } else {
        if constexpr (is_f32)
          return _mm_castps_si128(_mm_cmple_ps(a, b));
        else
          return _mm_castpd_si128(_mm_cmple_pd(a, b));
      }
    } else if constexpr (!has_signed_gt) {
      return base::le(a, b);
    } else {
      return x86i::ixor(lt(b, a), x86i::ones<MS>());
    }
  }

  static constexpr S select(const MS &k, const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::select(k, a, b);
    return x86i::as<S>(x86i::blend(k, x86i::as_int(a), x86i::as_int(b)));
  }

  static constexpr S min(const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::min(a, b);
    if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
      if constexpr (is_f32)
        return _mm256_min_ps(b, a);
      else if constexpr (is_f64)
        return _mm256_min_pd(b, a);
      else if constexpr (L == 1)
        return is_signed ? _mm256_min_epi8(a, b) : _mm256_min_epu8(a, b);
      else if constexpr (L == 2)
        return is_signed ? _mm256_min_epi16(a, b) : _mm256_min_epu16(a, b);
      else if constexpr (L == 4)
        return is_signed ? _mm256_min_epi32(a, b) : _mm256_min_epu32(a, b);
      else
        return select(lt(b, a), b, a);
#endif
    } else {
      if constexpr (is_f32)
        return _mm_min_ps(b, a);
      else if constexpr (is_f64)
        return _mm_min_pd(b, a);
      else if constexpr (L == 1 && !is_signed)
        return _mm_min_epu8(a, b);
      else if constexpr (L == 2 && is_signed)
        return _mm_min_epi16(a, b);
#if defined(NX_SIMD_X86_SSE41)
      else if constexpr (L == 1)
        return _mm_min_epi8(a, b);
      else if constexpr (L == 2)
        return _mm_min_epu16(a, b);
      else if constexpr (L == 4)
        return is_signed ? _mm_min_epi32(a, b) : _mm_min_epu32(a, b);
#endif
      else
        return select(lt(b, a), b, a);
    }
  }
  static constexpr S max(const S &a, const S &b) {
    if (std::is_constant_evaluated())
      return base::max(a, b);
    if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
      if constexpr (is_f32)
        return _mm256_max_ps(b, a);
      else if constexpr (is_f64)
        return _mm256_max_pd(b, a);
      else if constexpr (L == 1)
        return is_signed ? _mm256_max_epi8(a, b) : _mm256_max_epu8(a, b);
      else if constexpr (L == 2)
        return is_signed ? _mm256_max_epi16(a, b) : _mm256_max_epu16(a, b);
      else if constexpr (L == 4)
        return is_signed ? _mm256_max_epi32(a, b) : _mm256_max_epu32(a, b);
      else
        return select(lt(a, b), b, a);
#endif
    } else {
      if constexpr (is_f32)
        return _mm_max_ps(b, a);
      else if constexpr (is_f64)
        return _mm_max_pd(b, a);
      else if constexpr (L == 1 && !is_signed)
        return _mm_max_epu8(a, b);
      else if constexpr (L == 2 && is_signed)
        return _mm_max_epi16(a, b);
#if defined(NX_SIMD_X86_SSE41)
      else if constexpr (L == 1)
        return _mm_max_epi8(a, b);
      else if constexpr (L == 2)
        return _mm_max_epu16(a, b);
      else if constexpr (L == 4)
        return is_signed ? _mm_max_epi32(a, b) : _mm_max_epu32(a, b);
#endif
      else
        return select(lt(a, b), b, a);
    }
  }

  template <class Op> static S combine(const S &a, const S &b, Op) {
    if constexpr (std::is_same_v<Op, op_add>)
      return add(a, b);
    else if constexpr (std::is_same_v<Op, op_mul>)
      return mul(a, b);
    else if constexpr (std::is_same_v<Op, op_and>)
      return bit_and(a, b);
    else if constexpr (std::is_same_v<Op, op_or>)
      return bit_or(a, b);
    else if constexpr (std::is_same_v<Op, op_xor>)
      return bit_xor(a, b);
    else if constexpr (std::is_same_v<Op, op_min>)
      return min(a, b);
    else
      return max(a, b);
  }
  static T lane0(const S &a) {
    if constexpr (is_f32)
      return _mm_cvtss_f32(a);
    else if constexpr (is_f64)
      return _mm_cvtsd_f64(a);
    else if constexpr (L == 8)
      return static_cast<T>(_mm_cvtsi128_si64(a));
    else
      return static_cast<T>(static_cast<E>(_mm_cvtsi128_si32(a)));
  }
  template <int Shift, class Op> static S fold(const S &a, Op op) {
    if constexpr (Shift < L) {
      return a;
    } else {
      const S down = x86i::as<S>(_mm_srli_si128(x86i::as_int(a), Shift));
      return fold<Shift / 2>(combine(a, down, op), op);
    }
  }
  template <class Op>
    requires(std::is_same_v<Op, op_add> || std::is_same_v<Op, op_mul> ||
             std::is_same_v<Op, op_and> || std::is_same_v<Op, op_or> ||
             std::is_same_v<Op, op_xor> || std::is_same_v<Op, op_min> ||
             std::is_same_v<Op, op_max>)
  static constexpr T reduce(const S &v, Op op) {
    if (std::is_constant_evaluated())
      return base::reduce(v, op);
    if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
      using H = vec_ops<T, abi_x86<16>>;
      typename H::storage lo, hi;
      if constexpr (is_f32) {
        lo = _mm256_castps256_ps128(v);
        hi = _mm256_extractf128_ps(v, 1);
      } else if constexpr (is_f64) {
        lo = _mm256_castpd256_pd128(v);
        hi = _mm256_extractf128_pd(v, 1);
      } else {
        lo = _mm256_castsi256_si128(v);
        hi = _mm256_extracti128_si256(v, 1);
      }
      return H::reduce(H::combine(lo, hi, op), op);
#endif
    } else {
      return lane0(fold<8>(v, op));
    }
  }

  static S sqrt(const S &a) {
    if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
      if constexpr (is_f32)
        return _mm256_sqrt_ps(a);
      else
        return _mm256_sqrt_pd(a);
#endif
    } else {
      if constexpr (is_f32)
        return _mm_sqrt_ps(a);
      else
        return _mm_sqrt_pd(a);
    }
  }
  static S abs(const S &a) {
    if constexpr (is_float) {
      return x86i::as<S>(
          x86i::iandnot(x86i::as_int(broadcast(T(-0.0))), x86i::as_int(a)));
    } else if constexpr (!is_signed) {
      return a;
    } else if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
      if constexpr (L == 1)
        return _mm256_abs_epi8(a);
      else if constexpr (L == 2)
        return _mm256_abs_epi16(a);
      else if constexpr (L == 4)
        return _mm256_abs_epi32(a);
      else
        return select(lt(a, x86i::zero<S>()), neg(a), a);
#endif
    } else {
#if defined(NX_SIMD_X86_SSE41)
      if constexpr (L == 1)
        return _mm_abs_epi8(a);
      else if constexpr (L == 2)
        return _mm_abs_epi16(a);
      else if constexpr (L == 4)
        return _mm_abs_epi32(a);
      else
#endif
        return select(lt(a, x86i::zero<S>()), neg(a), a);
    }
  }

#if defined(NX_SIMD_X86_SSE41)
  template <int Mode> static S round_mode(const S &a) {
    if constexpr (wide) {
#if defined(NX_SIMD_X86_AVX2)
      if constexpr (is_f32)
        return _mm256_round_ps(a, Mode);
      else
        return _mm256_round_pd(a, Mode);
#endif
    } else {
      if constexpr (is_f32)
        return _mm_round_ps(a, Mode);
      else
        return _mm_round_pd(a, Mode);
    }
  }
  static S floor(const S &a) {
    return round_mode<_MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC>(a);
  }
  static S ceil(const S &a) {
    return round_mode<_MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC>(a);
  }
  static S trunc(const S &a) {
    return round_mode<_MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC>(a);
  }
  static S rint(const S &a) { return round_mode<_MM_FROUND_CUR_DIRECTION>(a); }
  // Halfway cases go away from zero, which no rounding mode does; the
  // difference from the truncation is exact.
  static S round(const S &a) {
    const S t = trunc(a);
    const S away = x86i::as<S>(x86i::ior(
        x86i::as_int(broadcast(T(1))),
        x86i::iand(x86i::as_int(a), x86i::as_int(broadcast(T(-0.0))))));
    return select(le(broadcast(T(0.5)), abs(sub(a, t))), add(t, away), t);
  }
#endif

#if defined(NX_SIMD_X86_AVX2)
  static S fma(const S &a, const S &b, const S &c) {
    if constexpr (wide) {
      if constexpr (is_f32)
        return _mm256_fmadd_ps(a, b, c);
      else
        return _mm256_fmadd_pd(a, b, c);
    } else {
      if constexpr (is_f32)
        return _mm_fmadd_ps(a, b, c);
      else
        return _mm_fmadd_pd(a, b, c);
    }
  }
#endif
};

} // namespace nx::simd::detail

#endif
