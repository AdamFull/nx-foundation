#pragma once

#include "core/foundation/math/matrix.h"

NX_MATH_BEGIN

namespace nx::math {

/// A rotation quaternion stored x, y, z, w, as glm's is. glm's (w, x, y, z)
/// constructor has no counterpart: build from (xyz, w) or a float4.
struct quat {
  float x, y, z, w;

  quat() = default;
  constexpr quat(const float3 &xyz, float w_)
      : x(xyz.x), y(xyz.y), z(xyz.z), w(w_) {}
  constexpr explicit quat(const float4 &xyzw)
      : x(xyzw.x), y(xyzw.y), z(xyzw.z), w(xyzw.w) {}

  static constexpr quat identity() { return quat(float3(0.f), 1.f); }

  constexpr float3 xyz() const { return {x, y, z}; }
  constexpr float4 xyzw() const { return {x, y, z, w}; }
  constexpr bool operator==(const quat &) const = default;

};

static_assert(sizeof(quat) == 16 && std::is_trivially_copyable_v<quat>);

constexpr quat operator+(const quat &a, const quat &b) {
  return quat(a.xyzw() + b.xyzw());
}
constexpr quat operator-(const quat &a, const quat &b) {
  return quat(a.xyzw() - b.xyzw());
}
constexpr quat operator-(const quat &q) { return quat(-q.xyzw()); }
constexpr quat operator*(const quat &q, float s) { return quat(q.xyzw() * s); }
constexpr quat operator*(float s, const quat &q) { return quat(q.xyzw() * s); }
constexpr quat operator/(const quat &q, float s) { return quat(q.xyzw() / s); }

/// (w w' + x x') + (y y' + z z'), glm's order.
constexpr float dot(const quat &a, const quat &b) {
  return (a.w * b.w + a.x * b.x) + (a.y * b.y + a.z * b.z);
}
inline float length(const quat &q) { return std::sqrt(dot(q, q)); }
/// q scaled to unit length; a zero quaternion gives the identity.
inline quat normalize(const quat &q) {
  const float len = length(q);
  if (len <= 0.f)
    return quat::identity();
  return q * (1.f / len);
}
constexpr quat conjugate(const quat &q) { return quat(-q.xyz(), q.w); }
constexpr quat inverse(const quat &q) { return conjugate(q) / dot(q, q); }

/// The Hamilton product: the rotation b, then a.
inline quat mul(const quat &a, const quat &b) {
  const f32x4 p = x4::load(&a.x), q = x4::load(&b.x);
  const f32x4 t0 = x4::mul(x4::splat<3>(p), q);
  const f32x4 t1 =
      x4::mul(x4::shuffle<0, 1, 2, 0>(p), x4::shuffle<3, 3, 3, 0>(q));
  const f32x4 t2 =
      x4::mul(x4::shuffle<1, 2, 0, 1>(p), x4::shuffle<2, 0, 1, 1>(q));
  const f32x4 t3 =
      x4::mul(x4::shuffle<2, 0, 1, 2>(p), x4::shuffle<1, 2, 0, 2>(q));
  const f32x4 s =
      x4::add(x4::add(t0, x4::negate<false, false, false, true>(t1)),
              x4::negate<false, false, false, true>(t2));
  quat r;
  x4::store(&r.x, x4::sub(s, t3));
  return r;
}

/// v rotated by the unit quaternion q.
constexpr float3 rotate(const quat &q, const float3 &v) {
  const float3 u = q.xyz();
  const float3 uv = cross(u, v);
  const float3 uuv = cross(u, uv);
  return v + ((uv * q.w) + uuv) * 2.f;
}

/// The rotation by angle radians about the unit axis.
inline quat axis_angle(const float3 &axis, float angle) {
  return quat(axis * std::sin(angle * 0.5f), std::cos(angle * 0.5f));
}

/// The angle of the rotation in radians, 0 to 2 pi.
inline float angle(const quat &q) {
  // cos(1/2): past it acos loses what asin keeps.
  if (abs(q.w) > 0.877582561890372716130286068203503191f) {
    const float a =
        std::asin(std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z)) * 2.f;
    return q.w < 0.f ? pi<float> * 2.f - a : a;
  }
  return std::acos(q.w) * 2.f;
}
/// The axis of the rotation; +z for the identity.
inline float3 axis(const quat &q) {
  const float t = 1.f - q.w * q.w;
  if (t <= 0.f)
    return float3(0.f, 0.f, 1.f);
  return q.xyz() * (1.f / std::sqrt(t));
}

