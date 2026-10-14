#pragma once

#include "core/foundation/core/foundation.h"

namespace nx {

struct fixed_step_config {
  f32 hz = 60.f;
  u32 max_steps = 4;
};

class fixed_step {
public:
  fixed_step() noexcept = default;

  explicit fixed_step(const fixed_step_config &config) noexcept
      : m_step(config.hz > 0.f ? 1.f / config.hz : 0.f),
        m_max_steps(max(config.max_steps, 1u)) {}

  [[nodiscard]] u32 advance(const f32 dt) noexcept {
    const u32 steps = peek(dt);

    if (dt > 0.f)
      m_accumulator = min(m_accumulator + dt, MAX_BACKLOG_SECONDS);
    for (u32 i = 0; i < steps; ++i)
      m_accumulator -= m_step;

    while (m_step > 0.f && m_accumulator >= m_step) {
      m_accumulator -= m_step;
      ++m_dropped;
    }
    return steps;
  }

  [[nodiscard]] u32 peek(const f32 dt) const noexcept {
    if (m_step <= 0.f)
      return 0;
    f32 accumulator = m_accumulator;
    if (dt > 0.f)
      accumulator = min(accumulator + dt, MAX_BACKLOG_SECONDS);

    const u32 limit = cap();
    u32 steps = 0;
    while (accumulator >= m_step && steps < limit) {
      accumulator -= m_step;
      ++steps;
    }
    return steps;
  }

  [[nodiscard]] f32 seconds() const noexcept { return m_step; }

  /// How long @p steps took to run, in real seconds. A step that costs more
  /// real time than it simulates cannot be caught up on: each extra one a
  /// frame runs makes the next frame longer and owe more. So the cap shrinks
  /// to the steps whose cost fits in one step's time, and past that the
  /// simulation runs slower than real time instead of the frame rate falling.
  void record_cost(const u32 steps, const f32 seconds) noexcept {
    if (steps == 0 || seconds <= 0.f)
      return;
    const f32 each = seconds / nx::cast<f32>(steps);
    m_cost = m_cost > 0.f ? m_cost + (each - m_cost) * COST_SMOOTHING : each;
  }

  /// Steps a frame may run now: max_steps until a cost is recorded, then as
  /// many as fit in one step's time, and never fewer than one.
  [[nodiscard]] u32 cap() const noexcept {
    if (m_cost <= 0.f || m_step <= 0.f)
      return m_max_steps;
    return clamp(nx::cast<u32>(m_step / m_cost), 1u, m_max_steps);
  }

  /// The smoothed real time one step takes, or 0 before any is recorded.
  [[nodiscard]] f32 step_cost() const noexcept { return m_cost; }

  [[nodiscard]] f32 alpha() const noexcept {
    return m_step > 0.f ? clamp(m_accumulator / m_step, 0.f, 1.f) : 0.f;
  }

  /// Steps the cap refused. Non-zero means simulated time is deliberately
  /// behind wall time.
  [[nodiscard]] u64 dropped() const noexcept { return m_dropped; }

private:
  static constexpr f32 MAX_BACKLOG_SECONDS = 1.f;
  static constexpr f32 COST_SMOOTHING = 0.2f;

  f32 m_step = 1.f / 60.f;
  f32 m_accumulator = 0.f;
  u32 m_max_steps = 4;
  f32 m_cost = 0.f;
  u64 m_dropped = 0;
};

}
