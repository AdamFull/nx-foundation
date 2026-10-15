#pragma once

#include "core/foundation/simd/backend_arm.h"
#include "core/foundation/simd/backend_generic.h"

#if defined(NX_SIMD_VECTOR_EXT)

#if defined(__SSE2__)
#include <immintrin.h>
#endif

#if defined(__has_builtin)
#define NX_SIMD_HAS_BUILTIN(x) __has_builtin(x)
#else
#define NX_SIMD_HAS_BUILTIN(x) 0
#endif

namespace nx::simd::detail {

template <class E, int Bytes> struct vector_type {
  using type [[gnu::vector_size(Bytes)]] = E;
};
template <class E, int Bytes>
using vector_t = typename vector_type<E, Bytes>::type;

template <class V, int N, int Shift>
NX_FORCE_INLINE constexpr V rotate_down(V x) {
#if NX_SIMD_HAS_BUILTIN(__builtin_shufflevector)
  return [&]<int... I>(std::integer_sequence<int, I...>) {
    return __builtin_shufflevector(x, x, ((I + Shift) % N)...);
  }(std::make_integer_sequence<int, N>{});
#else
  V r{};
  for (int i = 0; i < N; ++i)
    r[i] = x[(i + Shift) % N];
  return r;
#endif
}

template <size_t B, int Bytes>
  requires(B <= 8)
struct mask_ops<B, abi_vector<Bytes>>
    : mask_ops_base<mask_ops<B, abi_vector<Bytes>>, B, Bytes / int(B),
                    vector_t<integer_from<B>, Bytes>> {
  using E = integer_from<B>;
  using S = vector_t<E, Bytes>;
  static constexpr int N = Bytes / int(B);
  using base = mask_ops_base<mask_ops, B, N, S>;

  static constexpr S broadcast(bool b) { return S{} + E(b ? -1 : 0); }
  static constexpr bool get(const S &a, int i) { return a[i] != 0; }
  template <class G> static constexpr S generate(G &&g) {
    return [&]<int... I>(std::integer_sequence<int, I...>) {
      return S{E(static_cast<bool>(g(I)) ? -1 : 0)...};
    }(std::make_integer_sequence<int, N>{});
  }
  static constexpr S land(const S &a, const S &b) { return a & b; }
  static constexpr S lor(const S &a, const S &b) { return a | b; }
  static constexpr S lxor(const S &a, const S &b) { return a ^ b; }
  static constexpr S lnot(const S &a) { return ~a; }
  static constexpr S select(const S &k, const S &a, const S &b) {
    return (k & a) | (~k & b);
  }

  static constexpr u64 to_bits(const S &a) {
    if (std::is_constant_evaluated())
      return base::to_bits(a);
#if defined(__SSE2__)
    if constexpr (Bytes == 16) {
      if constexpr (B == 1)
        return static_cast<u32>(_mm_movemask_epi8((__m128i)a));
      else if constexpr (B == 2)
        return static_cast<u32>(_mm_movemask_epi8(
            _mm_packs_epi16((__m128i)a, _mm_setzero_si128())));
      else if constexpr (B == 4)
        return static_cast<u32>(_mm_movemask_ps((__m128)a));
      else
        return static_cast<u32>(_mm_movemask_pd((__m128d)a));
    }
#endif
#if defined(__AVX2__)
    if constexpr (Bytes == 32) {
      if constexpr (B == 1)
        return static_cast<u32>(_mm256_movemask_epi8((__m256i)a));
      else if constexpr (B == 4)
        return static_cast<u32>(_mm256_movemask_ps((__m256)a));
      else if constexpr (B == 8)
        return static_cast<u32>(_mm256_movemask_pd((__m256d)a));
    }
#endif
#if defined(NX_SIMD_NEON)
    if constexpr (Bytes == 16) {
      if constexpr (B == 1) {
        const uint8x16_t bits =
            vandq_u8((uint8x16_t)a, uint8x16_t{1, 2, 4, 8, 16, 32, 64, 128, 1,
                                               2, 4, 8, 16, 32, 64, 128});
        return u64{vaddv_u8(vget_low_u8(bits))} |
               (u64{vaddv_u8(vget_high_u8(bits))} << 8);
      } else if constexpr (B == 2) {
        return vaddvq_u16(
            vandq_u16((uint16x8_t)a, uint16x8_t{1, 2, 4, 8, 16, 32, 64, 128}));
      } else if constexpr (B == 4) {
        return vaddvq_u32(vandq_u32((uint32x4_t)a, uint32x4_t{1, 2, 4, 8}));
      } else {
        return vaddvq_u64(vandq_u64((uint64x2_t)a, uint64x2_t{1, 2}));
      }
    }
#endif
    if constexpr (Bytes > 16) {
#if NX_SIMD_HAS_BUILTIN(__builtin_shufflevector)
      using H = mask_ops<B, abi_vector<Bytes / 2>>;
      const auto half = [&]<int... I>(std::integer_sequence<int, I...>) {
        return typename H::storage(__builtin_shufflevector(a, a, I...));
      };
      const auto lo = half(std::make_integer_sequence<int, N / 2>{});
      const auto hi = [&]<int... I>(std::integer_sequence<int, I...>) {
        return half(std::integer_sequence<int, (I + N / 2)...>{});
      }(std::make_integer_sequence<int, N / 2>{});
      return H::to_bits(lo) | (H::to_bits(hi) << (N / 2));
#endif
    }
    return base::to_bits(a);
  }

  static constexpr bool all_of(const S &a) {
#if defined(NX_SIMD_NEON)
    if constexpr (Bytes == 16)
      if (!std::is_constant_evaluated())
        return neon::all_of(a);
#endif
    return base::all_of(a);
  }
  static constexpr bool any_of(const S &a) {
#if defined(NX_SIMD_NEON)
    if constexpr (Bytes == 16)
      if (!std::is_constant_evaluated())
        return neon::any_of(a);
#endif
    return base::any_of(a);
  }

  static constexpr S from_bits(u64 bits) {
#if defined(NX_SIMD_NEON)
    if constexpr (Bytes == 16)
      if (!std::is_constant_evaluated())
        return neon::from_bits<B, S>(bits);
#endif
    if constexpr (N > int(8 * B)) {
      return base::from_bits(bits);
    } else {
      const S lane_bits = [&]<int... I>(std::integer_sequence<int, I...>) {
        return S{E(u64{1} << I)...};
      }(std::make_integer_sequence<int, N>{});
      return (S)(((S{} + E(bits)) & lane_bits) != 0);
    }
  }
};

template <int Bytes>
struct mask_ops<16, abi_vector<Bytes>> : mask_ops_lanes<16, Bytes / 16> {};

template <class T, int Bytes>
  requires vectorizable_arithmetic<T>
struct vec_ops<T, abi_vector<Bytes>>
    : vec_ops_base<vec_ops<T, abi_vector<Bytes>>, T, Bytes / int(sizeof(T)),
                   vector_t<canonical_t<T>, Bytes>,
                   mask_ops<sizeof(T), abi_vector<Bytes>>> {
  using E = canonical_t<T>;
  using S = vector_t<E, Bytes>;
  using M = mask_ops<sizeof(T), abi_vector<Bytes>>;
  using MS = typename M::storage;
  static constexpr int N = Bytes / int(sizeof(T));
  using base = vec_ops_base<vec_ops, T, N, S, M>;
  using base::reduce;
  static constexpr bool is_float = std::is_floating_point_v<T>;
  static constexpr bool narrow = !is_float && sizeof(T) < 4;
  // The 32-bit lanes that 8- and 16-bit elements promote to in scalar code.
  // Widened values stay inside one expression: a function returning a vector
  // wider than the target's registers changes the calling convention.
  using W = vector_t<std::conditional_t<std::is_signed_v<E>, i32, u32>, N * 4>;
#if defined(NX_SIMD_NEON)
  static constexpr bool neon_q = Bytes == 16;
#else
  static constexpr bool neon_q = false;
#endif
#if defined(NX_SIMD_SVE)
  static constexpr bool sve_ok = sve::fits<Bytes>;
#else
  static constexpr bool sve_ok = false;
#endif

  static constexpr S broadcast(T x) { return S{} + static_cast<E>(x); }
  static constexpr T get(const S &a, int i) { return static_cast<T>(a[i]); }
  template <class G> static constexpr S generate(G &&g) {
    return [&]<int... I>(std::integer_sequence<int, I...>) {
      return S{static_cast<E>(static_cast<T>(g(I)))...};
    }(std::make_integer_sequence<int, N>{});
  }

  template <class U> static constexpr S load(const U *p) {
    if (std::is_constant_evaluated())
      return base::load(p);
    if constexpr (std::is_same_v<canonical_t<U>, E>) {
      S r;
      std::memcpy(&r, p, sizeof r);
      return r;
    } else if constexpr (vectorizable_arithmetic<U>) {
      vector_t<canonical_t<U>, int(sizeof(U)) * N> u;
      std::memcpy(&u, p, sizeof u);
      return __builtin_convertvector(u, S);
    } else {
      return base::load(p);
    }
  }
  template <class U> static constexpr void store(const S &v, U *p) {
    if (std::is_constant_evaluated())
      return base::store(v, p);
    if constexpr (std::is_same_v<canonical_t<U>, E>) {
      std::memcpy(p, &v, sizeof v);
    } else if constexpr (vectorizable_arithmetic<U>) {
      using UV = vector_t<canonical_t<U>, int(sizeof(U)) * N>;
      const UV u = __builtin_convertvector(v, UV);
      std::memcpy(p, &u, sizeof u);
    } else {
      base::store(v, p);
    }
  }
  template <class U> static constexpr S load_n(const U *p, int n) {
    if (std::is_constant_evaluated() || n <= 0)
      return base::load_n(p, n);
#if defined(NX_SIMD_SVE)
    if constexpr (sve_ok && std::is_same_v<canonical_t<U>, E>)
      return sve::load_n<E, S>(reinterpret_cast<const E *>(p), n);
#endif
    if (n >= N)
      return load(p);
    U buffer[N] = {};
    std::memcpy(buffer, p, sizeof(U) * static_cast<size_t>(n));
    return load(buffer);
  }
  template <class U> static constexpr void store_n(const S &v, U *p, int n) {
    if (std::is_constant_evaluated() || n <= 0)
      return base::store_n(v, p, n);
#if defined(NX_SIMD_SVE)
    if constexpr (sve_ok && std::is_same_v<canonical_t<U>, E>)
      return sve::store_n<E, S>(v, reinterpret_cast<E *>(p), n);
#endif
    if (n >= N)
      return store(v, p);
    U buffer[N];
    store(v, buffer);
    std::memcpy(p, buffer, sizeof(U) * static_cast<size_t>(n));
  }

  template <class U>
  static constexpr S load_masked(const U *p, const MS &k, int n) {
#if defined(NX_SIMD_SVE)
    if constexpr (sve_ok && std::is_same_v<canonical_t<U>, E>)
      if (!std::is_constant_evaluated())
        return sve::load_masked<E, S>(reinterpret_cast<const E *>(p), k, n);
#endif
    return base::load_masked(p, k, n);
  }
  template <class U>
  static constexpr void store_masked(const S &v, U *p, const MS &k, int n) {
#if defined(NX_SIMD_SVE)
    if constexpr (sve_ok && std::is_same_v<canonical_t<U>, E>)
      if (!std::is_constant_evaluated())
        return sve::store_masked<E, S>(v, reinterpret_cast<E *>(p), k, n);
#endif
    base::store_masked(v, p, k, n);
  }

  template <class U, class UAbi>
  static constexpr S convert(const vec_storage<U, UAbi> &u) {
    if constexpr (std::is_same_v<UAbi, abi_vector<int(sizeof(U)) * N>>)
      return __builtin_convertvector(u, S);
    else
      return base::template convert<U, UAbi>(u);
  }

  static constexpr S neg(const S &a) { return -a; }
  static constexpr S bit_not(const S &a) { return ~a; }
  static constexpr S add(const S &a, const S &b) { return a + b; }
  static constexpr S sub(const S &a, const S &b) { return a - b; }
  static constexpr S mul(const S &a, const S &b) { return a * b; }
  static constexpr S div(const S &a, const S &b) {
    if constexpr (narrow)
      return __builtin_convertvector(
          __builtin_convertvector(a, W) / __builtin_convertvector(b, W), S);
    else
      return a / b;
  }
  static constexpr S mod(const S &a, const S &b) {
    if constexpr (narrow)
      return __builtin_convertvector(
          __builtin_convertvector(a, W) % __builtin_convertvector(b, W), S);
    else
      return a % b;
  }
  static constexpr S bit_and(const S &a, const S &b) { return a & b; }
  static constexpr S bit_or(const S &a, const S &b) { return a | b; }
  static constexpr S bit_xor(const S &a, const S &b) { return a ^ b; }
  static constexpr S shl(const S &a, const S &b) {
    if constexpr (narrow)
      return __builtin_convertvector(
          __builtin_convertvector(a, W) << __builtin_convertvector(b, W), S);
    else
      return a << b;
  }
  static constexpr S shr(const S &a, const S &b) {
    if constexpr (narrow)
      return __builtin_convertvector(
          __builtin_convertvector(a, W) >> __builtin_convertvector(b, W), S);
    else
      return a >> b;
  }
  static constexpr S shl_n(const S &a, int n) {
    if constexpr (narrow)
      return __builtin_convertvector(__builtin_convertvector(a, W) << n, S);
    else
      return a << n;
  }
  static constexpr S shr_n(const S &a, int n) {
    if constexpr (narrow)
      return __builtin_convertvector(__builtin_convertvector(a, W) >> n, S);
    else
      return a >> n;
  }

  static constexpr MS eq(const S &a, const S &b) { return (MS)(a == b); }
  static constexpr MS ne(const S &a, const S &b) { return (MS)(a != b); }
  static constexpr MS lt(const S &a, const S &b) { return (MS)(a < b); }
  static constexpr MS le(const S &a, const S &b) { return (MS)(a <= b); }

  static constexpr S select(const MS &k, const S &a, const S &b) {
    return (S)((k & (MS)a) | (~k & (MS)b));
  }
  static constexpr S min(const S &a, const S &b) {
    return select(lt(b, a), b, a);
  }
  static constexpr S max(const S &a, const S &b) {
    return select(lt(a, b), b, a);
  }

  template <int Width, class Op> static constexpr S fold(const S &x, Op op) {
    if constexpr (Width == 0) {
      return x;
    } else {
      const S h = rotate_down<S, N, Width>(x);
      S r;
      if constexpr (std::is_same_v<Op, op_add>)
        r = x + h;
      else if constexpr (std::is_same_v<Op, op_mul>)
        r = x * h;
      else if constexpr (std::is_same_v<Op, op_and>)
        r = x & h;
      else if constexpr (std::is_same_v<Op, op_or>)
        r = x | h;
      else if constexpr (std::is_same_v<Op, op_xor>)
        r = x ^ h;
      else if constexpr (std::is_same_v<Op, op_min>)
        r = min(x, h);
      else
        r = max(x, h);
      return fold<Width / 2>(r, op);
    }
  }
  template <class Op>
    requires(std::is_same_v<Op, op_add> || std::is_same_v<Op, op_mul> ||
             std::is_same_v<Op, op_and> || std::is_same_v<Op, op_or> ||
             std::is_same_v<Op, op_xor> || std::is_same_v<Op, op_min> ||
             std::is_same_v<Op, op_max>)
  static constexpr T reduce(const S &v, Op op) {
    if (!std::is_constant_evaluated()) {
#if defined(NX_SIMD_NEON)
      if constexpr (neon_q && std::is_same_v<Op, op_add>)
        return static_cast<T>(neon::add_across<E>(neon::q<E>(v)));
      else if constexpr (neon_q && std::is_same_v<Op, op_min> &&
                         neon::has_minmax_across<E>)
        return static_cast<T>(neon::min_across<E>(neon::q<E>(v)));
      else if constexpr (neon_q && std::is_same_v<Op, op_max> &&
                         neon::has_minmax_across<E>)
        return static_cast<T>(neon::max_across<E>(neon::q<E>(v)));
#endif
#if defined(NX_SIMD_SVE)
      if constexpr (sve_ok && !neon_q && !std::is_same_v<Op, op_mul> &&
                    !(is_float && (std::is_same_v<Op, op_and> ||
                                   std::is_same_v<Op, op_or> ||
                                   std::is_same_v<Op, op_xor>)))
        return static_cast<T>(sve::reduce<E>(v, op));
#endif
    }
    return get(fold<N / 2>(v, op), 0);
  }

  static S sqrt(const S &a) {
#if defined(NX_SIMD_NEON)
    if constexpr (neon_q)
      return neon::sqrt<E>(a);
#endif
#if NX_SIMD_HAS_BUILTIN(__builtin_elementwise_sqrt)
    return __builtin_elementwise_sqrt(a);
#else
    return base::sqrt(a);
#endif
  }
  static S floor(const S &a) {
#if defined(NX_SIMD_NEON)
    if constexpr (neon_q)
      return neon::floor<E>(a);
#endif
#if NX_SIMD_HAS_BUILTIN(__builtin_elementwise_floor)
    return __builtin_elementwise_floor(a);
#else
    return base::floor(a);
#endif
  }
  static S ceil(const S &a) {
#if defined(NX_SIMD_NEON)
    if constexpr (neon_q)
      return neon::ceil<E>(a);
#endif
#if NX_SIMD_HAS_BUILTIN(__builtin_elementwise_ceil)
    return __builtin_elementwise_ceil(a);
#else
    return base::ceil(a);
#endif
  }
  static S trunc(const S &a) {
#if defined(NX_SIMD_NEON)
    if constexpr (neon_q)
      return neon::trunc<E>(a);
#endif
#if NX_SIMD_HAS_BUILTIN(__builtin_elementwise_trunc)
    return __builtin_elementwise_trunc(a);
#else
    return base::trunc(a);
#endif
  }
  static S round(const S &a) {
#if defined(NX_SIMD_NEON)
    if constexpr (neon_q)
      return neon::round<E>(a);
#endif
#if NX_SIMD_HAS_BUILTIN(__builtin_elementwise_round)
    return __builtin_elementwise_round(a);
#else
    return base::round(a);
#endif
  }
  static S rint(const S &a) {
#if defined(NX_SIMD_NEON)
    if constexpr (neon_q)
      return neon::rint<E>(a);
#endif
#if NX_SIMD_HAS_BUILTIN(__builtin_elementwise_rint)
    return __builtin_elementwise_rint(a);
#else
    return base::rint(a);
#endif
  }
  static S fma(const S &a, const S &b, const S &c) {
#if defined(NX_SIMD_NEON)
    if constexpr (neon_q)
      return neon::fma<E>(a, b, c);
#endif
#if NX_SIMD_HAS_BUILTIN(__builtin_elementwise_fma)
    return __builtin_elementwise_fma(a, b, c);
#else
    return base::fma(a, b, c);
#endif
  }
  static constexpr S fmin(const S &a, const S &b) {
#if defined(NX_SIMD_NEON)
    if constexpr (neon_q)
      if (!std::is_constant_evaluated())
        return neon::fmin<E>(a, b);
#endif
    return base::fmin(a, b);
  }
  static constexpr S fmax(const S &a, const S &b) {
#if defined(NX_SIMD_NEON)
    if constexpr (neon_q)
      if (!std::is_constant_evaluated())
        return neon::fmax<E>(a, b);
#endif
    return base::fmax(a, b);
  }

  static constexpr S popcount(const S &a) {
    if (!std::is_constant_evaluated()) {
#if defined(NX_SIMD_NEON)
      if constexpr (neon_q)
        return neon::popcount<sizeof(E)>(a);
#endif
#if NX_SIMD_HAS_BUILTIN(__builtin_elementwise_popcount)
      return __builtin_elementwise_popcount(a);
#endif
    }
    return base::popcount(a);
  }
  static constexpr S countl_zero(const S &a) {
#if defined(NX_SIMD_NEON)
    if constexpr (neon_q)
      if (!std::is_constant_evaluated())
        return neon::countl_zero<sizeof(E)>(a);
#endif
    return base::countl_zero(a);
  }
  static constexpr S byteswap(const S &a) {
#if defined(NX_SIMD_NEON)
    if constexpr (neon_q)
      if (!std::is_constant_evaluated())
        return neon::byteswap<sizeof(E)>(a);
#endif
    return base::byteswap(a);
  }
  static constexpr S bit_reverse(const S &a) {
    if (!std::is_constant_evaluated()) {
#if defined(NX_SIMD_NEON)
      if constexpr (neon_q)
        return neon::bit_reverse<sizeof(E)>(a);
#endif
#if NX_SIMD_HAS_BUILTIN(__builtin_elementwise_bitreverse)
      return __builtin_elementwise_bitreverse(a);
#endif
    }
    return base::bit_reverse(a);
  }
  static constexpr S bit_compress(const S &a, const S &m) {
#if defined(NX_SIMD_SVE) && defined(__ARM_FEATURE_SVE2_BITPERM)
    if constexpr (sve_ok)
      if (!std::is_constant_evaluated())
        return sve::bit_compress<E>(a, m);
#endif
    return base::bit_compress(a, m);
  }
  static constexpr S bit_expand(const S &a, const S &m) {
#if defined(NX_SIMD_SVE) && defined(__ARM_FEATURE_SVE2_BITPERM)
    if constexpr (sve_ok)
      if (!std::is_constant_evaluated())
        return sve::bit_expand<E>(a, m);
#endif
    return base::bit_expand(a, m);
  }

  static constexpr bool fast_compress = sve_ok && sizeof(E) >= 4;
  static constexpr S compress(const S &v, const MS &k) {
#if defined(NX_SIMD_SVE)
    if constexpr (fast_compress)
      if (!std::is_constant_evaluated())
        return sve::compress<E>(v, k);
#endif
    return base::compress(v, k);
  }

  // Hardware gathers and scatters take indices as wide as the elements.
  template <class I>
  static constexpr bool gathers =
      sve_ok && sizeof(E) >= 4 &&
      std::is_same_v<typename I::abi_type, abi_vector<Bytes>> &&
      sizeof(typename I::value_type) == sizeof(E);
#if defined(NX_SIMD_SVE)
  template <class IS>
  static S gather(const E *base, const IS &indices, const MS &active) {
    return sve::gather<E, S>(base, indices, active);
  }
  template <class IS>
  static void scatter(E *base, const IS &indices, const MS &active,
                      const S &v) {
    sve::scatter<E, S>(base, indices, active, v);
  }
#endif

  static S abs(const S &a) {
    if constexpr (is_float) {
      using L = integer_from<sizeof(T)>;
      using I = vector_t<L, Bytes>;
      return (S)((I)a & ~(I{} + std::numeric_limits<L>::min()));
    } else if constexpr (std::is_unsigned_v<E>) {
      return a;
    } else {
      return select(lt(a, S{}), -a, a);
    }
  }
};

} // namespace nx::simd::detail

#endif
