#include "math/math_test_util.h"

#include <cmath>

using namespace math_test;

NX_MATH_BEGIN

namespace {

quat glm_mul(const quat &p, const quat &q) {
  return quat(float3(p.w * q.x + p.x * q.w + p.y * q.z - p.z * q.y,
                     p.w * q.y + p.y * q.w + p.z * q.x - p.x * q.z,
                     p.w * q.z + p.z * q.w + p.x * q.y - p.y * q.x),
              p.w * q.w - p.x * q.x - p.y * q.y - p.z * q.z);
}

constexpr float half_pi = 1.57079632679489662f;

} // namespace

NX_MATH_END

TEST_CASE("math quaternion: the product is glm's, bit for bit") {
  lcg g{5};
  for (int i = 0; i < 256; ++i) {
    const quat a = g.next_quat(), b = g.next_quat();
    CHECK(same_bits(mul(a, b), glm_mul(a, b)));
  }
  const quat q = quat(float3(1.f, 2.f, 3.f), 4.f);
  CHECK(mul(quat::identity(), q) == q);
  CHECK(mul(q, quat::identity()) == q);
}

TEST_CASE("math quaternion: rotation agrees with its matrix") {
  lcg g{6};
  for (int i = 0; i < 128; ++i) {
    const quat a = g.next_quat(), b = g.next_quat();
    const float3 v = g.next3();
    CHECK(near(rotate(a, v), mul(to_float3x3(a), v)));
    CHECK(near(rotate(mul(a, b), v), rotate(a, rotate(b, v))));
    CHECK(near(mul(mul(a, quat(v, 0.f)), conjugate(a)).xyz(), rotate(a, v)));
    CHECK(near(mul(inverse(a), a), quat::identity()));
    CHECK(near(to_float4x4(a).columns[1].xyz(), to_float3x3(a).columns[1]));
  }
  const quat z90 = axis_angle(float3(0.f, 0.f, 1.f), half_pi);
  CHECK(near(rotate(z90, float3(1.f, 0.f, 0.f)), float3(0.f, 1.f, 0.f)));
}

TEST_CASE("math quaternion: to_quat inverts to_float3x3 on every branch") {
  const quat dominant[] = {
      normalize(quat(float3(0.1f, 0.2f, 0.3f), 0.9f)),
      normalize(quat(float3(0.9f, 0.2f, 0.3f), 0.1f)),
      normalize(quat(float3(0.1f, 0.9f, 0.3f), 0.2f)),
      normalize(quat(float3(0.1f, 0.2f, 0.9f), 0.3f)),
  };
  for (const quat &q : dominant)
    CHECK(near(to_quat(to_float3x3(q)), q));
  lcg g{8};
  for (int i = 0; i < 128; ++i) {
    const quat q = g.next_quat();
    const quat r = to_quat(to_float3x3(q));
    CHECK(near(dot(q, r) < 0.f ? -r : r, q, 1e-5));
  }
}

TEST_CASE("math quaternion: interpolation") {
  const quat a = quat::identity();
  const quat b = axis_angle(float3(0.f, 1.f, 0.f), half_pi);
  CHECK(near(slerp(a, b, 0.f), a));
  CHECK(near(slerp(a, b, 1.f), b));
  CHECK(
      near(slerp(a, b, 0.5f), axis_angle(float3(0.f, 1.f, 0.f), half_pi / 2)));
  // -b is the same rotation; the short way gives the same halfway point.
  CHECK(
      near(slerp(a, -b, 0.5f), axis_angle(float3(0.f, 1.f, 0.f), half_pi / 2)));
  const quat c = axis_angle(float3(0.f, 1.f, 0.f), 1e-5f);
  CHECK(near(slerp(a, c, 0.5f), quat(lerp(a.xyzw(), c.xyzw(), 0.5f))));
  CHECK(length(nlerp(a, b, 0.3f)) == nxtest::Approx(1.0).margin(1e-6));
  CHECK(normalize(quat(float3(0.f), 0.f)) == quat::identity());
}
