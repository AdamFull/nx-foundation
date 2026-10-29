#include "math/math_test_util.h"

using namespace math_test;

namespace {

float3 project(const float4x4 &m, const float3 &p) {
  const float4 c = mul(m, float4(p, 1.f));
  return c.xyz() / c.w;
}

} // namespace

TEST_CASE("math transform: translation, scaling and rotation") {
  CHECK(transform_point(translation(float3(1.f, 2.f, 3.f)), float3(1.f)) ==
        float3(2.f, 3.f, 4.f));
  CHECK(transform_vector(translation(float3(1.f, 2.f, 3.f)), float3(1.f)) ==
        float3(1.f));
  CHECK(transform_point(scaling(float3(2.f, 3.f, 4.f)), float3(1.f)) ==
        float3(2.f, 3.f, 4.f));
  lcg g{9};
  for (int i = 0; i < 64; ++i) {
    const float3 axis = g.next3();
    const float angle = g.next(-3.f, 3.f);
    CHECK(near(rotation(axis, angle),
               to_float4x4(axis_angle(normalize(axis), angle))));
  }
}

TEST_CASE("math transform: the perspective maps near to 0 and far to 1") {
  const float4x4 p = perspective_rh_zo(radians(90.f), 2.f, 0.5f, 100.f);
  CHECK(project(p, float3(0.f, 0.f, -0.5f)).z ==
        nxtest::Approx(0.0).margin(1e-6));
  CHECK(project(p, float3(0.f, 0.f, -100.f)).z ==
        nxtest::Approx(1.0).margin(1e-6));
  // A 90 degree field of view: the top edge at y = -z, the right at 2 * -z.
  CHECK(near(project(p, float3(2.f, 1.f, -1.f)).xy(), float2(1.f, 1.f)));
}

TEST_CASE("math transform: the orthographic box maps to clip space") {
  const float4x4 o = orthographic_rh_zo(-2.f, 6.f, -1.f, 3.f, 1.f, 11.f);
  CHECK(near(transform_point(o, float3(-2.f, -1.f, -1.f)),
             float3(-1.f, -1.f, 0.f)));
  CHECK(
      near(transform_point(o, float3(6.f, 3.f, -11.f)), float3(1.f, 1.f, 1.f)));
}

TEST_CASE("math transform: look_at puts the eye at the origin facing -z") {
  const float3 eye(1.f, 2.f, 3.f), center(4.f, 6.f, 3.f);
  const float4x4 v = look_at_rh(eye, center, float3(0.f, 0.f, 1.f));
  CHECK(near(transform_point(v, eye), float3(0.f)));
  CHECK(near(transform_point(v, center), float3(0.f, 0.f, -5.f)));
  CHECK(
      near(transform_vector(v, float3(0.f, 0.f, 1.f)), float3(0.f, 1.f, 0.f)));
}
