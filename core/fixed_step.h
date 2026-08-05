#pragma once

/**
 * @file fixed_step.h
 * @brief Dividing frame time into whole simulation steps.
 */

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

    u32 steps = 0;
    while (accumulator >= m_step && steps < m_max_steps) {
      accumulator -= m_step;
      ++steps;
    }
    return steps;
  }

  [[nodiscard]] f32 seconds() const noexcept { return m_step; }

  /// Where a render frame sits between the last step and the next, in [0, 1).
  [[nodiscard]] f32 alpha() const noexcept {
    return m_step > 0.f ? clamp(m_accumulator / m_step, 0.f, 1.f) : 0.f;
  }

  /// Steps the cap refused. Non-zero means simulated time is deliberately
  /// behind wall time.
  [[nodiscard]] u64 dropped() const noexcept { return m_dropped; }

private:
  static constexpr f32 MAX_BACKLOG_SECONDS = 1.f;

  f32 m_step = 1.f / 60.f;
  f32 m_accumulator = 0.f;
  u32 m_max_steps = 4;
  u64 m_dropped = 0;
};

} // namespace nx
