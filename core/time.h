#pragma once

/**
 * @file time.h
 * @brief Monotonic time: timestamps, durations, and the stopwatch over them.
 *
 * Monotonic, never wall clock. A wall clock steps when the OS syncs it or the
 * user changes the date - fine for stamping a log line, useless for measuring
 * anything, and a source of negative intervals. Everything that measures
 * measures against this.
 */

#include "foundation/core/foundation.h"

#include <chrono>
#include <compare>

namespace nx {

/// A span of monotonic time, to the nanosecond.
///
/// Signed, so subtracting two timestamps the wrong way round reads as a
/// negative span rather than wrapping to three hundred years.
class duration {
public:
  constexpr duration() noexcept = default;

  [[nodiscard]] static constexpr duration from_nanos(const i64 n) noexcept {
    return duration(n);
  }
  [[nodiscard]] static constexpr duration from_micros(const i64 n) noexcept {
    return duration(n * 1'000);
  }
  [[nodiscard]] static constexpr duration from_millis(const i64 n) noexcept {
    return duration(n * 1'000'000);
  }
  [[nodiscard]] static constexpr duration from_seconds(const f64 s) noexcept {
    return duration(cast<i64>(s * 1e9));
  }

  // Truncating, like any integer division: 1999 ns is one microsecond.
  [[nodiscard]] constexpr i64 nanos() const noexcept { return ns_; }
  [[nodiscard]] constexpr i64 micros() const noexcept { return ns_ / 1'000; }
  [[nodiscard]] constexpr i64 millis() const noexcept {
    return ns_ / 1'000'000;
  }

  // Fractional, for anything comparing two measurements rather than reporting
  // one - truncating to a microsecond throws away the difference between two
  // fast runs, which is exactly what a crossover search is looking at.
  [[nodiscard]] constexpr f64 micros_f() const noexcept {
    return cast<f64>(ns_) / 1e3;
  }
  [[nodiscard]] constexpr f64 millis_f() const noexcept {
    return cast<f64>(ns_) / 1e6;
  }
  [[nodiscard]] constexpr f64 seconds() const noexcept {
    return cast<f64>(ns_) / 1e9;
  }

  constexpr duration &operator+=(const duration d) noexcept {
    ns_ += d.ns_;
    return *this;
  }
  constexpr duration &operator-=(const duration d) noexcept {
    ns_ -= d.ns_;
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
    return duration(-d.ns_);
  }

  [[nodiscard]] friend constexpr auto operator<=>(duration, duration) = default;
  [[nodiscard]] friend constexpr bool operator==(duration, duration) = default;

private:
  explicit constexpr duration(const i64 ns) noexcept : ns_(ns) {}

  i64 ns_ = 0;
};

/// A point on the monotonic clock.
///
/// Its zero is arbitrary - it is whenever the machine decided to start
/// counting - so only differences carry meaning. That is the whole trade a
/// monotonic clock makes, and why one cannot be printed as a date.
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
  [[nodiscard]] constexpr u64 nanos() const noexcept { return ns_; }
  [[nodiscard]] static constexpr timestamp from_nanos(const u64 ns) noexcept {
    return timestamp(ns);
  }

  [[nodiscard]] constexpr duration
  operator-(const timestamp rhs) const noexcept {
    return duration::from_nanos(cast<i64>(ns_) - cast<i64>(rhs.ns_));
  }
  [[nodiscard]] constexpr timestamp operator+(const duration d) const noexcept {
    return timestamp(cast<u64>(cast<i64>(ns_) + d.nanos()));
  }
  [[nodiscard]] constexpr timestamp operator-(const duration d) const noexcept {
    return timestamp(cast<u64>(cast<i64>(ns_) - d.nanos()));
  }

  [[nodiscard]] friend constexpr auto operator<=>(timestamp,
                                                  timestamp) = default;
  [[nodiscard]] friend constexpr bool operator==(timestamp,
                                                 timestamp) = default;

private:
  explicit constexpr timestamp(const u64 ns) noexcept : ns_(ns) {}

  u64 ns_ = 0;
};

/// Time since it was started, or since the last lap.
class stopwatch {
public:
  /// Starts immediately: a stopwatch that had to be started separately would
  /// mostly be started on the line after it was declared.
  stopwatch() noexcept : begin_(timestamp::now()) {}

  void restart() noexcept { begin_ = timestamp::now(); }

  [[nodiscard]] duration elapsed() const noexcept {
    return timestamp::now() - begin_;
  }

  /// Elapsed, and start again from that same instant. One clock read rather
  /// than two, so a run of laps neither loses nor double-counts the gap
  /// between them.
  [[nodiscard]] duration lap() noexcept {
    const timestamp now = timestamp::now();
    const duration d = now - begin_;
    begin_ = now;
    return d;
  }

  [[nodiscard]] timestamp started() const noexcept { return begin_; }

private:
  timestamp begin_;
};

/// Writes its own lifetime, in microseconds, into @p out.
///
/// The shape a per-stage timer wants: declare one at the top of the scope and
/// the number lands wherever it is reported from, with no way to leave out the
/// stop on an early return.
class scoped_micros {
public:
  explicit scoped_micros(u64 &out) noexcept : out_(out) {}
  ~scoped_micros() {
    const i64 us = watch_.elapsed().micros();
    out_ = us > 0 ? cast<u64>(us) : 0u;
  }

  scoped_micros(const scoped_micros &) = delete;
  scoped_micros &operator=(const scoped_micros &) = delete;

private:
  u64 &out_;
  stopwatch watch_;
};

} // namespace nx
