#include "math/math_test_util.h"

#include <cmath>
#include <cstring>

using namespace math_test;

NX_MATH_BEGIN

namespace {

// glm's formulas, written out in its operation order on plain floats: the
// products must equal them bit for bit.
float4x4 glm_mul(const float4x4 &a, const float4x4 &b) {
  float4x4 m;
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) {
      float s = a.columns[0][r] * b.columns[c][0];
      s += a.columns[1][r] * b.columns[c][1];
      s += a.columns[2][r] * b.columns[c][2];
      s += a.columns[3][r] * b.columns[c][3];
      m.columns[c][r] = s;
    }
  return m;
}

float4 glm_mul(const float4x4 &a, const float4 &v) {
  float4 out;
  for (int r = 0; r < 4; ++r)
    out[r] = (a.columns[0][r] * v.x + a.columns[1][r] * v.y) +
             (a.columns[2][r] * v.z + a.columns[3][r] * v.w);
  return out;
}

float4x4 glm_inverse(const float4x4 &mm) {
  float m[4][4];
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      m[c][r] = mm.columns[c][r];
  const float c00 = m[2][2] * m[3][3] - m[3][2] * m[2][3];
  const float c02 = m[1][2] * m[3][3] - m[3][2] * m[1][3];
  const float c03 = m[1][2] * m[2][3] - m[2][2] * m[1][3];
  const float c04 = m[2][1] * m[3][3] - m[3][1] * m[2][3];
  const float c06 = m[1][1] * m[3][3] - m[3][1] * m[1][3];
  const float c07 = m[1][1] * m[2][3] - m[2][1] * m[1][3];
  const float c08 = m[2][1] * m[3][2] - m[3][1] * m[2][2];
  const float c10 = m[1][1] * m[3][2] - m[3][1] * m[1][2];
  const float c11 = m[1][1] * m[2][2] - m[2][1] * m[1][2];
  const float c12 = m[2][0] * m[3][3] - m[3][0] * m[2][3];
  const float c14 = m[1][0] * m[3][3] - m[3][0] * m[1][3];
  const float c15 = m[1][0] * m[2][3] - m[2][0] * m[1][3];
  const float c16 = m[2][0] * m[3][2] - m[3][0] * m[2][2];
  const float c18 = m[1][0] * m[3][2] - m[3][0] * m[1][2];
  const float c19 = m[1][0] * m[2][2] - m[2][0] * m[1][2];
  const float c20 = m[2][0] * m[3][1] - m[3][0] * m[2][1];
  const float c22 = m[1][0] * m[3][1] - m[3][0] * m[1][1];
  const float c23 = m[1][0] * m[2][1] - m[2][0] * m[1][1];
  const float4 f0(c00, c00, c02, c03), f1(c04, c04, c06, c07);
  const float4 f2(c08, c08, c10, c11), f3(c12, c12, c14, c15);
  const float4 f4(c16, c16, c18, c19), f5(c20, c20, c22, c23);
  const float4 v0(m[1][0], m[0][0], m[0][0], m[0][0]);
  const float4 v1(m[1][1], m[0][1], m[0][1], m[0][1]);
  const float4 v2(m[1][2], m[0][2], m[0][2], m[0][2]);
  const float4 v3(m[1][3], m[0][3], m[0][3], m[0][3]);
  const float4 sa(1.f, -1.f, 1.f, -1.f), sb(-1.f, 1.f, -1.f, 1.f);
  const float4 i0 = (v1 * f0 - v2 * f1 + v3 * f2) * sa;
  const float4 i1 = (v0 * f0 - v2 * f3 + v3 * f4) * sb;
  const float4 i2 = (v0 * f1 - v1 * f3 + v3 * f5) * sa;
  const float4 i3 = (v0 * f2 - v1 * f4 + v2 * f5) * sb;
  const float4 d0 = mm.columns[0] * float4(i0.x, i1.x, i2.x, i3.x);
  const float d = 1.f / ((d0.x + d0.y) + (d0.z + d0.w));
  return float4x4::from_columns(i0 * d, i1 * d, i2 * d, i3 * d);
}

