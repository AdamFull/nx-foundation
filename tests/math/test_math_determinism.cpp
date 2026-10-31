#include "math/math_test_util.h"

using namespace math_test;

namespace {

struct hasher {
  uint64_t h = 1469598103934665603ull;
  template <class T> void add(const T &v) {
    const auto *p = reinterpret_cast<const unsigned char *>(&v);
    for (size_t i = 0; i < sizeof(T); ++i)
      h = (h ^ p[i]) * 1099511628211ull;
  }
};

} // namespace

// Only +, -, *, / and sqrt, which IEEE rounds exactly: a different hash means
// a path that sums in another order or fuses a multiply-add.
TEST_CASE("math determinism: every backend computes the same bits") {
  lcg g{12345};
  hasher h;
  for (int i = 0; i < 512; ++i) {
    const float4x4 a = g.next4x4(), b = g.next4x4();
    const float4 v4 = g.next4();
    const float3 v3 = g.next3(), w3 = g.next3();
    const quat p = g.next_quat(), q = g.next_quat();
    const float3x4 af =
        float3x4::from_columns(g.next3(), g.next3(), g.next3(), g.next3());
    h.add(mul(a, b));
    h.add(mul(a, v4));
    h.add(mul(v4, a));
    h.add(inverse(a));
    h.add(transpose(a));
    h.add(determinant(a));
    h.add(affine_mul(af, af));
    h.add(transform_point(a, v3));
    h.add(transform_vector(a, v3));
    h.add(transform_point(af, v3));
    h.add(transform_vector(af, v3));
    h.add(normalize(v3));
    h.add(cross(v3, w3));
    h.add(dot(v4, v4));
    h.add(length(v3));
    h.add(lerp(v3, w3, 0.3f));
    h.add(f16tof32(f32tof16(v3.x)));
    h.add(angle(p));
    h.add(axis(p));
    h.add(look_at_quat_rh(normalize(v3), float3(0.f, 1.f, 0.f)));
    h.add(to_float3x4(a));
    h.add(to_float4x4(af));
    h.add(reflect(v3, normalize(w3)));
    h.add(mul(p, q));
    h.add(rotate(p, v3));
    h.add(normalize(p));
    h.add(inverse(p));
    h.add(to_float3x3(p));
    h.add(to_quat(to_float3x3(p)));
    h.add(inverse(to_float3x3(p)));
    h.add(translation(v3));
    h.add(orthographic_rh_zo(-1.f, v3.x + 2.f, -1.f, v3.y + 2.f, 0.1f, 10.f));
    h.add(look_at_rh(v3, w3 + float3(3.f), float3(0.f, 1.f, 0.f)));
  }
  CHECK(h.h == 0x84dc604125619fa2ull);
}

TEST_CASE("math half: two halves share a word, x in the low bits") {
  CHECK(nx::math::pack_half2x16(nx::math::float2(1.f, -2.f)) == 0xC0003C00u);
  const nx::math::float2 back = nx::math::unpack_half2x16(0xC0003C00u);
  CHECK(back.x == 1.f);
  CHECK(back.y == -2.f);
}
