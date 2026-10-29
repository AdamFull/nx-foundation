#pragma once

#include "core/foundation/math/matrix.h"

NX_MATH_BEGIN

namespace nx::math {

// Transform builders for column vectors (mul(m, v)): right-handed, clip
// depth 0 to 1, as glm with GLM_FORCE_DEPTH_ZERO_TO_ONE.

constexpr float4x4 translation(const float3 &t) {
  float4x4 m = float4x4::identity();
  m.columns[3] = float4(t, 1.f);
  return m;
}
constexpr float4x4 scaling(const float3 &s) {
  float4x4 m{};
  m.columns[0].x = s.x;
  m.columns[1].y = s.y;
  m.columns[2].z = s.z;
  m.columns[3].w = 1.f;
  return m;
}
/// The rotation by angle radians about axis, which need not be unit length.
inline float4x4 rotation(const float3 &axis, float angle) {
  const float c = std::cos(angle), s = std::sin(angle);
  const float3 a = normalize(axis);
  const float3 t = (1.f - c) * a;
  return float4x4::from_columns(
      float4(c + t.x * a.x, t.x * a.y + s * a.z, t.x * a.z - s * a.y, 0.f),
      float4(t.y * a.x - s * a.z, c + t.y * a.y, t.y * a.z + s * a.x, 0.f),
      float4(t.z * a.x + s * a.y, t.z * a.y - s * a.x, c + t.z * a.z, 0.f),
      float4(0.f, 0.f, 0.f, 1.f));
}

/// Vertical field of view fov_y in radians; maps -near to depth 0 and -far
/// to 1.
inline float4x4 perspective_rh_zo(float fov_y, float aspect, float near_z,
                                  float far_z) {
  const float t = std::tan(fov_y / 2.f);
  float4x4 m{};
  m.columns[0].x = 1.f / (aspect * t);
  m.columns[1].y = 1.f / t;
  m.columns[2].z = far_z / (near_z - far_z);
  m.columns[2].w = -1.f;
  m.columns[3].z = -(far_z * near_z) / (far_z - near_z);
  return m;
}
constexpr float4x4 orthographic_rh_zo(float left, float right, float bottom,
                                      float top, float near_z, float far_z) {
  float4x4 m = float4x4::identity();
  m.columns[0].x = 2.f / (right - left);
  m.columns[1].y = 2.f / (top - bottom);
  m.columns[2].z = -1.f / (far_z - near_z);
  m.columns[3].x = -(right + left) / (right - left);
  m.columns[3].y = -(top + bottom) / (top - bottom);
  m.columns[3].z = -near_z / (far_z - near_z);
  return m;
}
/// The view from eye towards center, up roughly up: the camera looks down -z.
inline float4x4 look_at_rh(const float3 &eye, const float3 &center,
                           const float3 &up) {
  const float3 f = normalize(center - eye);
  const float3 s = normalize(cross(f, up));
  const float3 u = cross(s, f);
  return float4x4::from_columns(
      float4(s.x, u.x, -f.x, 0.f), float4(s.y, u.y, -f.y, 0.f),
      float4(s.z, u.z, -f.z, 0.f),
      float4(-dot(s, eye), -dot(u, eye), dot(f, eye), 1.f));
}

} // namespace nx::math

NX_MATH_END
