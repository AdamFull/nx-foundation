#include "core/foundation/core/noise.h"

#include <algorithm>

namespace nx::noise {

namespace {

constexpr i64 HALF = ONE / 2;

[[nodiscard]] constexpr i64 floor_div(const i64 a, const i64 b) noexcept {
  const i64 q = a / b;
  return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

/// Lattice coordinates leave i32 for a small period far out; they wrap, as
/// the hash only needs them to differ from their neighbours.
[[nodiscard]] constexpr i32 wrap(const i64 v) noexcept {
  return static_cast<i32>(static_cast<u32>(static_cast<u64>(v)));
}

/// @p x in lattice units, Q16.16.
[[nodiscard]] constexpr i64 scaled(const i32 x, const i32 period) noexcept {
  return floor_div(static_cast<i64>(x) * ONE, std::max(period, 1));
}

[[nodiscard]] constexpr i64 ease(const i64 t) noexcept {
  return (((t * t) >> 16) * (3 * ONE - 2 * t)) >> 16;
}

[[nodiscard]] constexpr i64 lerp(const i64 a, const i64 b,
                                 const i64 t) noexcept {
  return a + (((b - a) * t) >> 16);
}

/// A lattice point's value, in [-ONE, ONE).
[[nodiscard]] constexpr i64 level(const u32 h) noexcept {
  return static_cast<i64>(h >> 15) - ONE;
}

[[nodiscard]] constexpr i32 clamp_one(const i64 v) noexcept {
  return static_cast<i32>(std::clamp<i64>(v, -ONE, ONE));
}

[[nodiscard]] constexpr u64 isqrt(u64 v) noexcept {
  u64 root = 0;
  u64 bit = u64{1} << 62;
  while (bit > v)
    bit >>= 2;
  while (bit != 0) {
    if (v >= root + bit) {
      v -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return root;
}

constexpr i8 GRADIENTS_2D[8][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1},
                                   {1, 0}, {-1, 0}, {0, 1},  {0, -1}};

constexpr i8 GRADIENTS_3D[12][3] = {
    {1, 1, 0},  {-1, 1, 0},  {1, -1, 0}, {-1, -1, 0}, {1, 0, 1},  {-1, 0, 1},
    {1, 0, -1}, {-1, 0, -1}, {0, 1, 1},  {0, -1, 1},  {0, 1, -1}, {0, -1, -1}};

[[nodiscard]] i64 corner(const u32 seed, const i64 i, const i64 j, const i64 dx,
                         const i64 dy) noexcept {
  i64 t = HALF - ((dx * dx + dy * dy) >> 16);
  if (t <= 0)
    return 0;
  t = (t * t) >> 16;
  t = (t * t) >> 16;
  const i8 *const g = GRADIENTS_2D[hash(seed, wrap(i), wrap(j)) & 7u];
  return (t * (g[0] * dx + g[1] * dy)) >> 16;
}

// 0.6 in Q16.16, the reach of a 3D simplex corner.
constexpr i64 REACH_3D = 39322;

[[nodiscard]] i64 corner(const u32 seed, const i64 i, const i64 j, const i64 k,
                         const i64 dx, const i64 dy, const i64 dz) noexcept {
  i64 t = REACH_3D - ((dx * dx + dy * dy + dz * dz) >> 16);
  if (t <= 0)
    return 0;
  t = (t * t) >> 16;
  t = (t * t) >> 16;
  const i8 *const g = GRADIENTS_3D[hash(seed, wrap(i), wrap(j), wrap(k)) % 12u];
  return (t * (g[0] * dx + g[1] * dy + g[2] * dz)) >> 16;
}

} // namespace

i32 value(const u32 seed, const i32 x, const i32 y, const i32 period) noexcept {
  const i64 sx = scaled(x, period);
  const i64 sy = scaled(y, period);
  const i64 cx = sx >> 16;
  const i64 cy = sy >> 16;
  const i64 tx = ease(sx - (cx << 16));
  const i64 ty = ease(sy - (cy << 16));
  const auto at = [&](const i64 dx, const i64 dy) {
    return level(hash(seed, wrap(cx + dx), wrap(cy + dy)));
  };
  return clamp_one(
      lerp(lerp(at(0, 0), at(1, 0), tx), lerp(at(0, 1), at(1, 1), tx), ty));
}

i32 value(const u32 seed, const i32 x, const i32 y, const i32 z,
          const i32 period) noexcept {
  const i64 sx = scaled(x, period);
  const i64 sy = scaled(y, period);
  const i64 sz = scaled(z, period);
  const i64 cx = sx >> 16;
  const i64 cy = sy >> 16;
  const i64 cz = sz >> 16;
  const i64 tx = ease(sx - (cx << 16));
  const i64 ty = ease(sy - (cy << 16));
  const i64 tz = ease(sz - (cz << 16));
  const auto at = [&](const i64 dx, const i64 dy, const i64 dz) {
    return level(hash(seed, wrap(cx + dx), wrap(cy + dy), wrap(cz + dz)));
  };
  const auto plane = [&](const i64 dz) {
    return lerp(lerp(at(0, 0, dz), at(1, 0, dz), tx),
                lerp(at(0, 1, dz), at(1, 1, dz), tx), ty);
  };
  return clamp_one(lerp(plane(0), plane(1), tz));
}

i32 simplex(const u32 seed, const i32 x, const i32 y,
            const i32 period) noexcept {
  // (sqrt(3) - 1) / 2 and (3 - sqrt(3)) / 6, Q16.16.
  constexpr i64 SKEW = 23988;
  constexpr i64 UNSKEW = 13849;
  const i64 sx = scaled(x, period);
  const i64 sy = scaled(y, period);
  const i64 s = ((sx + sy) * SKEW) >> 16;
  const i64 i = (sx + s) >> 16;
  const i64 j = (sy + s) >> 16;
  const i64 t = (i + j) * UNSKEW;
  const i64 x0 = sx - ((i << 16) - t);
  const i64 y0 = sy - ((j << 16) - t);
  const i64 i1 = x0 > y0 ? 1 : 0;
  const i64 j1 = 1 - i1;
  const i64 sum =
      corner(seed, i, j, x0, y0) +
      corner(seed, i + i1, j + j1, x0 - (i1 << 16) + UNSKEW,
             y0 - (j1 << 16) + UNSKEW) +
      corner(seed, i + 1, j + 1, x0 - ONE + 2 * UNSKEW, y0 - ONE + 2 * UNSKEW);
  return clamp_one(70 * sum);
}

i32 simplex(const u32 seed, const i32 x, const i32 y, const i32 z,
            const i32 period) noexcept {
  const i64 sx = scaled(x, period);
  const i64 sy = scaled(y, period);
  const i64 sz = scaled(z, period);
  const i64 s = floor_div(sx + sy + sz, 3);
  const i64 i = (sx + s) >> 16;
  const i64 j = (sy + s) >> 16;
  const i64 k = (sz + s) >> 16;
  const i64 t = floor_div((i + j + k) << 16, 6);
  const i64 x0 = sx - ((i << 16) - t);
  const i64 y0 = sy - ((j << 16) - t);
  const i64 z0 = sz - ((k << 16) - t);
  // The second and third corners: one step along the largest offset, then
  // along the two largest.
  i64 i1 = 0, j1 = 0, k1 = 0, i2 = 0, j2 = 0, k2 = 0;
  if (x0 >= y0) {
    if (y0 >= z0) {
      i1 = 1, i2 = 1, j2 = 1;
    } else if (x0 >= z0) {
      i1 = 1, i2 = 1, k2 = 1;
    } else {
      k1 = 1, i2 = 1, k2 = 1;
    }
  } else if (y0 < z0) {
    k1 = 1, j2 = 1, k2 = 1;
  } else if (x0 < z0) {
    j1 = 1, j2 = 1, k2 = 1;
  } else {
    j1 = 1, i2 = 1, j2 = 1;
  }
  constexpr i64 SIXTH = ONE / 6;
  const i64 sum =
      corner(seed, i, j, k, x0, y0, z0) +
      corner(seed, i + i1, j + j1, k + k1, x0 - (i1 << 16) + SIXTH,
             y0 - (j1 << 16) + SIXTH, z0 - (k1 << 16) + SIXTH) +
      corner(seed, i + i2, j + j2, k + k2, x0 - (i2 << 16) + 2 * SIXTH,
             y0 - (j2 << 16) + 2 * SIXTH, z0 - (k2 << 16) + 2 * SIXTH) +
      corner(seed, i + 1, j + 1, k + 1, x0 - HALF, y0 - HALF, z0 - HALF);
  return clamp_one(32 * sum);
}

cell cellular(const u32 seed, const i32 x, const i32 y,
              const i32 period) noexcept {
  const i64 sx = scaled(x, period);
  const i64 sy = scaled(y, period);
  const i64 cx = sx >> 16;
  const i64 cy = sy >> 16;
  const i64 fx = sx - (cx << 16);
  const i64 fy = sy - (cy << 16);
  u64 nearest = ~u64{0};
  u64 second = ~u64{0};
  u32 id = 0;
  for (i64 dy = -1; dy <= 1; ++dy)
    for (i64 dx = -1; dx <= 1; ++dx) {
      const u32 h = hash(seed, wrap(cx + dx), wrap(cy + dy));
      const i64 px = (dx << 16) + (h & 0xffffu) - fx;
      const i64 py = (dy << 16) + (h >> 16) - fy;
      const u64 d = static_cast<u64>(px * px + py * py);
      if (d < nearest) {
        second = nearest;
        nearest = d;
        id = mix(h);
      } else if (d < second) {
        second = d;
      }
    }
  return {static_cast<i32>(isqrt(nearest)), static_cast<i32>(isqrt(second)),
          id};
}

cell cellular(const u32 seed, const i32 x, const i32 y, const i32 z,
              const i32 period) noexcept {
  const i64 sx = scaled(x, period);
  const i64 sy = scaled(y, period);
  const i64 sz = scaled(z, period);
  const i64 cx = sx >> 16;
  const i64 cy = sy >> 16;
  const i64 cz = sz >> 16;
  const i64 fx = sx - (cx << 16);
  const i64 fy = sy - (cy << 16);
  const i64 fz = sz - (cz << 16);
  u64 nearest = ~u64{0};
  u64 second = ~u64{0};
  u32 id = 0;
  for (i64 dz = -1; dz <= 1; ++dz)
    for (i64 dy = -1; dy <= 1; ++dy)
      for (i64 dx = -1; dx <= 1; ++dx) {
        const u32 h = hash(seed, wrap(cx + dx), wrap(cy + dy), wrap(cz + dz));
        const u32 more = mix(h);
        const i64 px = (dx << 16) + (h & 0xffffu) - fx;
        const i64 py = (dy << 16) + (h >> 16) - fy;
        const i64 pz = (dz << 16) + (more & 0xffffu) - fz;
        const u64 d = static_cast<u64>(px * px + py * py + pz * pz);
        if (d < nearest) {
          second = nearest;
          nearest = d;
          id = more;
        } else if (d < second) {
          second = d;
        }
      }
  return {static_cast<i32>(isqrt(nearest)), static_cast<i32>(isqrt(second)),
          id};
}

} // namespace nx::noise
