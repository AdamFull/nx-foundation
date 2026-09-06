#pragma once

#include "core/foundation/core/foundation.h"

#include <atomic>
#include <exception>

namespace nx {

namespace detail {
struct cancellation_state {
  std::atomic<bool> requested{false};
};
} // namespace detail

/// Raised by cooperative jobs that stop because their cancellation token was
/// requested. Futures preserve cancellation separately from ordinary failure.
class task_cancelled final : public std::exception {
public:
  [[nodiscard]] const char *what() const noexcept override {
    return "task cancelled";
  }
};

/// Cheap, copyable view of a cancellation request. Cancellation is
/// cooperative: jobs should poll stop_requested() at safe interruption points
/// or call throw_if_requested().
class cancellation_token {
public:
  cancellation_token() = default;

  [[nodiscard]] bool stop_possible() const noexcept {
    return m_state != nullptr;
  }

  [[nodiscard]] bool stop_requested() const noexcept {
    return m_state != nullptr &&
           m_state->requested.load(std::memory_order_acquire);
  }

  void throw_if_requested() const {
    if (stop_requested())
      throw task_cancelled{};
  }

private:
  friend class cancellation_source;

  explicit cancellation_token(
      nx::shared_ptr<detail::cancellation_state> state) noexcept
      : m_state(std::move(state)) {}

  nx::shared_ptr<detail::cancellation_state> m_state;
};

/// Owns a cooperative cancellation request and creates tokens for jobs. reset()
/// starts an independent cancellation generation; existing tokens retain the
/// state of the generation from which they were created.
class cancellation_source {
public:
  cancellation_source()
      : m_state(nx::make_shared<detail::cancellation_state>()) {}

  [[nodiscard]] cancellation_token token() const noexcept {
    return cancellation_token(m_state);
  }

  [[nodiscard]] bool stop_requested() const noexcept {
    return m_state != nullptr &&
           m_state->requested.load(std::memory_order_acquire);
  }

  bool request_stop() noexcept {
    if (m_state == nullptr)
      return false;
    return !m_state->requested.exchange(true, std::memory_order_acq_rel);
  }

  void reset() { m_state = nx::make_shared<detail::cancellation_state>(); }

private:
  nx::shared_ptr<detail::cancellation_state> m_state;
};

} // namespace nx
