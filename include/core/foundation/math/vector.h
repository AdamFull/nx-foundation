#pragma once

#include "core/foundation/math/register.h"

#include <cmath>
#include <cstdint>
#include <type_traits>

NX_MATH_BEGIN

namespace nx::math {

template <class T, int N> struct vec;

template <class T> struct vec<T, 2> {
  using value_type = T;
  static constexpr int size = 2;

  T x, y;

  vec() = default;
  constexpr explicit vec(T s) : x(s), y(s) {}
  constexpr vec(T x_, T y_) : x(x_), y(y_) {}
  template <class U>
  constexpr explicit vec(const vec<U, 2> &v) : x(T(v.x)), y(T(v.y)) {}

  constexpr T &operator[](int i) { return i == 0 ? x : y; }
  constexpr const T &operator[](int i) const { return i == 0 ? x : y; }
  constexpr bool operator==(const vec &) const = default;

  constexpr vec yx() const { return {y, x}; }
};

template <class T> struct vec<T, 3> {
  using value_type = T;
  static constexpr int size = 3;

  T x, y, z;

  vec() = default;
  constexpr explicit vec(T s) : x(s), y(s), z(s) {}
  constexpr vec(T x_, T y_, T z_) : x(x_), y(y_), z(z_) {}
  constexpr vec(const vec<T, 2> &xy_, T z_) : x(xy_.x), y(xy_.y), z(z_) {}
  template <class U>
  constexpr explicit vec(const vec<U, 3> &v)
      : x(T(v.x)), y(T(v.y)), z(T(v.z)) {}

  constexpr T &operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
  constexpr const T &operator[](int i) const {
    return i == 0 ? x : i == 1 ? y : z;
  }
  constexpr bool operator==(const vec &) const = default;

  constexpr vec<T, 2> xy() const { return {x, y}; }
  constexpr vec<T, 2> xz() const { return {x, z}; }
  constexpr vec<T, 2> yz() const { return {y, z}; }
};

template <class T> struct vec<T, 4> {
  using value_type = T;
  static constexpr int size = 4;

  T x, y, z, w;

  vec() = default;
  constexpr explicit vec(T s) : x(s), y(s), z(s), w(s) {}
  constexpr vec(T x_, T y_, T z_, T w_) : x(x_), y(y_), z(z_), w(w_) {}
  constexpr vec(const vec<T, 3> &xyz_, T w_)
      : x(xyz_.x), y(xyz_.y), z(xyz_.z), w(w_) {}
  constexpr vec(const vec<T, 2> &xy_, T z_, T w_)
      : x(xy_.x), y(xy_.y), z(z_), w(w_) {}
  constexpr vec(const vec<T, 2> &xy_, const vec<T, 2> &zw_)
      : x(xy_.x), y(xy_.y), z(zw_.x), w(zw_.y) {}
  template <class U>
  constexpr explicit vec(const vec<U, 4> &v)
      : x(T(v.x)), y(T(v.y)), z(T(v.z)), w(T(v.w)) {}

  constexpr T &operator[](int i) {
    return i == 0 ? x : i == 1 ? y : i == 2 ? z : w;
  }
  constexpr const T &operator[](int i) const {
    return i == 0 ? x : i == 1 ? y : i == 2 ? z : w;
  }
  constexpr bool operator==(const vec &) const = default;

  constexpr vec<T, 2> xy() const { return {x, y}; }
  constexpr vec<T, 2> zw() const { return {z, w}; }
  constexpr vec<T, 3> xyz() const { return {x, y, z}; }
};

/// (v[I]...): swizzle<2, 1, 0>(v) is Slang's v.zyx.
template <int... I, class T, int N>
constexpr vec<T, int(sizeof...(I))> swizzle(const vec<T, N> &v) {
  static_assert(((I >= 0 && I < N) && ...));
  return vec<T, int(sizeof...(I))>(v[I]...);
}

using float2 = vec<float, 2>;
using float3 = vec<float, 3>;
using float4 = vec<float, 4>;
using double2 = vec<double, 2>;
using double3 = vec<double, 3>;
using double4 = vec<double, 4>;
using int2 = vec<int32_t, 2>;
using int3 = vec<int32_t, 3>;
using int4 = vec<int32_t, 4>;
using uint2 = vec<uint32_t, 2>;
using uint3 = vec<uint32_t, 3>;
using uint4 = vec<uint32_t, 4>;
using bool2 = vec<bool, 2>;
using bool3 = vec<bool, 3>;
using bool4 = vec<bool, 4>;

static_assert(sizeof(float3) == 12 && alignof(float3) == 4);
static_assert(sizeof(float4) == 16 && alignof(float4) == 4);
static_assert(std::is_trivially_copyable_v<float4> &&
              std::is_standard_layout_v<float4>);

template <class T> using scalar_of = std::type_identity_t<T>;

#define NX_MATH_VEC_BINARY(op)                                                 \
  template <class T>                                                           \
  constexpr vec<T, 2> operator op(const vec<T, 2> &a, const vec<T, 2> &b) {    \
    return vec<T, 2>(T(a.x op b.x), T(a.y op b.y));                            \
  }                                                                            \
  template <class T>                                                           \
  constexpr vec<T, 3> operator op(const vec<T, 3> &a, const vec<T, 3> &b) {    \
    return vec<T, 3>(T(a.x op b.x), T(a.y op b.y), T(a.z op b.z));             \
  }                                                                            \
  template <class T>                                                           \
  constexpr vec<T, 4> operator op(const vec<T, 4> &a, const vec<T, 4> &b) {    \
    return vec<T, 4>(T(a.x op b.x), T(a.y op b.y), T(a.z op b.z),              \
                     T(a.w op b.w));                                           \
  }                                                                            \
  template <class T>                                                           \
  constexpr vec<T, 2> operator op(const vec<T, 2> &a, scalar_of<T> b) {        \
    return vec<T, 2>(T(a.x op b), T(a.y op b));                                \
  }                                                                            \
  template <class T>                                                           \
  constexpr vec<T, 3> operator op(const vec<T, 3> &a, scalar_of<T> b) {        \
    return vec<T, 3>(T(a.x op b), T(a.y op b), T(a.z op b));                   \
  }                                                                            \
  template <class T>                                                           \
  constexpr vec<T, 4> operator op(const vec<T, 4> &a, scalar_of<T> b) {        \
    return vec<T, 4>(T(a.x op b), T(a.y op b), T(a.z op b), T(a.w op b));      \
  }                                                                            \
  template <class T>                                                           \
  constexpr vec<T, 2> operator op(scalar_of<T> a, const vec<T, 2> &b) {        \
    return vec<T, 2>(T(a op b.x), T(a op b.y));                                \
  }                                                                            \
  template <class T>                                                           \
  constexpr vec<T, 3> operator op(scalar_of<T> a, const vec<T, 3> &b) {        \
    return vec<T, 3>(T(a op b.x), T(a op b.y), T(a op b.z));                   \
  }                                                                            \
  template <class T>                                                           \
  constexpr vec<T, 4> operator op(scalar_of<T> a, const vec<T, 4> &b) {        \
    return vec<T, 4>(T(a op b.x), T(a op b.y), T(a op b.z), T(a op b.w));      \
  }                                                                            \
  template <class T, int N, class B>                                           \
  constexpr vec<T, N> &operator op## = (vec<T, N> & a, const B &b) {           \
    return a = a op b;                                                         \
  }

