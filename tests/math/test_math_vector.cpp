#include "math/math_test_util.h"

#include <cmath>

using namespace math_test;

static_assert(dot(float3(1.f, 2.f, 3.f), float3(4.f, 5.f, 6.f)) == 32.f);
static_assert(cross(float3(1.f, 0.f, 0.f), float3(0.f, 1.f, 0.f)) ==
              float3(0.f, 0.f, 1.f));
static_assert(swizzle<2, 0>(float3(1.f, 2.f, 3.f)) == float2(3.f, 1.f));
static_assert(float4(float2(1.f, 2.f), float2(3.f, 4.f)).zw() ==
              float2(3.f, 4.f));
static_assert(select(less(int3(1, 5, 3), int3(2, 2, 2)), int3(10), int3(-1)) ==
              int3(10, -1, -1));
static_assert(all(greater_equal(uint2(3u, 4u), uint2(3u))));
static_assert(!any(equal(int4(1, 2, 3, 4), int4(0))));

TEST_CASE("math vector: element access follows x, y, z, w") {
  float4 v(1.f, 2.f, 3.f, 4.f);
  v[2] = 7.f;
  CHECK(v.z == 7.f);
  CHECK(v[3] == 4.f);
  CHECK(v.xyz() == float3(1.f, 2.f, 7.f));
  CHECK(float3(v.xy(), 5.f) == float3(1.f, 2.f, 5.f));
  CHECK(float3(int3(1, -2, 3)) == float3(1.f, -2.f, 3.f));
}

TEST_CASE("math vector: arithmetic is per component, scalars broadcast") {
  CHECK(float3(1.f, 2.f, 3.f) * float3(2.f) == float3(2.f, 4.f, 6.f));
  CHECK(2.f * float2(1.f, -1.f) == float2(2.f, -2.f));
  CHECK(float2(1.f, -1.f) - 1.f == float2(0.f, -2.f));
  CHECK(1.f / float2(2.f, 4.f) == float2(0.5f, 0.25f));
  CHECK(-float3(1.f, -2.f, 0.f) == float3(-1.f, 2.f, -0.f));
  float3 v(1.f);
  v += float3(1.f, 2.f, 3.f);
  v *= 2.f;
  CHECK(v == float3(4.f, 6.f, 8.f));
  CHECK(int3(7, -7, 8) % 3 == int3(1, -1, 2));
  CHECK((uint2(1u, 3u) << 2u) == uint2(4u, 12u));
  CHECK((int2(6, 5) & int2(3)) == int2(2, 1));
  CHECK(~uint2(0u) == uint2(0xffffffffu));
  CHECK(!bool2(true, false) == bool2(false, true));
}

TEST_CASE("math vector: == compares whole vectors, the named comparisons "
          "components") {
  CHECK(float3(1.f, 2.f, 3.f) == float3(1.f, 2.f, 3.f));
  CHECK(float3(1.f, 2.f, 3.f) != float3(1.f, 2.f, 4.f));
  CHECK(not_equal(float3(1.f, 2.f, 3.f), float3(1.f, 0.f, 3.f)) ==
        bool3(false, true, false));
  CHECK(less_equal(float2(1.f, 2.f), float2(1.f)) == bool2(true, false));
}

TEST_CASE("math vector: geometry") {
  lcg g{11};
  for (int i = 0; i < 64; ++i) {
    const float3 a = g.next3(), b = g.next3();
    const float3 n = normalize(a);
    CHECK(length(n) == nxtest::Approx(1.0).margin(1e-6));
    const float3 c = cross(a, b);
    CHECK(dot(c, a) == nxtest::Approx(0.0).margin(1e-6));
    CHECK(dot(c, b) == nxtest::Approx(0.0).margin(1e-6));
    CHECK(distance(a, b) == nxtest::Approx(length(a - b)));
    const float3 r = reflect(a, n);
    CHECK(near(r, a - 2.f * dot(a, n) * n));
    CHECK(length(r) == nxtest::Approx(length(a)).epsilon(1e-5));
  }
  CHECK(near(refract(float3(0.6f, -0.8f, 0.f), float3(0.f, 1.f, 0.f), 1.f),
             float3(0.6f, -0.8f, 0.f)));
  // Past the critical angle the ray reflects totally: zero.
  CHECK(refract(float3(0.8f, -0.6f, 0.f), float3(0.f, 1.f, 0.f), 1.5f) ==
        float3(0.f));
  CHECK(faceforward(float3(0.f, 1.f, 0.f), float3(0.f, 1.f, 0.f),
                    float3(0.f, 1.f, 0.f)) == float3(0.f, -1.f, 0.f));
}

