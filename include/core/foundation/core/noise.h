#pragma once

#include "core/foundation/core/foundation.h"

#include <utility>

/// Coherent noise in integer arithmetic alone, so a seed and a coordinate
/// give the same bits on every CPU and compiler: world generation run on a
/// server and on each client agrees.
///
/// Coordinates are integers (a world's blocks), sampled on a lattice
/// @p period units apart (at least 1); results are Q16.16, ONE meaning 1.0.
namespace nx::noise {

inline constexpr i32 ONE = 1 << 16;

/// A bijection of 32 bits that spreads every input bit over the output.
[[nodiscard]] constexpr u32 mix(u32 h) noexcept {
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return h;
}

/// A well mixed hash of @p seed and the coordinates.
[[nodiscard]] constexpr u32 hash(const u32 seed, const i32 x) noexcept {
  return mix(mix(seed + 0x9e3779b9u) ^ static_cast<u32>(x));
}

[[nodiscard]] constexpr u32 hash(const u32 seed, const i32 x,
                                 const i32 y) noexcept {
  return mix(hash(seed, x) ^ static_cast<u32>(y));
}

[[nodiscard]] constexpr u32 hash(const u32 seed, const i32 x, const i32 y,
                                 const i32 z) noexcept {
  return mix(hash(seed, x, y) ^ static_cast<u32>(z));
}

/// Values at the lattice points, eased between: in [-ONE, ONE].
[[nodiscard]] i32 value(u32 seed, i32 x, i32 y, i32 period) noexcept;
[[nodiscard]] i32 value(u32 seed, i32 x, i32 y, i32 z, i32 period) noexcept;

/// Simplex gradient noise: in [-ONE, ONE].
[[nodiscard]] i32 simplex(u32 seed, i32 x, i32 y, i32 period) noexcept;
[[nodiscard]] i32 simplex(u32 seed, i32 x, i32 y, i32 z, i32 period) noexcept;

/// The nearest and second-nearest of one random point in each lattice cell
/// (Worley noise). Distances are in lattice units, Q16.16; `id` names the
/// nearest point's cell, the same everywhere that point is nearest, as a
/// biome or a region needs.
struct cell {
  i32 nearest = 0;
  i32 second = 0;
  u32 id = 0;
};

[[nodiscard]] cell cellular(u32 seed, i32 x, i32 y, i32 period) noexcept;
[[nodiscard]] cell cellular(u32 seed, i32 x, i32 y, i32 z, i32 period) noexcept;

/// @p octaves of @p sample summed, each at half the period of the last and
/// @p gain (Q16.16) times its amplitude, scaled back into [-ONE, ONE].
/// @p sample(octave, period) gives one octave; it should vary its seed by
/// octave. A period is not halved below 1.
template <typename F>
[[nodiscard]] i32 fbm(const i32 period, const u32 octaves, const i32 gain,
                      F &&sample) {
  i64 sum = 0;
  i64 total = 0;
  i64 amplitude = ONE;
  i32 at = period;
  for (u32 octave = 0; octave < octaves; ++octave) {
    sum += (static_cast<i64>(sample(octave, at)) * amplitude) >> 16;
    total += amplitude;
    amplitude = (amplitude * gain) >> 16;
    at = at > 1 ? at / 2 : 1;
  }
  if (total == 0)
    return 0;
  return static_cast<i32>((sum << 16) / total);
}

} // namespace nx::noise