/// The short way round; linear where a and b are almost equal.
inline quat slerp(const quat &a, const quat &b, float t) {
  quat z = b;
  float c = dot(a, b);
  if (c < 0.f) {
    z = -b;
    c = -c;
  }
  if (c > 1.f - 0x1p-23f)
    return quat(lerp(a.xyzw(), z.xyzw(), t));
  const float angle = std::acos(c);
  return (std::sin((1.f - t) * angle) * a + std::sin(t * angle) * z) /
         std::sin(angle);
}
/// The short way round.
inline quat nlerp(const quat &a, const quat &b, float t) {
  const quat z = dot(a, b) < 0.f ? -b : b;
  return normalize(quat(lerp(a.xyzw(), z.xyzw(), t)));
}

constexpr float3x3 to_float3x3(const quat &q) {
  const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const float xz = q.x * q.z, xy = q.x * q.y, yz = q.y * q.z;
  const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  return float3x3::from_columns(
      float3(1.f - 2.f * (yy + zz), 2.f * (xy + wz), 2.f * (xz - wy)),
      float3(2.f * (xy - wz), 1.f - 2.f * (xx + zz), 2.f * (yz + wx)),
      float3(2.f * (xz + wy), 2.f * (yz - wx), 1.f - 2.f * (xx + yy)));
}
constexpr float4x4 to_float4x4(const quat &q) {
  const float3x3 r = to_float3x3(q);
  return float4x4::from_columns(
      float4(r.columns[0], 0.f), float4(r.columns[1], 0.f),
      float4(r.columns[2], 0.f), float4(0.f, 0.f, 0.f, 1.f));
}

namespace detail {

/// The x, y, z and w of an orthonormal matrix's rotation.
template <typename T> inline vec<T, 4> rotation_of(const mat<T, 3, 3> &m) {
  const auto &c = m.columns;
  const T fx = c[0][0] - c[1][1] - c[2][2];
  const T fy = c[1][1] - c[0][0] - c[2][2];
  const T fz = c[2][2] - c[0][0] - c[1][1];
  const T fw = c[0][0] + c[1][1] + c[2][2];
  int biggest = 0;
  T f = fw;
  if (fx > f)
    f = fx, biggest = 1;
  if (fy > f)
    f = fy, biggest = 2;
  if (fz > f)
    f = fz, biggest = 3;
  const T v = std::sqrt(f + T(1)) * T(0.5);
  const T k = T(0.25) / v;
  switch (biggest) {
  case 0:
    return vec<T, 4>((c[1][2] - c[2][1]) * k, (c[2][0] - c[0][2]) * k,
                     (c[0][1] - c[1][0]) * k, v);
  case 1:
    return vec<T, 4>(v, (c[0][1] + c[1][0]) * k, (c[2][0] + c[0][2]) * k,
                     (c[1][2] - c[2][1]) * k);
  case 2:
    return vec<T, 4>((c[0][1] + c[1][0]) * k, v, (c[1][2] + c[2][1]) * k,
                     (c[2][0] - c[0][2]) * k);
  default:
    return vec<T, 4>((c[2][0] + c[0][2]) * k, (c[1][2] + c[2][1]) * k, v,
                     (c[0][1] - c[1][0]) * k);
  }
}

} // namespace detail

/// The rotation of an orthonormal matrix.
inline quat to_quat(const float3x3 &m) { return quat(detail::rotation_of(m)); }

/// The same worked out and normalized in double, for a cooker's source
/// matrices.
inline quat to_quat(const double3x3 &m) {
  const double4 q = detail::rotation_of(m);
  const double scale =
      1.0 / std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  return quat(
      float4(static_cast<float>(q.x * scale), static_cast<float>(q.y * scale),
             static_cast<float>(q.z * scale), static_cast<float>(q.w * scale)));
}

/// The rotation that points -z along direction, which is unit length, with +y
/// as near up as it can be.
inline quat look_at_quat_rh(const float3 &direction, const float3 &up) {
  const float3 back = -direction;
  const float3 side = cross(up, back);
  const float3 right = side * rsqrt(max(0.00001f, dot(side, side)));
  return to_quat(float3x3::from_columns(right, cross(back, right), back));
}

} // namespace nx::math

NX_MATH_END