NX_MATH_VEC_BINARY(+)
NX_MATH_VEC_BINARY(-)
NX_MATH_VEC_BINARY(*)
NX_MATH_VEC_BINARY(/)
NX_MATH_VEC_BINARY(%)
NX_MATH_VEC_BINARY(&)
NX_MATH_VEC_BINARY(|)
NX_MATH_VEC_BINARY(^)
NX_MATH_VEC_BINARY(<<)
NX_MATH_VEC_BINARY(>>)
#undef NX_MATH_VEC_BINARY

#define NX_MATH_VEC_UNARY(op)                                                  \
  template <class T> constexpr vec<T, 2> operator op(const vec<T, 2> &a) {     \
    return vec<T, 2>(T(op a.x), T(op a.y));                                    \
  }                                                                            \
  template <class T> constexpr vec<T, 3> operator op(const vec<T, 3> &a) {     \
    return vec<T, 3>(T(op a.x), T(op a.y), T(op a.z));                         \
  }                                                                            \
  template <class T> constexpr vec<T, 4> operator op(const vec<T, 4> &a) {     \
    return vec<T, 4>(T(op a.x), T(op a.y), T(op a.z), T(op a.w));              \
  }

NX_MATH_VEC_UNARY(-)
NX_MATH_VEC_UNARY(~)
NX_MATH_VEC_UNARY(!)
#undef NX_MATH_VEC_UNARY

