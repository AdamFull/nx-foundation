#pragma once

#include "core/foundation/core/callable.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/threading/sync.h"

#include <atomic>
#include <exception>

namespace nx {

class cancellation_callback;

namespace detail {
struct cancellation_state {
  std::atomic<bool> requested{false};
  spin_lock lock;
  cancellation_callback *callbacks = nullptr;
  /// The callback request_stop() is running outside the lock, and the thread
  /// running it.
  cancellation_callback *running = nullptr;
  u64 running_thread = 0;

  void run_callbacks() noexcept;
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
  friend class cancellation_callback;

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

  /// Runs every registered cancellation_callback on the calling thread before
  /// returning.
  bool request_stop() noexcept {
    if (m_state == nullptr ||
        m_state->requested.exchange(true, std::memory_order_acq_rel))
      return false;
    m_state->run_callbacks();
    return true;
  }

  void reset() { m_state = nx::make_shared<detail::cancellation_state>(); }

private:
  nx::shared_ptr<detail::cancellation_state> m_state;
};

/// Runs @p fn once when @p token is cancelled: at once if it already is,
/// otherwise on the thread that calls request_stop(). Destroying it first
/// unregisters it, and waits if another thread is running it right then, so
/// what @p fn uses may be destroyed right after. It must not throw or block.
class cancellation_callback {
public:
  using Fn = nx::unique_function<void(), 32>;

  cancellation_callback(const cancellation_token &token, Fn fn) noexcept;
  ~cancellation_callback();

  cancellation_callback(const cancellation_callback &) = delete;
  cancellation_callback &operator=(const cancellation_callback &) = delete;

private:
  friend struct detail::cancellation_state;

  nx::shared_ptr<detail::cancellation_state> m_state;
  Fn m_fn;
  cancellation_callback *m_prev = nullptr;
  cancellation_callback *m_next = nullptr;
  bool m_linked = false;
};

} // namespace nx