double4x4 widen(const float4x4 &m) {
  double4x4 r;
  for (int c = 0; c < 4; ++c)
    r.columns[c] = double4(m.columns[c]);
  return r;
}

/// A well-conditioned matrix: random entries on a dominant diagonal.
float4x4 conditioned(lcg &g) {
  float4x4 m = g.next4x4();
  for (int i = 0; i < 4; ++i)
    m.columns[i][i] += 4.f;
  return m;
}

} // namespace

NX_MATH_END

TEST_CASE("math matrix: rows, columns and storage") {
  const float3x4 m = float3x4::from_rows(float4(0.f, 1.f, 2.f, 3.f),
                                         float4(4.f, 5.f, 6.f, 7.f),
                                         float4(8.f, 9.f, 10.f, 11.f));
  CHECK(m.row(1) == float4(4.f, 5.f, 6.f, 7.f));
  CHECK(m.columns[3] == float3(3.f, 7.f, 11.f));
  const float *p = &m.columns[0].x;
  // Column-major, as glm stores a mat4x3.
  CHECK(p[0] == 0.f);
  CHECK(p[1] == 4.f);
  CHECK(p[3] == 1.f);
  CHECK(p[11] == 11.f);
  CHECK(float3x4::identity().columns[3] == float3(0.f));
  CHECK(float3x4::identity().row(2) == float4(0.f, 0.f, 1.f, 0.f));
  CHECK(transpose(m) == float4x3::from_columns(m.row(0), m.row(1), m.row(2)));
  CHECK(float4x4::identity() ==
        float4x4::from_rows(float4(1, 0, 0, 0), float4(0, 1, 0, 0),
                            float4(0, 0, 1, 0), float4(0, 0, 0, 1)));
}

TEST_CASE("math matrix: 4x4 products sum in glm's order") {
  lcg g{1};
  for (int i = 0; i < 256; ++i) {
    const float4x4 a = g.next4x4(), b = g.next4x4();
    const float4 v = g.next4();
    CHECK(same_bits(mul(a, b), glm_mul(a, b)));
    CHECK(same_bits(mul(a, v), glm_mul(a, v)));
    const float4 rv = mul(v, a);
    for (int c = 0; c < 4; ++c)
      CHECK(same_bits(rv[c], dot(v, a.columns[c])));
    CHECK(same_bits(transpose(a),
                    float4x4::from_rows(a.columns[0], a.columns[1],
                                        a.columns[2], a.columns[3])));
    CHECK(near(widen(mul(a, b)), mul(widen(a), widen(b)), 1e-5));
  }
}

TEST_CASE("math matrix: products of other shapes") {
  const float2x3 a =
      float2x3::from_rows(float3(1.f, 2.f, 3.f), float3(4.f, 5.f, 6.f));
  const float3x2 b = float3x2::from_rows(float2(7.f, 8.f), float2(9.f, 10.f),
                                         float2(11.f, 12.f));
  CHECK(mul(a, b) ==
        float2x2::from_rows(float2(58.f, 64.f), float2(139.f, 154.f)));
  CHECK(mul(a, float3(1.f, 0.f, -1.f)) == float2(-2.f, -2.f));
  CHECK(mul(float2(1.f, -1.f), a) == float3(-3.f, -3.f, -3.f));
}