template <class T, int N> constexpr vec<T, N> operator+(const vec<T, N> &a) {
  return a;
}

// Component-wise comparisons; == and != compare whole vectors.
#define NX_MATH_VEC_COMPARE(name, op)                                          \
  template <class T>                                                           \
  constexpr bool2 name(const vec<T, 2> &a, const vec<T, 2> &b) {               \
    return bool2(a.x op b.x, a.y op b.y);                                      \
  }                                                                            \
  template <class T>                                                           \
  constexpr bool3 name(const vec<T, 3> &a, const vec<T, 3> &b) {               \
    return bool3(a.x op b.x, a.y op b.y, a.z op b.z);                          \
  }                                                                            \
  template <class T>                                                           \
  constexpr bool4 name(const vec<T, 4> &a, const vec<T, 4> &b) {               \
    return bool4(a.x op b.x, a.y op b.y, a.z op b.z, a.w op b.w);              \
  }

NX_MATH_VEC_COMPARE(equal, ==)
NX_MATH_VEC_COMPARE(not_equal, !=)
NX_MATH_VEC_COMPARE(less, <)
NX_MATH_VEC_COMPARE(less_equal, <=)
NX_MATH_VEC_COMPARE(greater, >)
NX_MATH_VEC_COMPARE(greater_equal, >=)
#undef NX_MATH_VEC_COMPARE

template <class T> constexpr bool any(const vec<T, 2> &v) { return v.x || v.y; }
template <class T> constexpr bool any(const vec<T, 3> &v) {
  return v.x || v.y || v.z;
}
template <class T> constexpr bool any(const vec<T, 4> &v) {
  return v.x || v.y || v.z || v.w;
}
template <class T> constexpr bool all(const vec<T, 2> &v) { return v.x && v.y; }
template <class T> constexpr bool all(const vec<T, 3> &v) {
  return v.x && v.y && v.z;
}
template <class T> constexpr bool all(const vec<T, 4> &v) {
  return v.x && v.y && v.z && v.w;
}

template <class T>
constexpr vec<T, 2> select(const bool2 &c, const vec<T, 2> &a,
                           const vec<T, 2> &b) {
  return vec<T, 2>(c.x ? a.x : b.x, c.y ? a.y : b.y);
}
template <class T>
constexpr vec<T, 3> select(const bool3 &c, const vec<T, 3> &a,
                           const vec<T, 3> &b) {
  return vec<T, 3>(c.x ? a.x : b.x, c.y ? a.y : b.y, c.z ? a.z : b.z);
}
template <class T>
constexpr vec<T, 4> select(const bool4 &c, const vec<T, 4> &a,
                           const vec<T, 4> &b) {
  return vec<T, 4>(c.x ? a.x : b.x, c.y ? a.y : b.y, c.z ? a.z : b.z,
                   c.w ? a.w : b.w);
}

// min and max follow f32x4's rule, so a NaN gives the same answer everywhere.
template <class T>
  requires std::is_arithmetic_v<T>
constexpr T min(T a, T b) {
  return a < b ? a : b;
}
template <class T>
  requires std::is_arithmetic_v<T>
constexpr T max(T a, T b) {
  return a > b ? a : b;
}
template <class T>
  requires std::is_arithmetic_v<T>
constexpr T clamp(T v, T lo, T hi) {
  return min(max(v, lo), hi);
}
template <class T>
  requires std::is_floating_point_v<T>
