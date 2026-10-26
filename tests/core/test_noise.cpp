#include "framework/nxtest.h"

#include "core/foundation/containers/hash_set.h"
#include "core/foundation/core/noise.h"

#include <array>
#include <cstdlib>
#include <utility>

namespace noise = nx::noise;

namespace {

constexpr u32 SEED = 1234;

/// FNV-1a over every sample of @p f on a grid across zero, at three periods.
template <typename F> [[nodiscard]] u64 digest(F &&f) {
  u64 h = 0xcbf29ce484222325ull;
  for (const i32 period : {1, 7, 64})
    for (i32 y = -40; y < 40; y += 3)
      for (i32 x = -40; x < 40; x += 3) {
        const u32 v = static_cast<u32>(f(x, y, period));
        for (u32 b = 0; b < 4; ++b) {
          h ^= (v >> (8 * b)) & 0xffu;
          h *= 0x100000001b3ull;
        }
      }
  return h;
}

/// The largest change between neighbouring samples of @p f along a line
/// across zero.
template <typename F> [[nodiscard]] i32 steepest(F &&f) {
  i32 most = 0;
  for (i32 x = -300; x < 300; ++x)
    most = std::max(most, std::abs(f(x + 1) - f(x)));
  return most;
}

/// Whether @p f is near zero a quarter, half and three quarters of the way
/// from @p from to @p to.
template <usize N, typename F>
[[nodiscard]] bool flat_between(const std::array<i64, N> &from,
                                const std::array<i64, N> &to, F &&f) {
  for (i64 q = 1; q < 4; ++q) {
    std::array<i32, N> at{};
    for (usize a = 0; a < N; ++a)
      at[a] = static_cast<i32>(from[a] + (to[a] - from[a]) * q / 4);
    if (std::abs(f(at)) >= noise::ONE / 64)
      return false;
  }
  return true;
}

} // namespace

TEST_CASE("noise: a seed and a coordinate give the same bits everywhere") {
  // Pinned: a change here changes every generated world.
  CHECK(digest([](i32 x, i32 y, i32 p) {
          return noise::value(SEED, x, y, p);
        }) == 18199843432403111049ull);
  CHECK(digest([](i32 x, i32 y, i32 p) {
          return noise::value(SEED, x, y, x ^ y, p);
        }) == 713502327121048357ull);
  CHECK(digest([](i32 x, i32 y, i32 p) {
          return noise::simplex(SEED, x, y, p);
        }) == 6749378938950155345ull);
  CHECK(digest([](i32 x, i32 y, i32 p) {
          return noise::simplex(SEED, x, y, x - y, p);
        }) == 7663130524761051966ull);
  CHECK(digest([](i32 x, i32 y, i32 p) {
          const noise::cell c = noise::cellular(SEED, x, y, p);
          return c.nearest ^ c.second ^ static_cast<i32>(c.id);
        }) == 1206267361924328885ull);
  CHECK(digest([](i32 x, i32 y, i32 p) {
          const noise::cell c = noise::cellular(SEED, x, y, x + y, p);
          return c.nearest ^ c.second ^ static_cast<i32>(c.id);
        }) == 10867251358360776955ull);
  CHECK(noise::hash(SEED, -3, 4, 5) == 234107453u);
}

TEST_CASE("noise: value and simplex noise stay in range, reach across it and "
          "average near zero") {
  for (u32 kind = 0; kind < 4; ++kind) {
    i64 sum = 0;
    i32 low = noise::ONE;
    i32 high = -noise::ONE;
    u32 count = 0;
    for (i32 y = -200; y < 200; y += 3)
      for (i32 x = -200; x < 200; x += 3) {
        const i32 v = kind == 0   ? noise::value(SEED, x, y, 16)
                      : kind == 1 ? noise::value(SEED, x, y, 7 - x, 16)
                      : kind == 2 ? noise::simplex(SEED, x, y, 16)
                                  : noise::simplex(SEED, x, y, 3 * y, 16);
        REQUIRE(v >= -noise::ONE);
        REQUIRE(v <= noise::ONE);
        sum += v;
        low = std::min(low, v);
        high = std::max(high, v);
        ++count;
      }
    CHECK(high > noise::ONE / 2);
    CHECK(low < -noise::ONE / 2);
    CHECK(std::abs(sum / count) < noise::ONE / 10);
  }
}

TEST_CASE("noise: neighbouring samples differ by little, across zero too") {
  // At a long period a seam would stand out against the step allowed.
  for (const i32 period : {32, 256}) {
    // Eased value noise rises at most 1.5 times its lattice step; simplex's
    // slope stays within 8 a lattice step.
    const i32 value_limit = 3 * noise::ONE / period + 2;
    const i32 simplex_limit = 8 * noise::ONE / period + 2;
    const i32 distance_limit = noise::ONE / period + 2;
    for (const i32 offset : {-5, 0, 11}) {
      CHECK(steepest([&](i32 x) {
              return noise::value(SEED, x, offset, period);
            }) <= value_limit);
      CHECK(steepest([&](i32 x) {
              return noise::value(SEED, offset, x, x / 2, period);
            }) <= value_limit * 2);
      CHECK(steepest([&](i32 x) {
              return noise::simplex(SEED, x, offset, period);
            }) <= simplex_limit);
      CHECK(steepest([&](i32 x) {
              return noise::simplex(SEED, offset, offset, x, period);
            }) <= simplex_limit);
      // A distance moves no faster than the point measured from.
      CHECK(steepest([&](i32 x) {
              return noise::cellular(SEED, x, offset, period).nearest;
            }) <= distance_limit);
      CHECK(steepest([&](i32 x) {
              return noise::cellular(SEED, offset, x, offset, period).nearest;
            }) <= distance_limit);
    }
  }
}