TEST_CASE("math matrix: affine products and transforms") {
  lcg g{2};
  for (int i = 0; i < 128; ++i) {
    const float3x4 a =
        float3x4::from_columns(g.next3(), g.next3(), g.next3(), g.next3());
    const float3x4 b =
        float3x4::from_columns(g.next3(), g.next3(), g.next3(), g.next3());
    const auto full = [](const float3x4 &m) {
      return float4x4::from_columns(
          float4(m.columns[0], 0.f), float4(m.columns[1], 0.f),
          float4(m.columns[2], 0.f), float4(m.columns[3], 1.f));
    };
    const float4x4 ab = mul(full(a), full(b));
    const float3x4 p = affine_mul(a, b);
    for (int c = 0; c < 4; ++c)
      CHECK(p.columns[c] == ab.columns[c].xyz());
    const float3 v = g.next3();
    CHECK(transform_point(a, v) == mul(a, float4(v, 1.f)));
    CHECK(near(transform_vector(a, v), mul(a, float4(v, 0.f))));
    CHECK(transform_point(ab, v) == mul(ab, float4(v, 1.f)).xyz());
    CHECK(near(transform_vector(ab, v), mul(ab, float4(v, 0.f)).xyz()));
  }
}

TEST_CASE("math matrix: affine products over raw columns of either stride") {
  lcg g{12};
  for (int i = 0; i < 128; ++i) {
    const float3x4 a =
        float3x4::from_columns(g.next3(), g.next3(), g.next3(), g.next3());
    const float3x4 b =
        float3x4::from_columns(g.next3(), g.next3(), g.next3(), g.next3());
    // The fourth row is never read.
    const auto wide = [&](const float3x4 &m) {
      return float4x4::from_columns(
          float4(m.columns[0], g.next3().x), float4(m.columns[1], g.next3().x),
          float4(m.columns[2], g.next3().x), float4(m.columns[3], g.next3().x));
    };
    const float4x4 wa = wide(a), wb = wide(b);
    const float3x4 want = affine_mul(a, b);
    const float *pa = &a.columns[0].x, *pb = &b.columns[0].x;
    const float *qa = &wa.columns[0].x, *qb = &wb.columns[0].x;
    const auto check = [&](auto &&run) {
      float out[13];
      out[12] = 1234.5f;
      run(out);
      CHECK(std::memcmp(out, &want.columns[0].x, 12 * sizeof(float)) == 0);
      CHECK(out[12] == 1234.5f);
    };
    check([&](float *o) { affine_mul_columns<3, 3>(pa, pb, o); });
    check([&](float *o) { affine_mul_columns<4, 3>(qa, pb, o); });
    check([&](float *o) { affine_mul_columns<3, 4>(pa, qb, o); });
    check([&](float *o) { affine_mul_columns<4, 4>(qa, qb, o); });
  }
}

TEST_CASE("math matrix: a float3x4's rows, 4 floats each") {
  lcg g{14};
  for (int i = 0; i < 64; ++i) {
    const float3x4 m =
        float3x4::from_columns(g.next3(), g.next3(), g.next3(), g.next3());
    float out[13];
    out[12] = 1234.5f;
    affine_rows(&m.columns[0].x, out);
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 4; ++c)
        CHECK(out[4 * r + c] == m.columns[c][r]);
    CHECK(out[12] == 1234.5f);
  }
}

TEST_CASE("math matrix: a matrix as another shape") {
  lcg g{22};
  for (int i = 0; i < 64; ++i) {
    const float4x4 m = g.next4x4();
    const float3x4 a = to_float3x4(m);
    const float3x3 b = to_float3x3(m);
    for (int c = 0; c < 3; ++c) {
      CHECK(a.columns[c] == m.columns[c].xyz());
      CHECK(b.columns[c] == m.columns[c].xyz());
      CHECK(to_float3x3(a).columns[c] == m.columns[c].xyz());
      CHECK(to_float3x4(b).columns[c] == m.columns[c].xyz());
      CHECK(to_float4x4(a).columns[c] == float4(m.columns[c].xyz(), 0.f));
      CHECK(to_float4x4(b).columns[c] == float4(m.columns[c].xyz(), 0.f));
    }
    CHECK(a.columns[3] == m.columns[3].xyz());
    CHECK(to_float3x4(b).columns[3] == float3(0.f));
    CHECK(to_float4x4(a).columns[3] == float4(m.columns[3].xyz(), 1.f));
    CHECK(to_float4x4(b).columns[3] == float4(0.f, 0.f, 0.f, 1.f));
  }
  static_assert(std::is_trivial_v<float4x4> && std::is_trivial_v<float3x4>);
  CHECK(float3x4{}.columns[3] == float3(0.f));
}