constexpr T saturate(T v) {
  return clamp(v, T(0), T(1));
}
template <class T>
  requires std::is_floating_point_v<T>
constexpr T lerp(T a, T b, T t) {
  return a + t * (b - a);
}
template <class T>
  requires std::is_arithmetic_v<T>
constexpr T sign(T v) {
  return T((T(0) < v) - (v < T(0)));
}
template <class T>
  requires std::is_floating_point_v<T>
constexpr T step(T edge, T v) {
  return v >= edge ? T(1) : T(0);
}
template <class T>
  requires std::is_floating_point_v<T>
constexpr T smoothstep(T lo, T hi, T v) {
  const T t = saturate((v - lo) / (hi - lo));
  return t * t * (T(3) - T(2) * t);
}
template <class T>
  requires std::is_floating_point_v<T>
constexpr T radians(T degrees) {
  return degrees * T(0.017453292519943295769236907684886);
}
template <class T>
  requires std::is_floating_point_v<T>
constexpr T degrees(T radians) {
  return radians * T(57.295779513082320876798154814105);
}
template <class T>
  requires std::is_floating_point_v<T>
T frac(T v) {
  return v - std::floor(v);
}
template <class T>
  requires std::is_floating_point_v<T>
T rsqrt(T v) {
  return T(1) / std::sqrt(v);
}
template <class T>
  requires std::is_floating_point_v<T>
constexpr T rcp(T v) {
  return T(1) / v;
}
template <class T>
  requires std::is_arithmetic_v<T>
constexpr T abs(T v) {
  return v < T(0) ? T(-v) : v;
}

#define NX_MATH_VEC_MAP(name, f)                                               \
  template <class T> vec<T, 2> name(const vec<T, 2> &v) {                      \
    return vec<T, 2>(f(v.x), f(v.y));                                          \
  }                                                                            \
  template <class T> vec<T, 3> name(const vec<T, 3> &v) {                      \
    return vec<T, 3>(f(v.x), f(v.y), f(v.z));                                  \
  }                                                                            \
  template <class T> vec<T, 4> name(const vec<T, 4> &v) {                      \
    return vec<T, 4>(f(v.x), f(v.y), f(v.z), f(v.w));                          \
  }

NX_MATH_VEC_MAP(abs, abs)
NX_MATH_VEC_MAP(sign, sign)
NX_MATH_VEC_MAP(saturate, saturate)
NX_MATH_VEC_MAP(frac, frac)
NX_MATH_VEC_MAP(rsqrt, rsqrt)
NX_MATH_VEC_MAP(rcp, rcp)
NX_MATH_VEC_MAP(radians, radians)
NX_MATH_VEC_MAP(degrees, degrees)
NX_MATH_VEC_MAP(sqrt, std::sqrt)
NX_MATH_VEC_MAP(floor, std::floor)
NX_MATH_VEC_MAP(ceil, std::ceil)
NX_MATH_VEC_MAP(trunc, std::trunc)
// HLSL rounds halves to even.
NX_MATH_VEC_MAP(round, std::nearbyint)
NX_MATH_VEC_MAP(exp, std::exp)
NX_MATH_VEC_MAP(exp2, std::exp2)
NX_MATH_VEC_MAP(log, std::log)
NX_MATH_VEC_MAP(log2, std::log2)
NX_MATH_VEC_MAP(sin, std::sin)
NX_MATH_VEC_MAP(cos, std::cos)
NX_MATH_VEC_MAP(tan, std::tan)
NX_MATH_VEC_MAP(asin, std::asin)
NX_MATH_VEC_MAP(acos, std::acos)
NX_MATH_VEC_MAP(atan, std::atan)
#undef NX_MATH_VEC_MAP

#define NX_MATH_VEC_ZIP(name, f)                                               \
  template <class T> vec<T, 2> name(const vec<T, 2> &a, const vec<T, 2> &b) {  \
    return vec<T, 2>(f(a.x, b.x), f(a.y, b.y));                                \
  }                                                                            \
  template <class T> vec<T, 3> name(const vec<T, 3> &a, const vec<T, 3> &b) {  \
    return vec<T, 3>(f(a.x, b.x), f(a.y, b.y), f(a.z, b.z));                   \
  }                                                                            \
  template <class T> vec<T, 4> name(const vec<T, 4> &a, const vec<T, 4> &b) {  \
    return vec<T, 4>(f(a.x, b.x), f(a.y, b.y), f(a.z, b.z), f(a.w, b.w));      \
  }

