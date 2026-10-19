#pragma once

#include "core/foundation/core/foundation.h"

#include <compare>
#include <limits>

namespace nx {

/// Q16.16. Overflow wraps as i32 does in hardware, defined so every peer of a
/// rollback session computes the same bits; dividing by zero saturates by sign.
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
    return from_raw(static_cast<i32>(static_cast<i64>(i) * ONE));
  }
  [[nodiscard]] static constexpr fixed ratio(const i32 num,
                                             const i32 den) noexcept {
    if (den == 0)
      return over_zero(num);
    return from_raw(static_cast<i32>((static_cast<i64>(num) << FRAC) / den));
  }

  [[nodiscard]] constexpr i32 to_int() const noexcept { return raw / ONE; }
  [[nodiscard]] constexpr i32 floor() const noexcept { return raw >> FRAC; }
  [[nodiscard]] constexpr f32 to_float() const noexcept {
    return static_cast<f32>(raw) / static_cast<f32>(ONE);
  }

  [[nodiscard]] constexpr fixed operator+(const fixed o) const noexcept {
    return from_raw(static_cast<i32>(static_cast<i64>(raw) + o.raw));
  }
  [[nodiscard]] constexpr fixed operator-(const fixed o) const noexcept {
    return from_raw(static_cast<i32>(static_cast<i64>(raw) - o.raw));
  }
  [[nodiscard]] constexpr fixed operator-() const noexcept {
    return from_raw(static_cast<i32>(-static_cast<i64>(raw)));
  }
  [[nodiscard]] constexpr fixed operator*(const fixed o) const noexcept {
    return from_raw(
        static_cast<i32>((static_cast<i64>(raw) * o.raw) >> FRAC));
  }
  [[nodiscard]] constexpr fixed operator/(const fixed o) const noexcept {
    if (o.raw == 0)
      return over_zero(raw);
    return from_raw(static_cast<i32>((static_cast<i64>(raw) << FRAC) / o.raw));
  }

  constexpr fixed &operator+=(const fixed o) noexcept {
    return *this = *this + o;
  }
  constexpr fixed &operator-=(const fixed o) noexcept {
    return *this = *this - o;
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

private:
  [[nodiscard]] static constexpr fixed over_zero(const i32 sign) noexcept {
    if (sign > 0)
      return from_raw(std::numeric_limits<i32>::max());
    if (sign < 0)
      return from_raw(std::numeric_limits<i32>::min());
    return {};
  }
};

}
