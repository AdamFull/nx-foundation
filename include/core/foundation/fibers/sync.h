#pragma once

#include "core/foundation/fibers/wait.h"

#include <atomic>
#include <chrono>

// Synchronization that suspends the waiting fiber instead of its thread. On a
// thread no scheduler owns, the same objects block the thread. Unlike
// nx::mutex, these may be held across a fiber suspension.
namespace nx::fiber {

class mutex {
public:
  mutex() = default;
  mutex(const mutex &) = delete;
  mutex &operator=(const mutex &) = delete;

  void lock() noexcept {
    u32 expected = 0;
    if (!m_state.compare_exchange_strong(expected, LOCKED,
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed))
      lock_slow();
  }

  [[nodiscard]] bool try_lock() noexcept {
    u32 expected = 0;
    return m_state.compare_exchange_strong(
        expected, LOCKED, std::memory_order_acquire, std::memory_order_relaxed);
  }

  template <class Rep, class Period>
  [[nodiscard]] bool
  try_lock_for(std::chrono::duration<Rep, Period> timeout) noexcept {
    return try_lock() || lock_until(nx::detail::deadline_after(timeout));
  }
  template <class Clock, class Duration>
  [[nodiscard]] bool
  try_lock_until(std::chrono::time_point<Clock, Duration> t) noexcept {
    return try_lock() || lock_until(nx::detail::to_deadline(t));
  }

  ~mutex() {
    while (m_unlocking.load(std::memory_order_acquire) != 0)
      nx::detail::cpu_relax();
  }

  void unlock() noexcept {
    u32 expected = LOCKED;
    if (m_state.compare_exchange_strong(expected, 0, std::memory_order_release,
                                        std::memory_order_relaxed))
      return;
    // Waking reads the wait list after the release, when another thread may
    // already have locked, unlocked and destroyed the mutex; the destructor
    // waits this out.
    m_unlocking.fetch_add(1, std::memory_order_relaxed);
    m_state.store(0, std::memory_order_release);
    wake_one();
    m_unlocking.fetch_sub(1, std::memory_order_release);
  }

private:
  static constexpr u32 LOCKED = 1;
  static constexpr u32 CONTENDED = 2;

  void lock_slow() noexcept { (void)lock_until(internal::NO_DEADLINE); }
  bool lock_until(internal::clock::time_point deadline) noexcept;
  void wake_one() noexcept;

  std::atomic<u32> m_state{0};
  std::atomic<u32> m_unlocking{0};
  spin_lock m_lock;
  internal::WaitList m_waiters;
};

/// Writer-preferring: once a writer waits, new readers queue behind it.
/// Released ownership is handed straight to the next waiters.
class rw_lock {
public:
  rw_lock() = default;
  rw_lock(const rw_lock &) = delete;
  rw_lock &operator=(const rw_lock &) = delete;

  void lock() noexcept { (void)lock_until(internal::NO_DEADLINE); }
  [[nodiscard]] bool try_lock() noexcept;
  template <class Rep, class Period>
  [[nodiscard]] bool
  try_lock_for(std::chrono::duration<Rep, Period> timeout) noexcept {
    return lock_until(nx::detail::deadline_after(timeout));
  }
  template <class Clock, class Duration>
  [[nodiscard]] bool
  try_lock_until(std::chrono::time_point<Clock, Duration> t) noexcept {
    return lock_until(nx::detail::to_deadline(t));
  }
  void unlock() noexcept;

  void lock_shared() noexcept {
    (void)lock_shared_until(internal::NO_DEADLINE);
  }
  [[nodiscard]] bool try_lock_shared() noexcept;
  template <class Rep, class Period>
  [[nodiscard]] bool
  try_lock_shared_for(std::chrono::duration<Rep, Period> timeout) noexcept {
    return lock_shared_until(nx::detail::deadline_after(timeout));
  }
  template <class Clock, class Duration>
  [[nodiscard]] bool
  try_lock_shared_until(std::chrono::time_point<Clock, Duration> t) noexcept {
    return lock_shared_until(nx::detail::to_deadline(t));
  }
  void unlock_shared() noexcept;

private:
  bool lock_until(internal::clock::time_point deadline) noexcept;
  bool lock_shared_until(internal::clock::time_point deadline) noexcept;

  spin_lock m_lock;
  u32 m_readers = 0;
  bool m_writer = false;
  internal::WaitList m_waiting_readers;
  internal::WaitList m_waiting_writers;
};

class event {
public:
  explicit event(bool manual_reset = false, bool initially_set = false) noexcept
      : m_manual_reset(manual_reset), m_set(initially_set) {}
  event(const event &) = delete;
  event &operator=(const event &) = delete;

  /// A manual-reset event releases every waiter and stays set; an auto-reset
  /// one releases a single waiter, or stays set until one arrives.
  void set() noexcept;
  void reset() noexcept;
  [[nodiscard]] bool is_set() const noexcept {
    return m_set.load(std::memory_order_acquire);
  }