TEST_CASE("noise: no simplex edge lies near zero along its length") {
  // At a period of ONE a coordinate is its own lattice position, Q16.16,
  // so the samples fall on the edges exactly.
  u32 flat_2d = 0;
  for (i64 i = 0; i < 48; ++i)
    for (i64 j = 0; j < 48; ++j) {
      // (3 - sqrt(3)) / 6, Q16.16, as the noise unskews by.
      const auto node = [&](const i64 di, const i64 dj) {
        const i64 t = (i + di + j + dj) * 13849;
        return std::array<i64, 2>{((i + di) << 16) - t, ((j + dj) << 16) - t};
      };
      for (const auto &[di, dj] : {std::pair{1, 0}, {0, 1}, {1, 1}})
        flat_2d += flat_between(node(0, 0), node(di, dj), [](const auto &p) {
          return noise::simplex(SEED, p[0], p[1], noise::ONE);
        });
    }
  CHECK(flat_2d == 0u);
  u32 flat_3d = 0;
  for (i64 i = 0; i < 10; ++i)
    for (i64 j = 0; j < 10; ++j)
      for (i64 k = 0; k < 10; ++k) {
        const auto node = [&](const i64 di, const i64 dj, const i64 dk) {
          const i64 t = ((i + di + j + dj + k + dk) << 16) / 6;
          return std::array<i64, 3>{((i + di) << 16) - t, ((j + dj) << 16) - t,
                                    ((k + dk) << 16) - t};
        };
        for (i64 d = 1; d < 8; ++d)
          flat_3d += flat_between(
              node(0, 0, 0), node(d & 1, d >> 1 & 1, d >> 2 & 1),
              [](const auto &p) {
                return noise::simplex(SEED, p[0], p[1], p[2], noise::ONE);
              });
      }
  CHECK(flat_3d == 0u);
}

TEST_CASE("noise: another seed gives another field") {
  u32 same = 0;
  u32 total = 0;
  for (i32 y = 0; y < 64; y += 2)
    for (i32 x = 0; x < 64; x += 2) {
      same += noise::simplex(1, x, y, 8) == noise::simplex(2, x, y, 8);
      same += noise::value(1, x, y, 8) == noise::value(2, x, y, 8);
      total += 2;
    }
  CHECK(same < total / 20);
}

TEST_CASE("noise: cellular regions are about one a lattice cell, and keep "
          "their id where their point stays nearest") {
  constexpr i32 PERIOD = 16;
  nx::hash_set<u32> ids;
  for (i32 y = -128; y < 128; ++y)
    for (i32 x = -128; x < 128; ++x) {
      const noise::cell c = noise::cellular(SEED, x, y, PERIOD);
      REQUIRE(c.nearest <= c.second);
      CHECK(c.nearest <= 3 * noise::ONE / 2);
      ids.insert(c.id);
      // Well inside a region, a step does not leave it.
      if (c.second - c.nearest > noise::ONE / 4) {
        CHECK(noise::cellular(SEED, x + 1, y, PERIOD).id == c.id);
        CHECK(noise::cellular(SEED, x, y + 1, PERIOD).id == c.id);
      }
    }
  // 256 x 256 at 16 a cell is 256 cells.
  CHECK(ids.size() > 200u);
  CHECK(ids.size() < 320u);

  const noise::cell c = noise::cellular(SEED, 5, -9, 30, 8);
  CHECK(c.nearest <= c.second);
  CHECK(c.second < 2 * noise::ONE);
}

TEST_CASE("noise: octaves halve the period and scale by the gain") {
  nx::vector<i32> periods;
  const i32 one = noise::fbm(64, 1, noise::ONE / 2, [&](u32, i32 period) {
    periods.push_back(period);
    return noise::ONE / 4;
  });
  CHECK(one == noise::ONE / 4);
  periods.clear();
  // Every octave at ONE sums back to ONE whatever the gain.
  CHECK(noise::fbm(8, 5, noise::ONE / 2, [&](u32, i32 period) {
          periods.push_back(period);
          return noise::ONE;
        }) == noise::ONE);
  CHECK(periods == nx::vector<i32>{8, 4, 2, 1, 1});
  // The first octave alone weighs twice the second at half gain.
  const i32 mixed = noise::fbm(8, 2, noise::ONE / 2, [](u32 octave, i32) {
    return octave == 0 ? noise::ONE : -noise::ONE;
  });
  CHECK(mixed == noise::ONE / 3);
}
