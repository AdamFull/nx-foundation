#pragma once

/**
 * @file fixed.h
 * @brief Deterministic Q16.16 fixed-point arithmetic.
 */

#include "core/foundation/core/foundation.h"

#include <compare>

namespace nx {

struct fixed {
  static constexpr i32 FRAC = 16;
  static constexpr i32 ONE = 1 << FRAC;

  i32 raw = 0;

  constexpr fixed() noexcept = default;

  [[nodiscard]] static constexpr fixed from_raw(const i32 r) noexcept {
    fixed f;
    f.raw = r;
    return f;
  }
  [[nodiscard]] static constexpr fixed from_int(const i32 i) noexcept {
    return from_raw(i * ONE);
  }
  /// A constant from an exact rational, e.g. ratio(1, 60) for a 60 Hz step. The
  /// rounding is computed once here and is the same on every target.
  [[nodiscard]] static constexpr fixed ratio(const i32 num,
                                             const i32 den) noexcept {
    return from_raw(static_cast<i32>((static_cast<i64>(num) << FRAC) / den));
  }

  /// Truncates toward zero. floor() rounds down, which differs for negatives.
  [[nodiscard]] constexpr i32 to_int() const noexcept { return raw / ONE; }
  [[nodiscard]] constexpr i32 floor() const noexcept { return raw >> FRAC; }
  /// Rendering only - never feed the result back into the simulation.
  [[nodiscard]] constexpr f32 to_float() const noexcept {
    return static_cast<f32>(raw) / static_cast<f32>(ONE);
  }

  [[nodiscard]] constexpr fixed operator+(const fixed o) const noexcept {
    return from_raw(raw + o.raw);
  }
  [[nodiscard]] constexpr fixed operator-(const fixed o) const noexcept {
    return from_raw(raw - o.raw);
  }
  [[nodiscard]] constexpr fixed operator-() const noexcept {
    return from_raw(-raw);
  }
  [[nodiscard]] constexpr fixed operator*(const fixed o) const noexcept {
    return from_raw(
        static_cast<i32>((static_cast<i64>(raw) * o.raw) >> FRAC));
  }
  [[nodiscard]] constexpr fixed operator/(const fixed o) const noexcept {
    return from_raw(
        static_cast<i32>((static_cast<i64>(raw) << FRAC) / o.raw));
  }

  constexpr fixed &operator+=(const fixed o) noexcept {
    raw += o.raw;
    return *this;
  }
  constexpr fixed &operator-=(const fixed o) noexcept {
    raw -= o.raw;
    return *this;
  }
  constexpr fixed &operator*=(const fixed o) noexcept {
    return *this = *this * o;
  }

  [[nodiscard]] constexpr bool operator==(const fixed o) const noexcept {
    return raw == o.raw;
  }
  [[nodiscard]] constexpr std::strong_ordering
  operator<=>(const fixed o) const noexcept {
    return raw <=> o.raw;
  }
};

} // namespace nx
