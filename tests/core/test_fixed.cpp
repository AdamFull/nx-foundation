#include "framework/nxtest.h"

#include "core/foundation/core/fixed.h"

#include <limits>

using nx::fixed;

namespace {

constexpr i32 I32_MAX = std::numeric_limits<i32>::max();
constexpr i32 I32_MIN = std::numeric_limits<i32>::min();

}

TEST_CASE("fixed: arithmetic on representable values is exact") {
  CHECK(fixed::from_int(3).to_int() == 3);
  CHECK((fixed::from_int(3) + fixed::from_int(4)).to_int() == 7);
  CHECK((fixed::from_int(3) - fixed::from_int(4)).to_int() == -1);
  CHECK((fixed::from_int(6) * fixed::ratio(1, 2)).to_int() == 3);
  CHECK((fixed::from_int(7) / fixed::from_int(2)) == fixed::ratio(7, 2));
  CHECK((-fixed::from_int(5)).to_int() == -5);
}

TEST_CASE("fixed: overflow wraps the same way every time") {
  static_assert(fixed::from_int(40000).raw == -1673527296);
  static_assert((fixed::from_raw(I32_MAX) + fixed::from_raw(1)).raw == I32_MIN);
  static_assert((fixed::from_raw(I32_MIN) - fixed::from_raw(1)).raw == I32_MAX);
  static_assert((-fixed::from_raw(I32_MIN)).raw == I32_MIN);

  fixed sum = fixed::from_raw(I32_MAX);
  sum += fixed::from_raw(1);
  CHECK(sum.raw == I32_MIN);
  sum -= fixed::from_raw(1);
  CHECK(sum.raw == I32_MAX);
}

TEST_CASE("fixed: dividing by zero saturates by the sign of the dividend") {
  static_assert((fixed::from_int(3) / fixed{}).raw == I32_MAX);
  static_assert((fixed::from_int(-3) / fixed{}).raw == I32_MIN);
  static_assert((fixed{} / fixed{}).raw == 0);
  static_assert(fixed::ratio(1, 0).raw == I32_MAX);
  static_assert(fixed::ratio(-1, 0).raw == I32_MIN);

  const fixed zero{};
  CHECK((fixed::from_int(3) / zero).raw == I32_MAX);
  CHECK(fixed::ratio(0, 0).raw == 0);
}
