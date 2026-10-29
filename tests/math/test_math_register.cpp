#include "math/math_test_util.h"

#include <bit>
#include <cmath>
#include <limits>

using namespace math_test;

namespace {

float4 lanes(f32x4 v) {
  float4 r;
  x4::store(&r.x, v);
  return r;
}

} // namespace

TEST_CASE("math register: load3 zeroes w and store3 writes three floats") {
  const float in[4] = {1.f, 2.f, 3.f, 4.f};
  CHECK(lanes(x4::load3(in)) == float4(1.f, 2.f, 3.f, 0.f));
  float out[4] = {0.f, 0.f, 0.f, 9.f};
  x4::store3(out, x4::load(in));
  CHECK(out[2] == 3.f);
  CHECK(out[3] == 9.f);
}

TEST_CASE("math register: shuffles pick lanes from one or two sources") {
  const f32x4 a = x4::set(0.f, 1.f, 2.f, 3.f);
  const f32x4 b = x4::set(4.f, 5.f, 6.f, 7.f);
  CHECK(lanes(x4::shuffle<3, 1, 2, 0>(a)) == float4(3.f, 1.f, 2.f, 0.f));
  CHECK(lanes(x4::shuffle<2, 0, 3, 1>(a, b)) == float4(2.f, 0.f, 7.f, 5.f));
  CHECK(lanes(x4::splat<2>(a)) == float4(2.f));
  CHECK(x4::get<0>(a) == 0.f);
  CHECK(x4::get<3>(a) == 3.f);
}

TEST_CASE("math register: negate flips only the flagged lanes, zeros too") {
  const float4 r =
      lanes(x4::negate<true, false, true, false>(x4::set(0.f, 1.f, 2.f, 0.f)));
  CHECK(std::signbit(r.x));
  CHECK(r.y == 1.f);
  CHECK(r.z == -2.f);
  CHECK_FALSE(std::signbit(r.w));
}

TEST_CASE("math register: min and max return the second operand beside NaN") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float4 lo = lanes(
      x4::min(x4::set(nan, 1.f, 2.f, -1.f), x4::set(1.f, nan, 3.f, -2.f)));
  CHECK(lo.x == 1.f);
  CHECK(std::isnan(lo.y));
  CHECK(lo.z == 2.f);
  CHECK(lo.w == -2.f);
  const float4 hi = lanes(
      x4::max(x4::set(nan, 1.f, 2.f, -1.f), x4::set(1.f, nan, 3.f, -2.f)));
  CHECK(hi.x == 1.f);
  CHECK(std::isnan(hi.y));
  CHECK(hi.z == 3.f);
  CHECK(hi.w == -1.f);
  CHECK(nx::math::min(nan, 1.f) == 1.f);
  CHECK(std::isnan(nx::math::min(1.f, nan)));
}

TEST_CASE("math register: dot4 sums the pairs first") {
  // Left to right this is ((1e8 + 1) - 1e8) + 1 = 1; pairwise it is 0.
  const f32x4 a = x4::set(1e8f, 1.f, -1e8f, 1.f);
  const float4 d = lanes(x4::dot4(a, x4::splat(1.f)));
  CHECK(d == float4(0.f));
  CHECK(dot(float4(1e8f, 1.f, -1e8f, 1.f), float4(1.f)) == 0.f);
}

TEST_CASE("math register: arithmetic is exact per lane") {
  lcg g{7};
  for (int i = 0; i < 64; ++i) {
    const float4 a = g.next4(), b = g.next4() + float4(2.f);
    const f32x4 va = x4::load(&a.x), vb = x4::load(&b.x);
    CHECK(same_bits(lanes(x4::add(va, vb)), a + b));
    CHECK(same_bits(lanes(x4::sub(va, vb)), a - b));
    CHECK(same_bits(lanes(x4::mul(va, vb)), a * b));
    CHECK(same_bits(lanes(x4::div(va, vb)), a / b));
    CHECK(same_bits(lanes(x4::sqrt(vb)), sqrt(b)));
  }
}

TEST_CASE("math register: transpose swaps rows and columns") {
  f32x4 r0 = x4::set(0, 1, 2, 3), r1 = x4::set(4, 5, 6, 7);
  f32x4 r2 = x4::set(8, 9, 10, 11), r3 = x4::set(12, 13, 14, 15);
  x4::transpose(r0, r1, r2, r3);
  CHECK(lanes(r0) == float4(0, 4, 8, 12));
  CHECK(lanes(r1) == float4(1, 5, 9, 13));
  CHECK(lanes(r2) == float4(2, 6, 10, 14));
  CHECK(lanes(r3) == float4(3, 7, 11, 15));
}