NX_MATH_VEC_ZIP(min, min)
NX_MATH_VEC_ZIP(max, max)
NX_MATH_VEC_ZIP(step, step)
NX_MATH_VEC_ZIP(fmod, std::fmod)
NX_MATH_VEC_ZIP(pow, std::pow)
NX_MATH_VEC_ZIP(atan2, std::atan2)
#undef NX_MATH_VEC_ZIP

template <class T, int N>
constexpr vec<T, N> clamp(const vec<T, N> &v, const vec<T, N> &lo,
                          const vec<T, N> &hi) {
  return min(max(v, lo), hi);
}
template <class T, int N>
constexpr vec<T, N> clamp(const vec<T, N> &v, scalar_of<T> lo,
                          scalar_of<T> hi) {
  return min(max(v, vec<T, N>(lo)), vec<T, N>(hi));
}
template <class T, int N>
constexpr vec<T, N> lerp(const vec<T, N> &a, const vec<T, N> &b,
                         scalar_of<T> t) {
  return a + t * (b - a);
}
template <class T, int N>
constexpr vec<T, N> lerp(const vec<T, N> &a, const vec<T, N> &b,
                         const vec<T, N> &t) {
  return a + t * (b - a);
}
template <class T, int N>
constexpr vec<T, N> smoothstep(const vec<T, N> &lo, const vec<T, N> &hi,
                               const vec<T, N> &v) {
  const vec<T, N> t = saturate((v - lo) / (hi - lo));
  return t * t * (T(3) - T(2) * t);
}

template <class T> constexpr T dot(const vec<T, 2> &a, const vec<T, 2> &b) {
  return a.x * b.x + a.y * b.y;
}
template <class T> constexpr T dot(const vec<T, 3> &a, const vec<T, 3> &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
/// (x + y) + (z + w), the order f32x4's dot4 sums in.
template <class T> constexpr T dot(const vec<T, 4> &a, const vec<T, 4> &b) {
  return (a.x * b.x + a.y * b.y) + (a.z * b.z + a.w * b.w);
}

template <class T>
constexpr vec<T, 3> cross(const vec<T, 3> &a, const vec<T, 3> &b) {
  return vec<T, 3>(a.y * b.z - b.y * a.z, a.z * b.x - b.z * a.x,
                   a.x * b.y - b.x * a.y);
}

template <class T, int N> constexpr T length_squared(const vec<T, N> &v) {
  return dot(v, v);
}
template <class T, int N> T length(const vec<T, N> &v) {
  return std::sqrt(dot(v, v));
}
template <class T, int N> T distance(const vec<T, N> &a, const vec<T, N> &b) {
  return length(b - a);
}
/// v / length(v); a zero vector gives NaNs, as in HLSL.
template <class T, int N> vec<T, N> normalize(const vec<T, N> &v) {
  return v * rsqrt(dot(v, v));
}
template <class T, int N>
constexpr vec<T, N> reflect(const vec<T, N> &i, const vec<T, N> &n) {
  return i - n * dot(n, i) * T(2);
}
/// Zero on total internal reflection.
template <class T, int N>
vec<T, N> refract(const vec<T, N> &i, const vec<T, N> &n, scalar_of<T> eta) {
  const T d = dot(n, i);
  const T k = T(1) - eta * eta * (T(1) - d * d);
  if (k < T(0))
    return vec<T, N>(T(0));
  return eta * i - (eta * d + std::sqrt(k)) * n;
}
template <class T, int N>
constexpr vec<T, N> faceforward(const vec<T, N> &n, const vec<T, N> &i,
                                const vec<T, N> &ng) {
  return dot(ng, i) < T(0) ? n : -n;
}

} // namespace nx::math

NX_MATH_END
