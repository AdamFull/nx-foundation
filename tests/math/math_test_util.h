#pragma once

#include "framework/nxtest.h"

#include "core/foundation/math/math.h"

#include <cstdint>
#include <cstring>

NX_MATH_BEGIN

namespace math_test {

using namespace nx::math;

/// A fixed generator, so the determinism hash never depends on nx::rng.
struct lcg {
  uint32_t s;
  float next(float lo = -1.f, float hi = 1.f) {
    s = s * 1664525u + 1013904223u;
    return lo + (hi - lo) * float(s >> 8) * (1.f / 16777216.f);
  }
  float3 next3() { return float3(next(), next(), next()); }
  float4 next4() { return float4(next(), next(), next(), next()); }
  float4x4 next4x4() {
    return float4x4::from_columns(next4(), next4(), next4(), next4());
  }
  quat next_quat() { return normalize(quat(next4())); }
};

template <class T> bool same_bits(const T &a, const T &b) {
  return std::memcmp(&a, &b, sizeof(T)) == 0;
}

template <class T, int N>
bool near(const vec<T, N> &a, const vec<T, N> &b, double tolerance = 1e-5) {
  for (int i = 0; i < N; ++i)
    if (!(double(a[i]) == nxtest::Approx(double(b[i])).margin(tolerance)))
      return false;
  return true;
}
template <class T, int R, int C>
bool near(const mat<T, R, C> &a, const mat<T, R, C> &b,
          double tolerance = 1e-5) {
  for (int c = 0; c < C; ++c)
    if (!near(a.columns[c], b.columns[c], tolerance))
      return false;
  return true;
}
inline bool near(const quat &a, const quat &b, double tolerance = 1e-5) {
  return near(a.xyzw(), b.xyzw(), tolerance);
}

} // namespace math_test

NX_MATH_END
