#pragma once

#include "core/foundation/core/foundation.h"

#include <chrono>
#include <compare>
#include <limits>

namespace nx {

/// A span of monotonic time, to the nanosecond. Signed, so subtracting two
/// timestamps the wrong way round reads as a negative span rather than wrapping
/// to three hundred years. The factories saturate at about 292 years either
/// way; NaN seconds is zero.
class duration {
public:
  constexpr duration() noexcept = default;

  [[nodiscard]] static constexpr duration from_nanos(const i64 n) noexcept {
    return duration(n);
  }
  [[nodiscard]] static constexpr duration from_micros(const i64 n) noexcept {
    return scaled(n, 1'000);
  }
  [[nodiscard]] static constexpr duration from_millis(const i64 n) noexcept {
    return scaled(n, 1'000'000);
  }
  [[nodiscard]] static constexpr duration from_seconds(const f64 s) noexcept {
    const f64 ns = s * 1e9;
    if (ns != ns)
      return {};
    if (ns >= 0x1p63)
      return duration(std::numeric_limits<i64>::max());
    if (ns <= -0x1p63)
      return duration(std::numeric_limits<i64>::min());
    return duration(cast<i64>(ns));
  }

  [[nodiscard]] constexpr i64 nanos() const noexcept { return m_ns; }
  [[nodiscard]] constexpr i64 micros() const noexcept { return m_ns / 1'000; }
  [[nodiscard]] constexpr i64 millis() const noexcept {
    return m_ns / 1'000'000;
  }

  [[nodiscard]] constexpr f64 micros_f() const noexcept {
    return cast<f64>(m_ns) / 1e3;
  }
  [[nodiscard]] constexpr f64 millis_f() const noexcept {
    return cast<f64>(m_ns) / 1e6;
  }
  [[nodiscard]] constexpr f64 seconds() const noexcept {
    return cast<f64>(m_ns) / 1e9;
  }

  constexpr duration &operator+=(const duration d) noexcept {
    m_ns += d.m_ns;
    return *this;
  }
  constexpr duration &operator-=(const duration d) noexcept {
    m_ns -= d.m_ns;
    return *this;
  }

  [[nodiscard]] friend constexpr duration operator+(duration a,
                                                    const duration b) noexcept {
    return a += b;
  }
  [[nodiscard]] friend constexpr duration operator-(duration a,
                                                    const duration b) noexcept {
    return a -= b;
  }
  [[nodiscard]] friend constexpr duration operator-(const duration d) noexcept {
    return duration(-d.m_ns);
  }

  [[nodiscard]] friend constexpr auto operator<=>(duration, duration) = default;
  [[nodiscard]] friend constexpr bool operator==(duration, duration) = default;

private:
  explicit constexpr duration(const i64 ns) noexcept : m_ns(ns) {}

  [[nodiscard]] static constexpr duration scaled(const i64 n,
                                                 const i64 unit) noexcept {
    constexpr i64 MAX = std::numeric_limits<i64>::max();
    constexpr i64 MIN = std::numeric_limits<i64>::min();
    if (n > MAX / unit)
      return duration(MAX);
    if (n < MIN / unit)
      return duration(MIN);
    return duration(n * unit);
  }

  i64 m_ns = 0;
};

/// A point on the monotonic clock. Its zero is arbitrary - it is whenever the
/// machine decided to start counting - so only differences carry meaning. That
/// is the whole trade a monotonic clock makes, and why one cannot be printed as
/// a date.
class timestamp {
public:
  constexpr timestamp() noexcept = default;

  [[nodiscard]] static timestamp now() noexcept {
    return timestamp(
        cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count()));
  }

  /// Nanoseconds since the clock's own zero. For handing a stamp to something
  /// that stores or transmits it; the value is only comparable within one run.
  [[nodiscard]] constexpr u64 nanos() const noexcept { return m_ns; }
  [[nodiscard]] static constexpr timestamp from_nanos(const u64 ns) noexcept {
    return timestamp(ns);
  }

  [[nodiscard]] constexpr duration
  operator-(const timestamp rhs) const noexcept {
    return duration::from_nanos(cast<i64>(m_ns) - cast<i64>(rhs.m_ns));
  }
  [[nodiscard]] constexpr timestamp operator+(const duration d) const noexcept {
    return timestamp(cast<u64>(cast<i64>(m_ns) + d.nanos()));
  }
  [[nodiscard]] constexpr timestamp operator-(const duration d) const noexcept {
    return timestamp(cast<u64>(cast<i64>(m_ns) - d.nanos()));
  }

  [[nodiscard]] friend constexpr auto operator<=>(timestamp,
                                                  timestamp) = default;
  [[nodiscard]] friend constexpr bool operator==(timestamp,
                                                 timestamp) = default;

private:
  explicit constexpr timestamp(const u64 ns) noexcept : m_ns(ns) {}

  u64 m_ns = 0;
};

class stopwatch {
public:
  stopwatch() noexcept : m_begin(timestamp::now()) {}

  void restart() noexcept { m_begin = timestamp::now(); }

  [[nodiscard]] duration elapsed() const noexcept {
    return timestamp::now() - m_begin;
  }

  [[nodiscard]] duration lap() noexcept {
    const timestamp now = timestamp::now();
    const duration d = now - m_begin;
    m_begin = now;
    return d;
  }

  [[nodiscard]] timestamp started() const noexcept { return m_begin; }

private:
  timestamp m_begin;
};

/// Writes its own lifetime, in microseconds, into @p out. The shape a per-stage
/// timer wants: declare one at the top of the scope and the number lands
/// wherever it is reported from, with no way to leave out the stop on an early
/// return.
class scoped_micros {
public:
  explicit scoped_micros(u64 &out) noexcept : m_out(out) {}
  ~scoped_micros() {
    const i64 us = m_watch.elapsed().micros();
    m_out = us > 0 ? cast<u64>(us) : 0u;
  }

  scoped_micros(const scoped_micros &) = delete;
  scoped_micros &operator=(const scoped_micros &) = delete;

private:
  u64 &m_out;
  stopwatch m_watch;
};

}