TEST_CASE("math matrix: an affine product by a transform") {
  lcg g{13};
  for (int i = 0; i < 256; ++i) {
    const float3x4 a =
        float3x4::from_columns(g.next3(), g.next3(), g.next3(), g.next3());
    const float4x4 wide_a = float4x4::from_columns(
        float4(a.columns[0], g.next3().x), float4(a.columns[1], g.next3().x),
        float4(a.columns[2], g.next3().x), float4(a.columns[3], g.next3().x));
    // Not unit length: the formula's bits hold for any quaternion.
    const float3 v = g.next3();
    const quat q(v, g.next3().x);
    const float rotation[4] = {q.x, q.y, q.z, q.w};
    const float3 s = g.next3(), t = g.next3();

    const float3x3 r = to_float3x3(q);
    const float3x4 b = float3x4::from_columns(
        r.columns[0] * s.x, r.columns[1] * s.y, r.columns[2] * s.z, t);
    const float3x4 want = affine_mul(a, b);
    float out[13];
    out[12] = 1234.5f;
    affine_mul_trs<3>(&a.columns[0].x, rotation, &s.x, &t.x, out);
    CHECK(std::memcmp(out, &want.columns[0].x, 12 * sizeof(float)) == 0);
    CHECK(out[12] == 1234.5f);
    affine_mul_trs<4>(&wide_a.columns[0].x, rotation, &s.x, &t.x, out);
    CHECK(std::memcmp(out, &want.columns[0].x, 12 * sizeof(float)) == 0);
    CHECK(out[12] == 1234.5f);
    affine_trs(rotation, &s.x, &t.x, out);
    CHECK(std::memcmp(out, &b.columns[0].x, 12 * sizeof(float)) == 0);
    CHECK(out[12] == 1234.5f);
  }
}

TEST_CASE("math matrix: the 4x4 inverse is glm's") {
  lcg g{3};
  for (int i = 0; i < 256; ++i) {
    const float4x4 m = conditioned(g);
    const float4x4 inv = inverse(m);
    CHECK(same_bits(inv, glm_inverse(m)));
    CHECK(near(mul(m, inv), float4x4::identity(), 1e-5));
  }
  const float4x4 singular{};
  CHECK_FALSE(std::isfinite(inverse(singular).columns[0].x));
}

TEST_CASE("math matrix: determinants and small inverses") {
  lcg g{4};
  for (int i = 0; i < 64; ++i) {
    const float4x4 a = conditioned(g), b = conditioned(g);
    CHECK(
        determinant(mul(a, b)) ==
        nxtest::Approx(double(determinant(a)) * determinant(b)).epsilon(1e-4));
    const float3x3 m3 = float3x3::from_columns(
        a.columns[0].xyz(), a.columns[1].xyz(), a.columns[2].xyz());
    CHECK(near(mul(m3, inverse(m3)), float3x3::identity(), 1e-5));
    CHECK(determinant(inverse(m3)) ==
          nxtest::Approx(1.0 / determinant(m3)).epsilon(1e-4));
    const float2x2 m2 =
        float2x2::from_columns(a.columns[0].xy(), a.columns[1].xy());
    CHECK(near(mul(inverse(m2), m2), float2x2::identity(), 1e-5));
  }
  CHECK(determinant(float4x4::identity()) == 1.f);
  CHECK(determinant(float3x3::from_rows(float3(2, 0, 0), float3(0, 3, 0),
                                        float3(0, 0, 4))) == 24.f);
}
