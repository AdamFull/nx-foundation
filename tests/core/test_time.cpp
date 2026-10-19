
#include "framework/nxtest.h"
#include <limits>

#include "core/foundation/core/time.h"
#include "core/foundation/threading/thread.h"

#include <chrono>

TEST_CASE("duration: the factories and accessors agree") {
  CHECK(nx::duration::from_nanos(1500).nanos() == 1500);
  CHECK(nx::duration::from_micros(2).nanos() == 2000);
  CHECK(nx::duration::from_millis(3).nanos() == 3'000'000);
  CHECK(nx::duration::from_seconds(0.25).nanos() == 250'000'000);

  const nx::duration d = nx::duration::from_millis(1500);
  CHECK(d.micros() == 1'500'000);
  CHECK(d.millis() == 1500);
  CHECK(d.seconds() == nxtest::Approx(1.5));
}

TEST_CASE("duration: integer accessors truncate, fractional ones do not") {
  const nx::duration a = nx::duration::from_nanos(1600);
  const nx::duration b = nx::duration::from_nanos(1999);

  CHECK(a.micros() == 1);
  CHECK(b.micros() == 1);
  CHECK(a.micros_f() == nxtest::Approx(1.6));
  CHECK(b.micros_f() == nxtest::Approx(1.999));
  CHECK(a.micros_f() < b.micros_f());
}

TEST_CASE("duration: a default one is zero and durations are ordered") {
  constexpr nx::duration zero;
  CHECK(zero.nanos() == 0);
  CHECK((zero == nx::duration::from_nanos(0)));
  CHECK(nx::duration::from_micros(1) < nx::duration::from_micros(2));
  CHECK(nx::duration::from_millis(1) > nx::duration::from_micros(999));
}

TEST_CASE("duration: arithmetic is signed") {
  const nx::duration a = nx::duration::from_micros(5);
  const nx::duration b = nx::duration::from_micros(8);

  CHECK((a + b).micros() == 13);
  CHECK((a - b).micros() == -3);
  CHECK((a - b) < nx::duration{});
  CHECK((-a).micros() == -5);

  nx::duration acc;
  acc += a;
  acc += b;
  CHECK(acc.micros() == 13);
  acc -= b;
  CHECK(acc.micros() == 5);
}

TEST_CASE("time: constructed at compile time") {
  constexpr nx::duration frame = nx::duration::from_seconds(1.0 / 60.0);
  static_assert(frame.nanos() == 16'666'666);
  static_assert((frame + frame).millis() == 33);

  constexpr nx::timestamp origin = nx::timestamp::from_nanos(1000);
  static_assert((origin + nx::duration::from_micros(1)).nanos() == 2000);
  CHECK(frame.millis() == 16);
}

TEST_CASE("timestamp: differences carry the meaning, not the value") {
  const nx::timestamp a = nx::timestamp::from_nanos(5'000);
  const nx::timestamp b = nx::timestamp::from_nanos(9'000);

  CHECK((b - a).nanos() == 4'000);
  CHECK((a - b).nanos() == -4'000); // signed both ways round
  CHECK(a < b);
  CHECK((a + nx::duration::from_nanos(4'000)) == b);
  CHECK((b - nx::duration::from_nanos(4'000)) == a);
}

TEST_CASE("timestamp: the clock only moves forward") {
  const nx::timestamp first = nx::timestamp::now();
  nx::this_thread::sleep_for(std::chrono::milliseconds(2));
  const nx::timestamp second = nx::timestamp::now();

  CHECK(second > first);
  CHECK((second - first) > nx::duration{});
  CHECK((second - first).millis() >= 1);
}

TEST_CASE("stopwatch: starts on construction and accumulates") {
  const nx::stopwatch watch;
  const nx::duration first = watch.elapsed();
  nx::this_thread::sleep_for(std::chrono::milliseconds(2));
  const nx::duration second = watch.elapsed();

  CHECK(first >= nx::duration{});
  CHECK(second > first);
}

TEST_CASE("stopwatch: restart drops what came before") {
  nx::stopwatch watch;
  nx::this_thread::sleep_for(std::chrono::milliseconds(3));
  const nx::duration before = watch.elapsed();
  watch.restart();
  const nx::duration after = watch.elapsed();

  CHECK(before.millis() >= 2);
  CHECK(after < before);
}

TEST_CASE("stopwatch: laps tile the interval rather than overlapping it") {
  nx::stopwatch watch;
  const nx::timestamp begin = watch.started();

  nx::duration total;
  for (int i = 0; i < 3; ++i) {
    nx::this_thread::sleep_for(std::chrono::milliseconds(1));
    total += watch.lap();
  }
  const nx::timestamp end = watch.started();

  CHECK(total == (end - begin));
  CHECK(total.millis() >= 2);
}

TEST_CASE("scoped_micros: writes its own lifetime on the way out") {
  u64 elapsed = 0xFFFF;
  {
    const nx::scoped_micros timer(elapsed);
    CHECK(elapsed == 0xFFFF);
    nx::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(elapsed >= 1000u);
}

TEST_CASE("scoped_micros: an early return still reports") {
  u64 elapsed = 0;
  const auto measured = [&](const bool bail) {
    const nx::scoped_micros timer(elapsed);
    if (bail)
      return;
    nx::this_thread::sleep_for(std::chrono::milliseconds(1));
  };

  measured(true);
  const u64 quick = elapsed;
  measured(false);
  CHECK(elapsed > quick);
}

TEST_CASE("scoped_micros: a span under a microsecond reports zero, not wrapped") {
  u64 elapsed = 0xFFFF;
  { const nx::scoped_micros timer(elapsed); }
  CHECK(elapsed < 1000u);
}

TEST_CASE("duration: out-of-range factories saturate") {
  constexpr i64 MAX = std::numeric_limits<i64>::max();
  constexpr i64 MIN = std::numeric_limits<i64>::min();
  static_assert(nx::duration::from_seconds(1e300).nanos() == MAX);
  static_assert(nx::duration::from_seconds(-1e300).nanos() == MIN);
  static_assert(nx::duration::from_millis(MAX).nanos() == MAX);
  static_assert(nx::duration::from_micros(MIN).nanos() == MIN);
  static_assert(nx::duration::from_millis(-9'223'372'036'854).nanos() ==
                -9'223'372'036'854'000'000);

  CHECK(nx::duration::from_seconds(std::numeric_limits<f64>::infinity())
            .nanos() == MAX);
  CHECK(nx::duration::from_seconds(-std::numeric_limits<f64>::infinity())
            .nanos() == MIN);
  CHECK(nx::duration::from_seconds(std::numeric_limits<f64>::quiet_NaN())
            .nanos() == 0);
  CHECK(nx::duration::from_seconds(9.3e9).nanos() == MAX);
}