TEST_CASE("math vector: HLSL's scalar functions") {
  CHECK(round(float4(2.5f, 3.5f, -2.5f, 0.4f)) == float4(2.f, 4.f, -2.f, 0.f));
  CHECK(frac(float2(-1.25f, 2.75f)) == float2(0.75f, 0.75f));
  CHECK(sign(float3(-3.f, 0.f, 2.f)) == float3(-1.f, 0.f, 1.f));
  CHECK(sign(int2(-3, 4)) == int2(-1, 1));
  CHECK(saturate(float3(-1.f, 0.5f, 2.f)) == float3(0.f, 0.5f, 1.f));
  CHECK(step(float2(1.f), float2(0.5f, 1.f)) == float2(0.f, 1.f));
  CHECK(clamp(float3(-2.f, 0.5f, 3.f), -1.f, 1.f) == float3(-1.f, 0.5f, 1.f));
  CHECK(lerp(float2(0.f, 10.f), float2(10.f, 20.f), 0.25f) ==
        float2(2.5f, 12.5f));
  CHECK(smoothstep(0.f, 1.f, 0.5f) == 0.5f);
  CHECK(smoothstep(float2(0.f), float2(1.f), float2(-1.f, 2.f)) ==
        float2(0.f, 1.f));
  CHECK(rsqrt(4.f) == 0.5f);
  CHECK(abs(int3(-1, 2, -3)) == int3(1, 2, 3));
  CHECK(min(float2(1.f, 5.f), float2(2.f, 3.f)) == float2(1.f, 3.f));
  CHECK(max(int2(1, 5), int2(2, 3)) == int2(2, 5));
  CHECK(degrees(radians(90.f)) == nxtest::Approx(90.0));
}

TEST_CASE("math vector: scalars with vectors, and rounding") {
  CHECK(float3(1.f, float2(2.f, 3.f)) == float3(1.f, 2.f, 3.f));
  CHECK(float4(1.f, float3(2.f, 3.f, 4.f)) == float4(1.f, 2.f, 3.f, 4.f));
  CHECK(min(float2(1.f, 5.f), 3.f) == float2(1.f, 3.f));
  CHECK(max(int2(1, 5), 3) == int2(3, 5));
  CHECK(floor(-1.5f) == -2.f);
  CHECK(ceil(-1.5f) == -1.f);
  CHECK(trunc(-1.5f) == -1.f);
  CHECK(nx::math::round(2.5f) == 2.f);
  CHECK(exp(0.f) == 1.f);
  CHECK(two_pi<float> == pi<float> * 2.f);
  CHECK(half_pi<float> == pi<float> / 2.f);
  CHECK(quarter_pi<float> == pi<float> / 4.f);
}

TEST_CASE("math vector: lerp reaches both ends exactly") {
  lcg g{21};
  for (int i = 0; i < 256; ++i) {
    const float3 a = g.next3() * 100.f, b = g.next3() * 100.f;
    CHECK(lerp(a, b, 0.f) == a);
    CHECK(lerp(a, b, 1.f) == b);
    CHECK(lerp(a.x, b.x, 1.f) == b.x);
    CHECK(lerp(a, b, float3(1.f)) == b);
  }
}

TEST_CASE("math vector: 16-bit floats") {
  CHECK(f32tof16(0.f) == 0);
  CHECK(f32tof16(-0.f) == 0x8000);
  CHECK(f32tof16(1.f) == 0x3c00);
  CHECK(f32tof16(-2.f) == 0xc000);
  CHECK(f32tof16(65504.f) == 0x7bff);
  // Rounds past the largest finite, and past the largest of an exponent.
  CHECK(f32tof16(65520.f) == 0x7c00);
  CHECK(f32tof16(1.9998779f) == 0x4000);
  CHECK(f32tof16(1e10f) == 0x7c00);
  CHECK(f32tof16(-1e10f) == 0xfc00);
  // The smallest normal, a subnormal, and what rounds to the smallest of them.
  CHECK(f32tof16(0x1p-14f) == 0x0400);
  CHECK(f32tof16(0x1p-24f) == 0x0001);
  CHECK(f32tof16(0x1.8p-25f) == 0x0001);
  CHECK(f32tof16(0x1p-26f) == 0);
  CHECK(f32tof16(std::bit_cast<float>(0x7f800001u)) == 0x7c01);
  CHECK(f32tof16(std::bit_cast<float>(0xffc00000u)) == 0xfe00);
  CHECK(f16tof32(0x3c00) == 1.f);
  CHECK(f16tof32(0x0001) == 0x1p-24f);
  CHECK(f16tof32(0x03ff) == 0x1.ff8p-15f);
  CHECK(std::bit_cast<uint32_t>(f16tof32(0x8000)) == 0x80000000u);
  CHECK(std::bit_cast<uint32_t>(f16tof32(0x7c00)) == 0x7f800000u);
  CHECK(std::bit_cast<uint32_t>(f16tof32(0xfe01)) == 0xffc02000u);
  int wrong = 0;
  for (uint32_t h = 0; h <= 0xffffu; ++h)
    wrong += f32tof16(f16tof32(uint16_t(h))) != h;
  CHECK(wrong == 0);
}
