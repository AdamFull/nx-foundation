#pragma once

#include "framework/nxtest.h"

#include "core/foundation/simd/simd.h"

#include <bit>
#include <cmath>
#include <complex>
#include <cstdio>
#include <limits>
#include <string>
#include <type_traits>

namespace sd = nx::simd;

namespace simd_test {

template <class T> constexpr const char *name_of() {
  if constexpr (std::is_same_v<T, char>)
    return "char";
  else if constexpr (std::is_same_v<T, signed char>)
    return "signed char";
  else if constexpr (std::is_same_v<T, unsigned char>)
    return "unsigned char";
  else if constexpr (std::is_same_v<T, short>)
    return "short";
  else if constexpr (std::is_same_v<T, unsigned short>)
    return "unsigned short";
  else if constexpr (std::is_same_v<T, int>)
    return "int";
  else if constexpr (std::is_same_v<T, unsigned>)
    return "unsigned";
  else if constexpr (std::is_same_v<T, long>)
    return "long";
  else if constexpr (std::is_same_v<T, unsigned long>)
    return "unsigned long";
  else if constexpr (std::is_same_v<T, long long>)
    return "long long";
  else if constexpr (std::is_same_v<T, unsigned long long>)
    return "unsigned long long";
  else if constexpr (std::is_same_v<T, float>)
    return "float";
  else if constexpr (std::is_same_v<T, double>)
    return "double";
  else if constexpr (std::is_same_v<T, wchar_t>)
    return "wchar_t";
  else if constexpr (std::is_same_v<T, char8_t>)
    return "char8_t";
  else if constexpr (std::is_same_v<T, char16_t>)
    return "char16_t";
  else if constexpr (std::is_same_v<T, char32_t>)
    return "char32_t";
  else if constexpr (std::is_same_v<T, std::complex<float>>)
    return "complex<float>";
  else if constexpr (std::is_same_v<T, std::complex<double>>)
    return "complex<double>";
  else if constexpr (std::is_same_v<T, bool>)
    return "bool";
  else
    return "?";
}

template <class T> std::string show(T v) {
  if constexpr (sd::detail::is_complex_v<T>) {
    return "(" + show(v.real()) + ", " + show(v.imag()) + ")";
  } else if constexpr (std::is_floating_point_v<T>) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.17g", static_cast<double>(v));
    return buffer;
  } else if constexpr (std::is_same_v<T, bool>) {
    return v ? "true" : "false";
  } else if constexpr (std::is_signed_v<T>) {
    return std::to_string(static_cast<long long>(v));
  } else {
    return std::to_string(static_cast<unsigned long long>(v));
  }
}

// Floating-point lanes compare by bits, so -0.0 and +0.0 differ; any two
// NaNs are equal.
template <class T> bool same(T a, T b) {
  if constexpr (sd::detail::is_complex_v<T>) {
    return same(a.real(), b.real()) && same(a.imag(), b.imag());
  } else if constexpr (std::is_floating_point_v<T>) {
    using I = sd::detail::integer_from<sizeof(T)>;
    return (a != a && b != b) || std::bit_cast<I>(a) == std::bit_cast<I>(b);
  } else {
    return a == b;
  }
}

template <class V, class F>
bool check_lanes(const char *what, const V &v, F &&expected) {
  using T = typename V::value_type;
  for (int i = 0; i < V::size(); ++i) {
    const T e = static_cast<T>(expected(i));
    const T got = v[i];
    if (!same(got, e)) {
      ::nxtest::report_failure(__FILE__, __LINE__, "LANES", what,
                               std::string(name_of<T>()) + " x" +
                                   std::to_string(V::size()) + ", lane " +
                                   std::to_string(i) + ": got " + show(got) +
                                   ", expected " + show(e));
      return false;
    }
  }
  ::nxtest::report_success();
  return true;
}

template <class M, class F>
bool check_mask(const char *what, const M &m, F &&expected) {
  for (int i = 0; i < M::size(); ++i) {
    const bool e = static_cast<bool>(expected(i));
    if (m[i] != e) {
      ::nxtest::report_failure(__FILE__, __LINE__, "MASK", what,
                               "mask x" + std::to_string(M::size()) +
                                   ", lane " + std::to_string(i) + ": got " +
                                   show(m[i]) + ", expected " + show(e));
      return false;
    }
  }
  ::nxtest::report_success();
  return true;
}

template <class T> struct rng {
  u64 state;

  explicit rng(u64 seed) : state(seed * 0x9e3779b97f4a7c15ull + 1) {}

  u64 next() {
    u64 z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
  }

  // Any bit pattern for integers; finite values of mixed magnitude for
  // floating point.
  T any() {
    if constexpr (std::is_floating_point_v<T>) {
      const double unit = static_cast<double>(next() >> 11) * 0x1p-53;
      const int scale = static_cast<int>(next() % 24) - 12;
      const double v = (unit * 2.0 - 1.0) * std::ldexp(1.0, scale);
      return static_cast<T>(v);
    } else {
      return static_cast<T>(next());
    }
  }

  // Small enough that sums, differences and products of two never overflow
  // a signed type.
  T small() {
    if constexpr (std::is_floating_point_v<T>) {
      return static_cast<T>(static_cast<int>(next() % 2001) - 1000) /
             static_cast<T>(8);
    } else if constexpr (std::is_signed_v<T>) {
      constexpr long long range = sizeof(T) == 1   ? 11
                                  : sizeof(T) == 2 ? 181
                                                   : 1000;
      return static_cast<T>(static_cast<long long>(next() % (2 * range + 1)) -
                            range);
    } else {
      constexpr unsigned long long range = sizeof(T) == 1   ? 15
                                           : sizeof(T) == 2 ? 255
                                                            : 1000;
      return static_cast<T>(next() % (range + 1));
    }
  }

  T nonzero_small() {
    T v = small();
    return v == T(0) ? T(1) : v;
  }

  bool flip() { return (next() & 1) != 0; }
};

template <class T>
inline constexpr bool wide_signed =
    std::is_integral_v<T> && std::is_signed_v<T> && sizeof(T) >= sizeof(int);

template <class T> T shift_limit() {
  return static_cast<T>(sizeof(T) < sizeof(int) ? 32 : sizeof(T) * 8);
}

// Integer references wrap like the vector lanes do, without signed overflow.
template <class T> T wrap_add(T a, T b) {
  if constexpr (std::is_integral_v<T>) {
    using U = std::make_unsigned_t<T>;
    return static_cast<T>(
        static_cast<U>(static_cast<U>(a) + static_cast<U>(b)));
  } else {
    return a + b;
  }
}
template <class T> T wrap_mul(T a, T b) {
  if constexpr (std::is_integral_v<T>) {
    using U = std::conditional_t<(sizeof(T) < sizeof(unsigned)), unsigned,
                                 std::make_unsigned_t<T>>;
    return static_cast<T>(
        static_cast<U>(static_cast<U>(a) * static_cast<U>(b)));
  } else {
    return a * b;
  }
}

template <class T> T negate(T x) {
  if constexpr (std::is_unsigned_v<T>)
    return static_cast<T>(T(0) - x);
  else
    return static_cast<T>(-x);
}

template <class V> V random_vec(rng<typename V::value_type> &r, bool small) {
  return sd::detail::vec_generate<V>(
      [&](int) { return small ? r.small() : r.any(); });
}

} // namespace simd_test
