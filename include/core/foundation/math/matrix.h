#pragma once

#include "core/foundation/math/register.h"
#include "core/foundation/math/vector.h"

#include <utility>

NX_MATH_BEGIN

namespace nx::math {

/// R rows and C columns as in Slang, stored as C columns as in glm. Slang and
/// glm read m[i], m * v and the scalar and vector constructors differently, so
/// none of them exists here.
template <class T, int R, int C> struct mat {
  using value_type = T;
  using column_type = vec<T, R>;
  using row_type = vec<T, C>;
  static constexpr int rows = R;
  static constexpr int cols = C;

  column_type columns[C];

  static constexpr mat identity()
    requires(R <= C)
  {
    mat m{};
    for (int i = 0; i < R; ++i)
      m.columns[i][i] = T(1);
    return m;
  }
  template <class... V>
    requires(sizeof...(V) == C)
  static constexpr mat from_columns(const V &...c) {
    return mat{{column_type(c)...}};
  }
  template <class... V>
    requires(sizeof...(V) == R)
  static constexpr mat from_rows(const V &...r) {
    const row_type in[R] = {row_type(r)...};
    mat m;
    for (int c = 0; c < C; ++c)
      for (int i = 0; i < R; ++i)
        m.columns[c][i] = in[i][c];
    return m;
  }

  constexpr row_type row(int r) const {
    return [&]<int... I>(std::integer_sequence<int, I...>) {
      return row_type(columns[I][r]...);
    }(std::make_integer_sequence<int, C>{});
  }
  constexpr bool operator==(const mat &) const = default;
};

using float2x2 = mat<float, 2, 2>;
using float2x3 = mat<float, 2, 3>;
using float3x2 = mat<float, 3, 2>;
using float3x3 = mat<float, 3, 3>;
using float3x4 = mat<float, 3, 4>;
using float4x3 = mat<float, 4, 3>;
using float4x4 = mat<float, 4, 4>;
using double3x3 = mat<double, 3, 3>;
using double4x4 = mat<double, 4, 4>;

static_assert(sizeof(float4x4) == 64 && alignof(float4x4) == 4);
static_assert(sizeof(float3x4) == 48);
static_assert(std::is_trivially_copyable_v<float4x4> &&
              std::is_standard_layout_v<float4x4>);

template <class T, int R, int C>
constexpr mat<T, R, C> operator+(const mat<T, R, C> &a, const mat<T, R, C> &b) {
  mat<T, R, C> m;
  for (int c = 0; c < C; ++c)
    m.columns[c] = a.columns[c] + b.columns[c];
  return m;
}
template <class T, int R, int C>
constexpr mat<T, R, C> operator-(const mat<T, R, C> &a, const mat<T, R, C> &b) {
  mat<T, R, C> m;
  for (int c = 0; c < C; ++c)
    m.columns[c] = a.columns[c] - b.columns[c];
  return m;
}
template <class T, int R, int C>
constexpr mat<T, R, C> operator-(const mat<T, R, C> &a) {
  mat<T, R, C> m;
  for (int c = 0; c < C; ++c)
    m.columns[c] = -a.columns[c];
  return m;
}
template <class T, int R, int C>
constexpr mat<T, R, C> operator*(const mat<T, R, C> &a, scalar_of<T> s) {
  mat<T, R, C> m;
  for (int c = 0; c < C; ++c)
    m.columns[c] = a.columns[c] * s;
  return m;
}
template <class T, int R, int C>
constexpr mat<T, R, C> operator*(scalar_of<T> s, const mat<T, R, C> &a) {
  return a * s;
}

// The products sum in glm's order, so their results equal glm's: columns
// left to right, but a 4x4 times a vector as (c0 + c1) + (c2 + c3).
template <class T, int R, int K, int C>
constexpr mat<T, R, C> mul(const mat<T, R, K> &a, const mat<T, K, C> &b) {
  mat<T, R, C> m;
  for (int c = 0; c < C; ++c) {
    vec<T, R> s = a.columns[0] * b.columns[c][0];
    for (int k = 1; k < K; ++k)
      s += a.columns[k] * b.columns[c][k];
    m.columns[c] = s;
  }
  return m;
}
template <class T, int R, int C>
constexpr vec<T, R> mul(const mat<T, R, C> &a, const vec<T, C> &v) {
  vec<T, R> s = a.columns[0] * v[0];
  for (int k = 1; k < C; ++k)
    s += a.columns[k] * v[k];
  return s;
}
template <class T, int R, int C>
constexpr vec<T, C> mul(const vec<T, R> &v, const mat<T, R, C> &a) {
  vec<T, C> r;
  for (int c = 0; c < C; ++c)
    r[c] = dot(v, a.columns[c]);
  return r;
}

namespace detail {

NX_FORCE_INLINE f32x4 combine3(f32x4 c0, f32x4 c1, f32x4 c2, f32x4 b) {
  f32x4 s = x4::mul(c0, x4::splat<0>(b));
  s = x4::add(s, x4::mul(c1, x4::splat<1>(b)));
  return x4::add(s, x4::mul(c2, x4::splat<2>(b)));
}
NX_FORCE_INLINE f32x4 combine(f32x4 c0, f32x4 c1, f32x4 c2, f32x4 c3, f32x4 b) {
  return x4::add(combine3(c0, c1, c2, b), x4::mul(c3, x4::splat<3>(b)));
}

} // namespace detail

inline float4x4 mul(const float4x4 &a, const float4x4 &b) {
  const f32x4 a0 = x4::load(&a.columns[0].x), a1 = x4::load(&a.columns[1].x),
              a2 = x4::load(&a.columns[2].x), a3 = x4::load(&a.columns[3].x);
  float4x4 m;
  for (int c = 0; c < 4; ++c)
    x4::store(&m.columns[c].x,
              detail::combine(a0, a1, a2, a3, x4::load(&b.columns[c].x)));
  return m;
}

inline float4 mul(const float4x4 &a, const float4 &v) {
  const f32x4 b = x4::load(&v.x);
  const f32x4 s01 =
      x4::add(x4::mul(x4::load(&a.columns[0].x), x4::splat<0>(b)),
              x4::mul(x4::load(&a.columns[1].x), x4::splat<1>(b)));
  const f32x4 s23 =
      x4::add(x4::mul(x4::load(&a.columns[2].x), x4::splat<2>(b)),
              x4::mul(x4::load(&a.columns[3].x), x4::splat<3>(b)));
  float4 r;
  x4::store(&r.x, x4::add(s01, s23));
  return r;
}

inline float4 mul(const float4 &v, const float4x4 &a) {
  const f32x4 b = x4::load(&v.x);
  f32x4 p0 = x4::mul(b, x4::load(&a.columns[0].x));
  f32x4 p1 = x4::mul(b, x4::load(&a.columns[1].x));
  f32x4 p2 = x4::mul(b, x4::load(&a.columns[2].x));
  f32x4 p3 = x4::mul(b, x4::load(&a.columns[3].x));
  x4::transpose(p0, p1, p2, p3);
  float4 r;
  x4::store(&r.x, x4::add(x4::add(p0, p1), x4::add(p2, p3)));
  return r;
}

/// affine_mul over raw columns StrideA and StrideB floats apart: 3 for a
/// float3x4's bytes, 4 for a float4x4's. Writes a float3x4's 12 floats to o,
/// which overlaps neither. Copying o at once through wider moves defeats store
/// forwarding (2.6x here), so write it where it is wanted.
template <int StrideA, int StrideB>
inline void affine_mul_columns(const float *a, const float *b, float *o) {
  const f32x4 a0 = x4::load(a), a1 = x4::load(a + StrideA),
              a2 = x4::load(a + 2 * StrideA), a3 = x4::load3(a + 3 * StrideA);
  // Each 4-wide store's last lane is the next column's x, written after it.
  x4::store(o, detail::combine3(a0, a1, a2, x4::load(b)));
  x4::store(o + 3, detail::combine3(a0, a1, a2, x4::load(b + StrideB)));
  x4::store(o + 6, detail::combine3(a0, a1, a2, x4::load(b + 2 * StrideB)));
  x4::store3(
      o + 9,
      x4::add(detail::combine3(a0, a1, a2, x4::load3(b + 3 * StrideB)), a3));
}

/// The affine product of a and b, each taken to end in a row of 0 0 0 1.
inline float3x4 affine_mul(const float3x4 &a, const float3x4 &b) {
  float3x4 m;
  affine_mul_columns<3, 3>(&a.columns[0].x, &b.columns[0].x, &m.columns[0].x);
  return m;
}

/// affine_mul_columns of a by a transform: a rotation (x, y, z, w) read as
/// to_float3x3 reads it, each column times its scale, then the translation.
/// o overlaps nothing it reads. The columns stay in registers: written out and
/// loaded back wide they stall on store forwarding, 2.6x under MSVC.
template <int StrideA>
inline void affine_mul_trs(const float *a, const float *rotation,
                           const float *scale, const float *translation,
                           float *o) {
  const float x = rotation[0], y = rotation[1], z = rotation[2],
              w = rotation[3];
  const float xx = x * x, yy = y * y, zz = z * z;
  const float xz = x * z, xy = x * y, yz = y * z;
  const float wx = w * x, wy = w * y, wz = w * z;
  const f32x4 a0 = x4::load(a), a1 = x4::load(a + StrideA),
              a2 = x4::load(a + 2 * StrideA), a3 = x4::load3(a + 3 * StrideA);
  const f32x4 c0 = x4::mul(
      x4::set(1.f - 2.f * (yy + zz), 2.f * (xy + wz), 2.f * (xz - wy), 0.f),
      x4::splat(scale[0]));
  const f32x4 c1 = x4::mul(
      x4::set(2.f * (xy - wz), 1.f - 2.f * (xx + zz), 2.f * (yz + wx), 0.f),
      x4::splat(scale[1]));
  const f32x4 c2 = x4::mul(
      x4::set(2.f * (xz + wy), 2.f * (yz - wx), 1.f - 2.f * (xx + yy), 0.f),
      x4::splat(scale[2]));
  // Each 4-wide store's last lane is the next column's x, written after it.
  x4::store(o, detail::combine3(a0, a1, a2, c0));
  x4::store(o + 3, detail::combine3(a0, a1, a2, c1));
  x4::store(o + 6, detail::combine3(a0, a1, a2, c2));
  x4::store3(o + 9,
             x4::add(detail::combine3(a0, a1, a2, x4::load3(translation)), a3));
}

/// mul(m, float4(p, 1)).xyz, without the divide.
inline float3 transform_point(const float4x4 &m, const float3 &p) {
  const f32x4 b = x4::load3(&p.x);
  const f32x4 s01 =
      x4::add(x4::mul(x4::load(&m.columns[0].x), x4::splat<0>(b)),
              x4::mul(x4::load(&m.columns[1].x), x4::splat<1>(b)));
  const f32x4 s23 = x4::add(x4::mul(x4::load(&m.columns[2].x), x4::splat<2>(b)),
                            x4::load(&m.columns[3].x));
  float3 r;
  x4::store3(&r.x, x4::add(s01, s23));
  return r;
}
inline float3 transform_vector(const float4x4 &m, const float3 &v) {
  const f32x4 b = x4::load3(&v.x);
  f32x4 s = x4::mul(x4::load(&m.columns[0].x), x4::splat<0>(b));
  s = x4::add(s, x4::mul(x4::load(&m.columns[1].x), x4::splat<1>(b)));
  s = x4::add(s, x4::mul(x4::load(&m.columns[2].x), x4::splat<2>(b)));
  float3 r;
  x4::store3(&r.x, s);
  return r;
}
inline float3 transform_point(const float3x4 &m, const float3 &p) {
  const float *c = &m.columns[0].x;
  const f32x4 b = x4::load3(&p.x);
  f32x4 s = x4::mul(x4::load(c), x4::splat<0>(b));
  s = x4::add(s, x4::mul(x4::load(c + 3), x4::splat<1>(b)));
  s = x4::add(s, x4::mul(x4::load(c + 6), x4::splat<2>(b)));
  float3 r;
  x4::store3(&r.x, x4::add(s, x4::load3(c + 9)));
  return r;
}
inline float3 transform_vector(const float3x4 &m, const float3 &v) {
  const float *c = &m.columns[0].x;
  const f32x4 b = x4::load3(&v.x);
  f32x4 s = x4::mul(x4::load(c), x4::splat<0>(b));
  s = x4::add(s, x4::mul(x4::load(c + 3), x4::splat<1>(b)));
  float3 r;
  x4::store3(&r.x, x4::add(s, x4::mul(x4::load3(c + 6), x4::splat<2>(b))));
  return r;
}

template <class T, int R, int C>
constexpr mat<T, C, R> transpose(const mat<T, R, C> &a) {
  mat<T, C, R> m;
  for (int r = 0; r < R; ++r)
    m.columns[r] = a.row(r);
  return m;
}
inline float4x4 transpose(const float4x4 &a) {
  f32x4 c0 = x4::load(&a.columns[0].x), c1 = x4::load(&a.columns[1].x),
        c2 = x4::load(&a.columns[2].x), c3 = x4::load(&a.columns[3].x);
  x4::transpose(c0, c1, c2, c3);
  float4x4 m;
  x4::store(&m.columns[0].x, c0);
  x4::store(&m.columns[1].x, c1);
  x4::store(&m.columns[2].x, c2);
  x4::store(&m.columns[3].x, c3);
  return m;
}

template <class T> constexpr T determinant(const mat<T, 2, 2> &m) {
  const auto &c = m.columns;
  return c[0][0] * c[1][1] - c[1][0] * c[0][1];
}
template <class T> constexpr T determinant(const mat<T, 3, 3> &m) {
  const auto &c = m.columns;
  return +c[0][0] * (c[1][1] * c[2][2] - c[2][1] * c[1][2]) -
         c[1][0] * (c[0][1] * c[2][2] - c[2][1] * c[0][2]) +
         c[2][0] * (c[0][1] * c[1][2] - c[1][1] * c[0][2]);
}
template <class T> constexpr T determinant(const mat<T, 4, 4> &m) {
  const auto &c = m.columns;
  const T f0 = c[2][2] * c[3][3] - c[3][2] * c[2][3];
  const T f1 = c[2][1] * c[3][3] - c[3][1] * c[2][3];
  const T f2 = c[2][1] * c[3][2] - c[3][1] * c[2][2];
  const T f3 = c[2][0] * c[3][3] - c[3][0] * c[2][3];
  const T f4 = c[2][0] * c[3][2] - c[3][0] * c[2][2];
  const T f5 = c[2][0] * c[3][1] - c[3][0] * c[2][1];
  const vec<T, 4> cof(+(c[1][1] * f0 - c[1][2] * f1 + c[1][3] * f2),
                      -(c[1][0] * f0 - c[1][2] * f3 + c[1][3] * f4),
                      +(c[1][0] * f1 - c[1][1] * f3 + c[1][3] * f5),
                      -(c[1][0] * f2 - c[1][1] * f4 + c[1][2] * f5));
  return c[0][0] * cof[0] + c[0][1] * cof[1] + c[0][2] * cof[2] +
         c[0][3] * cof[3];
}

/// The inverse; a singular matrix gives infinities or NaNs.
template <class T> constexpr mat<T, 2, 2> inverse(const mat<T, 2, 2> &m) {
  const auto &c = m.columns;
  const T d = T(1) / (+c[0][0] * c[1][1] - c[1][0] * c[0][1]);
  return mat<T, 2, 2>::from_columns(vec<T, 2>(+c[1][1] * d, -c[0][1] * d),
                                    vec<T, 2>(-c[1][0] * d, +c[0][0] * d));
}
template <class T> constexpr mat<T, 3, 3> inverse(const mat<T, 3, 3> &m) {
  const auto &c = m.columns;
  const T d = T(1) / (+c[0][0] * (c[1][1] * c[2][2] - c[2][1] * c[1][2]) -
                      c[1][0] * (c[0][1] * c[2][2] - c[2][1] * c[0][2]) +
                      c[2][0] * (c[0][1] * c[1][2] - c[1][1] * c[0][2]));
  mat<T, 3, 3> r;
  r.columns[0] = vec<T, 3>(+(c[1][1] * c[2][2] - c[2][1] * c[1][2]),
                           -(c[0][1] * c[2][2] - c[2][1] * c[0][2]),
                           +(c[0][1] * c[1][2] - c[1][1] * c[0][2]));
  r.columns[1] = vec<T, 3>(-(c[1][0] * c[2][2] - c[2][0] * c[1][2]),
                           +(c[0][0] * c[2][2] - c[2][0] * c[0][2]),
                           -(c[0][0] * c[1][2] - c[1][0] * c[0][2]));
  r.columns[2] = vec<T, 3>(+(c[1][0] * c[2][1] - c[2][0] * c[1][1]),
                           -(c[0][0] * c[2][1] - c[2][0] * c[0][1]),
                           +(c[0][0] * c[1][1] - c[1][0] * c[0][1]));
  for (auto &col : r.columns)
    col *= d;
  return r;
}

/// glm's cofactor expansion, four lanes at a time.
inline float4x4 inverse(const float4x4 &m) {
  const f32x4 c0 = x4::load(&m.columns[0].x), c1 = x4::load(&m.columns[1].x),
              c2 = x4::load(&m.columns[2].x), c3 = x4::load(&m.columns[3].x);

  // Each lane is one 2x2 minor a * b - c * d of glm's coefficient table.
  const auto minors = [&]<int I, int J>() {
    const f32x4 a = x4::shuffle<I, I, I, I>(c2, c1);
    const f32x4 b = x4::shuffle<0, 0, 0, 2>(x4::shuffle<J, J, J, J>(c3, c2));
    const f32x4 c = x4::shuffle<0, 0, 0, 2>(x4::shuffle<I, I, I, I>(c3, c2));
    const f32x4 d = x4::shuffle<J, J, J, J>(c2, c1);
    return x4::sub(x4::mul(a, b), x4::mul(c, d));
  };
  const f32x4 f0 = minors.template operator()<2, 3>();
  const f32x4 f1 = minors.template operator()<1, 3>();
  const f32x4 f2 = minors.template operator()<1, 2>();
  const f32x4 f3 = minors.template operator()<0, 3>();
  const f32x4 f4 = minors.template operator()<0, 2>();
  const f32x4 f5 = minors.template operator()<0, 1>();

  const f32x4 v0 = x4::shuffle<0, 2, 2, 2>(x4::shuffle<0, 0, 0, 0>(c1, c0));
  const f32x4 v1 = x4::shuffle<0, 2, 2, 2>(x4::shuffle<1, 1, 1, 1>(c1, c0));
  const f32x4 v2 = x4::shuffle<0, 2, 2, 2>(x4::shuffle<2, 2, 2, 2>(c1, c0));
  const f32x4 v3 = x4::shuffle<0, 2, 2, 2>(x4::shuffle<3, 3, 3, 3>(c1, c0));

  const auto cofactors = [](f32x4 a, f32x4 fa, f32x4 b, f32x4 fb, f32x4 c,
                            f32x4 fc) {
    return x4::add(x4::sub(x4::mul(a, fa), x4::mul(b, fb)), x4::mul(c, fc));
  };
  const f32x4 i0 =
      x4::negate<false, true, false, true>(cofactors(v1, f0, v2, f1, v3, f2));
  const f32x4 i1 =
      x4::negate<true, false, true, false>(cofactors(v0, f0, v2, f3, v3, f4));
  const f32x4 i2 =
      x4::negate<false, true, false, true>(cofactors(v0, f1, v1, f3, v3, f5));
  const f32x4 i3 =
      x4::negate<true, false, true, false>(cofactors(v0, f2, v1, f4, v2, f5));

  const f32x4 row0 = x4::shuffle<0, 2, 0, 2>(x4::shuffle<0, 0, 0, 0>(i0, i1),
                                             x4::shuffle<0, 0, 0, 0>(i2, i3));
  const f32x4 d = x4::div(x4::splat(1.f), x4::dot4(c0, row0));
  float4x4 r;
  x4::store(&r.columns[0].x, x4::mul(i0, d));
  x4::store(&r.columns[1].x, x4::mul(i1, d));
  x4::store(&r.columns[2].x, x4::mul(i2, d));
  x4::store(&r.columns[3].x, x4::mul(i3, d));
  return r;
}

} // namespace nx::math

NX_MATH_END
