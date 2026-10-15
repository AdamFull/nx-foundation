#pragma once

#include "core/foundation/core/foundation.h"

#include <bit>
#include <complex>
#include <concepts>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

// Backend selection. NX_SIMD_FORCE_FIXED builds every type on plain arrays,
// which is the reference the register backends are tested against.
#if !defined(NX_SIMD_FORCE_FIXED)
#if defined(__GNUC__) || defined(__clang__)
#define NX_SIMD_VECTOR_EXT 1
#elif defined(NX_COMPILER_MSVC) && defined(NX_ARCH_X64)
#define NX_SIMD_X86 1
#include <immintrin.h>
#endif
#endif

#if defined(NX_SIMD_X86)
#if defined(__AVX2__)
#define NX_SIMD_X86_AVX2 1
#endif
#if defined(__AVX__) || defined(__SSE4_1__)
#define NX_SIMD_X86_SSE41 1
#endif
#endif

namespace nx::simd {

template <size_t Bytes, class Abi> class basic_mask;
template <class T, class Abi> class basic_vec;

namespace detail {

using size_type = int;

inline constexpr size_type max_lanes = 64;

template <class T> struct is_complex : std::false_type {};
template <class F> struct is_complex<std::complex<F>> : std::true_type {};
template <class T> inline constexpr bool is_complex_v = is_complex<T>::value;

template <class T>
concept vectorizable_arithmetic =
    std::is_same_v<T, std::remove_cv_t<T>> &&
    ((std::is_integral_v<T> && !std::is_same_v<T, bool>) ||
     std::is_same_v<T, float> || std::is_same_v<T, double>);

template <class T>
concept vectorizable =
    vectorizable_arithmetic<T> || std::is_same_v<T, std::complex<float>> ||
    std::is_same_v<T, std::complex<double>>;

template <class T> struct complex_value {
  using type = T;
};
template <class F> struct complex_value<std::complex<F>> {
  using type = F;
};

template <size_t Bytes> struct signed_of_size;
template <> struct signed_of_size<1> {
  using type = i8;
};
template <> struct signed_of_size<2> {
  using type = i16;
};
template <> struct signed_of_size<4> {
  using type = i32;
};
template <> struct signed_of_size<8> {
  using type = i64;
};
template <size_t Bytes>
using integer_from = typename signed_of_size<Bytes>::type;

// The fixed-width type a lane is stored as: char types, long and wchar_t
// share storage with the integer of their size and signedness.
template <class T> struct canonical {
  using type = T;
};
template <class T>
  requires std::is_integral_v<T>
struct canonical<T> {
  using type =
      std::conditional_t<std::is_signed_v<T>, integer_from<sizeof(T)>,
                         std::make_unsigned_t<integer_from<sizeof(T)>>>;
};
template <class T> using canonical_t = typename canonical<T>::type;

struct lane16 {
  i64 lo, hi;
};
template <size_t Bytes> struct mask_lane {
  using type = integer_from<Bytes>;
};
template <> struct mask_lane<16> {
  using type = lane16;
};
template <size_t Bytes> using mask_lane_t = typename mask_lane<Bytes>::type;

template <size_t Bytes>
inline constexpr bool mask_bytes_enabled =
    Bytes == 1 || Bytes == 2 || Bytes == 4 || Bytes == 8 || Bytes == 16;

template <class From, class To> consteval bool value_preserving() {
  if constexpr (std::is_same_v<From, To>) {
    return true;
  } else if constexpr (is_complex_v<To>) {
    return value_preserving<typename complex_value<From>::type,
                            typename To::value_type>();
  } else if constexpr (is_complex_v<From>) {
    return false;
  } else {
    using F = std::numeric_limits<From>;
    using T = std::numeric_limits<To>;
    if constexpr (std::is_integral_v<From> && std::is_integral_v<To>) {
      if constexpr (F::is_signed && !T::is_signed)
        return false;
      return F::digits <= T::digits;
    } else if constexpr (std::is_integral_v<From>) {
      return F::digits <= T::digits;
    } else if constexpr (std::is_integral_v<To>) {
      return false;
    } else {
      return F::digits <= T::digits && F::max_exponent <= T::max_exponent &&
             F::min_exponent >= T::min_exponent;
    }
  }
}

template <class T> consteval int integer_rank() {
  if constexpr (std::is_same_v<T, signed char> ||
                std::is_same_v<T, unsigned char> || std::is_same_v<T, char>)
    return 1;
  else if constexpr (std::is_same_v<T, short> ||
                     std::is_same_v<T, unsigned short>)
    return 2;
  else if constexpr (std::is_same_v<T, int> || std::is_same_v<T, unsigned>)
    return 3;
  else if constexpr (std::is_same_v<T, long> ||
                     std::is_same_v<T, unsigned long>)
    return 4;
  else if constexpr (std::is_same_v<T, long long> ||
                     std::is_same_v<T, unsigned long long>)
    return 5;
  else
    return sizeof(T) == 1 ? 1 : sizeof(T) == 2 ? 2 : sizeof(T) == 4 ? 3 : 5;
}

template <class From, class To> consteval bool higher_rank() {
  using F = typename complex_value<From>::type;
  using T = typename complex_value<To>::type;
  if constexpr (std::is_integral_v<F> && std::is_integral_v<T>)
    return integer_rank<F>() > integer_rank<T>();
  else if constexpr (std::is_floating_point_v<F> && std::is_floating_point_v<T>)
    return sizeof(F) > sizeof(T);
  else
    return false;
}

template <class From, class To>
concept explicitly_convertible_to =
    requires { static_cast<To>(std::declval<From>()); };

template <class T>
concept constexpr_wrapper_like =
    std::convertible_to<T, decltype(T::value)> &&
    std::equality_comparable_with<T, decltype(T::value)> &&
    std::bool_constant<T() == T::value>::value &&
    std::bool_constant<static_cast<decltype(T::value)>(T()) == T::value>::value;

template <class To, class From> constexpr bool representable(From v) {
  if constexpr (is_complex_v<To>) {
    return representable<typename To::value_type>(v);
  } else if constexpr (std::is_integral_v<To> && std::is_integral_v<From>) {
    return std::in_range<To>(v);
  } else if constexpr (std::is_integral_v<To>) {
    if (!(v >= static_cast<From>(std::numeric_limits<To>::min()) &&
          v <= static_cast<From>(std::numeric_limits<To>::max())))
      return false;
    return static_cast<From>(static_cast<To>(v)) == v;
  } else if constexpr (std::is_integral_v<From>) {
    return static_cast<From>(static_cast<To>(v)) == v &&
           static_cast<To>(v) <= std::numeric_limits<To>::max();
  } else {
    return v != v || static_cast<From>(static_cast<To>(v)) == v;
  }
}

template <class From, class To>
concept broadcast_from =
    (!std::is_arithmetic_v<From> && !constexpr_wrapper_like<From> &&
     std::convertible_to<From, To>) ||
    (std::is_arithmetic_v<From> && value_preserving<From, To>()) ||
    (constexpr_wrapper_like<From> &&
     std::is_arithmetic_v<std::remove_cvref_t<decltype(From::value)>> &&
     representable<To>(From::value));

// ABI tags. A tag gives each element type a lane count; basic_vec<T, Abi>
// and basic_mask<sizeof(T), Abi> always agree on it.
template <int N> struct abi_fixed {
  template <class T> static constexpr int size = N;
  template <class T> static constexpr bool supports = N >= 1 && N <= max_lanes;
};

template <class Part, int Count> struct abi_multi {
  using part = Part;
  static constexpr int count = Count;
  template <class T> static constexpr int size = Count * Part::template size<T>;
  template <class T>
  static constexpr bool supports = Count >= 2 && Part::template supports<T> &&
                                   Count * Part::template size<T> <= max_lanes;
};

#if defined(NX_SIMD_VECTOR_EXT)
template <int Bytes> struct abi_vector {
  template <class T>
  static constexpr int size = Bytes / static_cast<int>(sizeof(T));
  template <class T>
  static constexpr bool supports =
      Bytes % sizeof(T) == 0 && size<T> >= 2 && size<T> <= max_lanes &&
      std::has_single_bit(static_cast<unsigned>(Bytes));
};

template <class T> consteval int native_bytes() {
#if defined(__AVX512F__)
  using E = typename complex_value<T>::type;
  if constexpr (sizeof(E) < 4) {
#if defined(__AVX512BW__)
    return 64;
#else
    return 32;
#endif
  }
  return 64;
#elif defined(__AVX2__)
  return 32;
#elif defined(__AVX__)
  return std::is_floating_point_v<E> ? 32 : 16;
#elif defined(__ARM_FEATURE_SVE_BITS) && __ARM_FEATURE_SVE_BITS > 128
  return __ARM_FEATURE_SVE_BITS / 8;
#elif defined(__SSE2__) || defined(__ARM_NEON) || defined(__ARM_NEON__) ||     \
    defined(__wasm_simd128__) || defined(__ALTIVEC__) ||                       \
    defined(__riscv_vector)
  return 16;
#else
  return 0;
#endif
}

template <class T> consteval int register_bytes(int bytes) {
  if (bytes < 2 * static_cast<int>(sizeof(T)) ||
      !std::has_single_bit(static_cast<unsigned>(bytes)))
    return 0;
  return bytes;
}
#endif

#if defined(NX_SIMD_X86)
template <int Bytes> struct abi_x86 {
  template <class T>
  static constexpr int size = Bytes / static_cast<int>(sizeof(T));
  template <class T>
  static constexpr bool supports = (Bytes == 16
#if defined(NX_SIMD_X86_AVX2)
                                    || Bytes == 32
#endif
                                    ) &&
                                   size<T> >= 1;
};

template <class T> consteval int native_bytes() {
#if defined(NX_SIMD_X86_AVX2)
  return 32;
#else
  return 16;
#endif
}

template <class T> consteval int register_bytes(int bytes) {
  return bytes == 16 || bytes == native_bytes<T>() ? bytes : 0;
}
#endif

#if !defined(NX_SIMD_VECTOR_EXT) && !defined(NX_SIMD_X86)
template <class T> consteval int native_bytes() { return 0; }
#endif

#if defined(NX_SIMD_VECTOR_EXT)
template <int Bytes> using abi_register = abi_vector<Bytes>;
#elif defined(NX_SIMD_X86)
template <int Bytes> using abi_register = abi_x86<Bytes>;
#endif

// One register when the lanes fill one exactly, an array of registers when
// they fill several, plain arrays otherwise.
template <class T, int N> struct deduce_abi {
  template <int R> static consteval auto split() {
#if defined(NX_SIMD_VECTOR_EXT) || defined(NX_SIMD_X86)
    constexpr int bytes = N * static_cast<int>(sizeof(T));
    if constexpr (R > 0 && bytes > R && bytes % R == 0 &&
                  abi_multi<abi_register<R>, bytes / R>::template supports<T>)
      return abi_multi<abi_register<R>, bytes / R>{};
    else
      return static_cast<void *>(nullptr);
#else
    return static_cast<void *>(nullptr);
#endif
  }

  static consteval auto pick() {
    constexpr int native = native_bytes<T>();
    if constexpr (N < 1 || N > max_lanes) {
      return static_cast<void *>(nullptr);
    } else if constexpr (native == 0 || N == 1) {
      return abi_fixed<N>{};
    } else {
#if defined(NX_SIMD_VECTOR_EXT) || defined(NX_SIMD_X86)
      using E = typename complex_value<T>::type;
      constexpr int bytes = N * static_cast<int>(sizeof(T));
      if constexpr (register_bytes<E>(bytes) != 0 && bytes <= native &&
                    abi_register<register_bytes<E>(
                        bytes)>::template supports<T>)
        return abi_register<register_bytes<E>(bytes)>{};
      else if constexpr (!std::is_same_v<decltype(split<native>()), void *>)
        return split<native>();
      else if constexpr (!std::is_same_v<decltype(split<16>()), void *>)
        return split<16>();
      else
        return abi_fixed<N>{};
#else
      return abi_fixed<N>{};
#endif
    }
  }
  using type = decltype(pick());
};

template <class T, int N>
  requires(!std::is_same_v<typename deduce_abi<T, N>::type, void *>)
using deduce_abi_t = typename deduce_abi<T, N>::type;

template <class T>
using native_abi =
    deduce_abi_t<T, native_bytes<T>() == 0 ? 1
                    : native_bytes<T>() / static_cast<int>(sizeof(T)) < 1
                        ? 1
                        : native_bytes<T>() / static_cast<int>(sizeof(T))>;

template <class T, class Abi> consteval int simd_size() {
  if constexpr (vectorizable<T> && Abi::template supports<T>)
    return Abi::template size<T>;
  else
    return 0;
}

template <size_t Bytes, class Abi> consteval int mask_size() {
  if constexpr (!mask_bytes_enabled<Bytes>)
    return 0;
  else if constexpr (Bytes == 16)
    return simd_size<std::complex<double>, Abi>();
  else if constexpr (Bytes == 8)
    return simd_size<i64, Abi>();
  else
    return simd_size<integer_from<Bytes>, Abi>();
}

template <class T> NX_FORCE_INLINE constexpr T lane_to_value(bool b) {
  if constexpr (std::is_same_v<T, lane16>)
    return b ? lane16{-1, -1} : lane16{0, 0};
  else
    return b ? T(-1) : T(0);
}

template <class T> NX_FORCE_INLINE constexpr bool lane_to_bool(T v) {
  if constexpr (std::is_same_v<T, lane16>)
    return v.lo != 0;
  else
    return v != 0;
}

struct private_init_t {};
inline constexpr private_init_t private_init{};

} // namespace detail
} // namespace nx::simd