  void wait() noexcept { (void)wait_deadline(internal::NO_DEADLINE); }
  template <class Rep, class Period>
  [[nodiscard]] bool
  wait_for(std::chrono::duration<Rep, Period> timeout) noexcept {
    return wait_deadline(nx::detail::deadline_after(timeout));
  }
  template <class Clock, class Duration>
  [[nodiscard]] bool
  wait_until(std::chrono::time_point<Clock, Duration> t) noexcept {
    return wait_deadline(nx::detail::to_deadline(t));
  }

private:
  bool wait_deadline(internal::clock::time_point deadline) noexcept;

  const bool m_manual_reset;
  std::atomic<bool> m_set;
  spin_lock m_lock;
  internal::WaitList m_waiters;
};

class semaphore {
public:
  explicit semaphore(u32 initial = 0) noexcept : m_count(initial) {}
  semaphore(const semaphore &) = delete;
  semaphore &operator=(const semaphore &) = delete;

  void acquire() noexcept { (void)acquire_until(internal::NO_DEADLINE); }
  [[nodiscard]] bool try_acquire() noexcept;
  template <class Rep, class Period>
  [[nodiscard]] bool
  try_acquire_for(std::chrono::duration<Rep, Period> timeout) noexcept {
    return acquire_until(nx::detail::deadline_after(timeout));
  }
  template <class Clock, class Duration>
  [[nodiscard]] bool
  try_acquire_until(std::chrono::time_point<Clock, Duration> t) noexcept {
    return acquire_until(nx::detail::to_deadline(t));
  }
  void release(u32 n = 1) noexcept;

private:
  bool acquire_until(internal::clock::time_point deadline) noexcept;

  spin_lock m_lock;
  u32 m_count;
  internal::WaitList m_waiters;
};

class cond_var {
public:
  cond_var() = default;
  cond_var(const cond_var &) = delete;
  cond_var &operator=(const cond_var &) = delete;

  void notify_one() noexcept;
  void notify_all() noexcept;

  /// Waits under any lock the waiter holds, as nx::cond_var does. The timed
  /// waits return false when the time passed first.
  template <basic_lockable Lock> void wait(Lock &lock) noexcept {
    (void)wait_deadline(lock, internal::NO_DEADLINE);
  }
  template <basic_lockable Lock, typename Pred>
  void wait(Lock &lock, Pred pred) noexcept {
    while (!pred())
      wait(lock);
  }

  template <basic_lockable Lock, class Rep, class Period>
  bool wait_for(Lock &lock,
                std::chrono::duration<Rep, Period> timeout) noexcept {
    return wait_deadline(lock, nx::detail::deadline_after(timeout));
  }
  template <basic_lockable Lock, class Rep, class Period, typename Pred>
  bool wait_for(Lock &lock, std::chrono::duration<Rep, Period> timeout,
                Pred pred) noexcept {
    return wait_until(lock, nx::detail::deadline_after(timeout),
                      std::move(pred));
  }

  template <basic_lockable Lock, class Clock, class Duration>
  bool wait_until(Lock &lock,
                  std::chrono::time_point<Clock, Duration> t) noexcept {
    return wait_deadline(lock, nx::detail::to_deadline(t));
  }
  template <basic_lockable Lock, class Clock, class Duration, typename Pred>
  bool wait_until(Lock &lock, std::chrono::time_point<Clock, Duration> t,
                  Pred pred) noexcept {
    const auto deadline = nx::detail::to_deadline(t);
    while (!pred())
      if (!wait_deadline(lock, deadline))
        return pred();
    return true;
  }

private:
  template <typename Lock>
  bool wait_deadline(Lock &lock,
                     const internal::clock::time_point deadline) noexcept {
    if constexpr (requires { lock.owns_lock(); })
      NX_ASSERT(lock.owns_lock() &&
                "cond_var: waiting without holding the lock");
    m_lock.lock();
    lock.unlock();
    const internal::WaitResult result =
        internal::block(&m_lock, &m_waiters, deadline);
    lock.lock();
    return result == internal::WaitResult::Woken;
  }

  spin_lock m_lock;
  internal::WaitList m_waiters;
};

/// Reusable: once @p count parties have arrived, releases them all and starts
/// over.
class barrier {
public:
  explicit barrier(u32 count) noexcept : m_threshold(count) {
    NX_ASSERT(count > 0 && "barrier: count must be non-zero");
  }
  barrier(const barrier &) = delete;
  barrier &operator=(const barrier &) = delete;

  void arrive_and_wait() noexcept;
  /// Leaves the barrier: this phase and every later one wait for one fewer.
  /// For a participant that stops early, such as one that threw.
  void arrive_and_drop() noexcept;

private:
  u32 m_threshold;
  u32 m_arrived = 0;
  spin_lock m_lock;
  internal::WaitList m_waiters;
};

/// Suspends the calling fiber, or sleeps the calling thread.
template <class Rep, class Period>
void sleep_for(std::chrono::duration<Rep, Period> duration) noexcept {
  (void)internal::block(nullptr, nullptr, nx::detail::deadline_after(duration));
}

} // namespace nx::fiber
