#pragma once

/**
 * @file sync.h
 * @brief Futex-based synchronization primitives.
 *
 * Every primitive here is a thin wrapper over a 32-bit futex word (nx::futex
 * in os.h): 4-8 bytes of state, lock-free fast paths, and a kernel wait only
 * under contention. None allocate and none throw. Timed waits take chrono
 * durations; zero or negative means "do not block".
 */

#include "core/foundation/core/foundation.h"

#include <atomic>
#include <chrono>

namespace nx {

/**
 * @brief Raw 32-bit futex: the wait/wake primitive everything here builds on.
 *
 * wait() blocks while @p addr still holds @p expected_value; a zero timeout
 * means wait forever. WaitOnAddress on Windows, SYS_futex on Linux/Android.
 * Spurious wakeups are possible: always re-check the word in a loop.
 */
namespace futex {
enum class Result { Success = 0, TimedOut = 1, Error = 2 };

Result wait(std::atomic<u32> *addr, u32 expected_value,
            std::chrono::nanoseconds timeout);
i32 wake(std::atomic<u32> *addr, i32 count = 1);
i32 wake_all(std::atomic<u32> *addr);
} // namespace futex

namespace this_thread {
/// OS thread id of the calling thread (cached; defined in thread.cpp).
u64 id() noexcept;
} // namespace this_thread

namespace detail {

/// Iterations of cpu_relax() before falling back to a kernel wait.
inline constexpr i32 SYNC_SPIN_COUNT = 64;

/// nx::futex treats a zero timeout as "wait forever".
inline constexpr std::chrono::nanoseconds WAIT_FOREVER =
    std::chrono::nanoseconds::zero();

using sync_clock = std::chrono::steady_clock;

template <class Rep, class Period>
[[nodiscard]] NX_FORCE_INLINE std::chrono::nanoseconds
to_ns(std::chrono::duration<Rep, Period> d) noexcept {
  return std::chrono::ceil<std::chrono::nanoseconds>(d);
}

/// Waits on @p addr while it holds @p expected, up to @p deadline.
/// Returns false once the deadline has passed.
inline bool futex_wait_until(std::atomic<u32> *addr, u32 expected,
                             sync_clock::time_point deadline) noexcept {
  const auto now = sync_clock::now();
  if (now >= deadline)
    return false;
  futex::wait(addr, expected, to_ns(deadline - now));
  return true;
}
} // namespace detail

/// Minimal test-and-set spin lock. Never enters the kernel, so contended
/// waiters burn CPU: use only for critical sections of a few instructions
/// where a sleeping lock's wake latency would dominate.
class spin_lock {
public:
  spin_lock() = default;
  spin_lock(const spin_lock &) = delete;
  spin_lock &operator=(const spin_lock &) = delete;

  void lock() noexcept {
    for (;;) {
      if (!m_locked.exchange(1, std::memory_order_acquire))
        return;
      while (m_locked.load(std::memory_order_relaxed))
        detail::cpu_relax();
    }
  }

  [[nodiscard]] bool try_lock() noexcept {
    return !m_locked.exchange(1, std::memory_order_acquire);
  }

  void unlock() noexcept { m_locked.store(0, std::memory_order_release); }

private:
  std::atomic<u32> m_locked{0};
};

/// Fast futex-based mutex (4 bytes). Uncontended lock/unlock is a single CAS;
/// contended waiters spin briefly, then sleep. Not recursive and not fair -
/// no FIFO ordering under contention.
class mutex {
public:
  mutex() = default;
  mutex(const mutex &) = delete;
  mutex &operator=(const mutex &) = delete;

  void lock() noexcept {
    u32 expected = 0;
    if (NX_LIKELY(m_state.compare_exchange_strong(expected, LOCKED,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed)))
      return;
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
    if (try_lock())
      return true;
    if (timeout <= timeout.zero())
      return false;
    const auto deadline = detail::sync_clock::now() + detail::to_ns(timeout);
    // Once we announce ourselves with CONTENDED we may cause one spurious
    // unlock() wake after timing out; that is harmless.
    while (m_state.exchange(CONTENDED, std::memory_order_acquire) != 0) {
      if (!detail::futex_wait_until(&m_state, CONTENDED, deadline))
        return false;
    }
    return true;
  }

  void unlock() noexcept {
    if (m_state.exchange(0, std::memory_order_release) == CONTENDED)
      futex::wake(&m_state, 1);
  }

private:
  friend class cond_var;

  static constexpr u32 LOCKED = 1;    // held, no waiters
  static constexpr u32 CONTENDED = 2; // held, waiters may be sleeping

  NX_NO_INLINE void lock_slow() noexcept {
    // The owner usually releases within tens of cycles; spin before
    // paying for a kernel transition.
    for (i32 i = 0; i < detail::SYNC_SPIN_COUNT; ++i) {
      u32 s = m_state.load(std::memory_order_relaxed);
      if (s == 0 &&
          m_state.compare_exchange_weak(s, LOCKED, std::memory_order_acquire,
                                        std::memory_order_relaxed))
        return;
      detail::cpu_relax();
    }
    while (m_state.exchange(CONTENDED, std::memory_order_acquire) != 0)
      futex::wait(&m_state, CONTENDED, detail::WAIT_FOREVER);
  }

  std::atomic<u32> m_state{0};
};

/// Recursive mutex: nx::mutex plus an owner thread id. The owning thread may
/// re-lock any number of times; unlock() must be called once per lock().
class recursive_mutex {
public:
  recursive_mutex() = default;
  recursive_mutex(const recursive_mutex &) = delete;
  recursive_mutex &operator=(const recursive_mutex &) = delete;

  void lock() noexcept {
    const u64 tid = this_thread::id();
    if (m_owner.load(std::memory_order_relaxed) == tid) {
      ++m_depth;
      return;
    }
    m_inner.lock();
    m_owner.store(tid, std::memory_order_relaxed);
    m_depth = 1;
  }

  [[nodiscard]] bool try_lock() noexcept {
    const u64 tid = this_thread::id();
    if (m_owner.load(std::memory_order_relaxed) == tid) {
      ++m_depth;
      return true;
    }
    if (!m_inner.try_lock())
      return false;
    m_owner.store(tid, std::memory_order_relaxed);
    m_depth = 1;
    return true;
  }

  void unlock() noexcept {
    assert(m_owner.load(std::memory_order_relaxed) == this_thread::id() &&
           "recursive_mutex: unlock from non-owning thread");
    if (--m_depth == 0) {
      m_owner.store(0, std::memory_order_relaxed);
      m_inner.unlock();
    }
  }

private:
  mutex m_inner;
  std::atomic<u64> m_owner{0};
  u32 m_depth = 0;
};

/// Condition variable for nx::mutex. Subject to spurious wakeups: always wait
/// in a predicate loop, or use the predicate overloads. notify_* may be called
/// with or without the mutex held.
class cond_var {
public:
  cond_var() = default;
  cond_var(const cond_var &) = delete;
  cond_var &operator=(const cond_var &) = delete;

  void notify_one() noexcept {
    m_seq.fetch_add(1, std::memory_order_release);
    futex::wake(&m_seq, 1);
  }

  void notify_all() noexcept {
    m_seq.fetch_add(1, std::memory_order_release);
    futex::wake_all(&m_seq);
  }

  /// Atomically releases @p m and sleeps; re-acquires @p m before returning.
  void wait(mutex &m) noexcept {
    const u32 seq = m_seq.load(std::memory_order_acquire);
    m.unlock();
    futex::wait(&m_seq, seq, detail::WAIT_FOREVER);
    m.lock();
  }

  template <typename Pred> void wait(mutex &m, Pred pred) noexcept {
    while (!pred())
      wait(m);
  }

  /// @return false if the wait timed out. The mutex is re-acquired either way.
  template <class Rep, class Period>
  bool wait_for(mutex &m, std::chrono::duration<Rep, Period> timeout) noexcept {
    if (timeout <= timeout.zero())
      return false;
    const u32 seq = m_seq.load(std::memory_order_acquire);
    m.unlock();
    const futex::Result r = futex::wait(&m_seq, seq, detail::to_ns(timeout));
    m.lock();
    return r != futex::Result::TimedOut;
  }

  /// @return the predicate's value on exit (false only on timeout).
  template <class Rep, class Period, typename Pred>
  bool wait_for(mutex &m, std::chrono::duration<Rep, Period> timeout,
                Pred pred) noexcept {
    const auto deadline = detail::sync_clock::now() + detail::to_ns(timeout);
    while (!pred()) {
      const u32 seq = m_seq.load(std::memory_order_acquire);
      const auto now = detail::sync_clock::now();
      if (now >= deadline)
        return pred();
      m.unlock();
      futex::wait(&m_seq, seq, detail::to_ns(deadline - now));
      m.lock();
    }
    return true;
  }

private:
  std::atomic<u32> m_seq{0};
};

/// Writer-preferring reader/writer lock: any number of readers or one writer,
/// and once a writer waits, new readers block so it cannot starve. Not
/// recursive; readers must not upgrade in place.
class rw_lock {
public:
  rw_lock() = default;
  rw_lock(const rw_lock &) = delete;
  rw_lock &operator=(const rw_lock &) = delete;

  void lock_shared() noexcept {
    for (i32 spin = 0;;) {
      if (try_lock_shared())
        return;
      if (spin++ < detail::SYNC_SPIN_COUNT) {
        detail::cpu_relax();
        continue;
      }
      const u32 gate = m_reader_gate.load(std::memory_order_acquire);
      // Re-check after reading the gate: a release between the check and
      // the wait bumps the gate, so the wait below returns immediately.
      if (m_waiting_writers.load(std::memory_order_relaxed) == 0 &&
          !(m_state.load(std::memory_order_relaxed) & WRITER))
        continue;
      futex::wait(&m_reader_gate, gate, detail::WAIT_FOREVER);
      spin = 0;
    }
  }

  [[nodiscard]] bool try_lock_shared() noexcept {
    if (m_waiting_writers.load(std::memory_order_relaxed) != 0)
      return false;
    u32 s = m_state.load(std::memory_order_relaxed);
    while (!(s & WRITER)) {
      if (m_state.compare_exchange_weak(s, s + 1, std::memory_order_acquire,
                                        std::memory_order_relaxed))
        return true;
    }
    return false;
  }

  void unlock_shared() noexcept {
    // seq_cst pairs with the seq_cst m_waiting_writers increment in lock():
    // either we observe the waiting writer here, or it observes our
    // decrement before sleeping. Anything weaker can lose the wake.
    const u32 prev = m_state.fetch_sub(1, std::memory_order_seq_cst);
    if (prev == 1 && m_waiting_writers.load(std::memory_order_seq_cst) != 0) {
      m_writer_gate.fetch_add(1, std::memory_order_release);
      futex::wake(&m_writer_gate, 1);
    }
  }

  void lock() noexcept {
    m_waiting_writers.fetch_add(1, std::memory_order_seq_cst);
    for (i32 spin = 0;;) {
      u32 expected = 0;
      if (m_state.compare_exchange_weak(expected, WRITER,
                                        std::memory_order_acquire,
                                        std::memory_order_relaxed))
        break;
      if (spin++ < detail::SYNC_SPIN_COUNT) {
        detail::cpu_relax();
        continue;
      }
      const u32 gate = m_writer_gate.load(std::memory_order_acquire);
      if (m_state.load(std::memory_order_seq_cst) == 0)
        continue;
      futex::wait(&m_writer_gate, gate, detail::WAIT_FOREVER);
      spin = 0;
    }
    m_waiting_writers.fetch_sub(1, std::memory_order_relaxed);
  }

  [[nodiscard]] bool try_lock() noexcept {
    u32 expected = 0;
    return m_state.compare_exchange_strong(
        expected, WRITER, std::memory_order_acquire, std::memory_order_relaxed);
  }

  void unlock() noexcept {
    m_state.store(0, std::memory_order_seq_cst);
    if (m_waiting_writers.load(std::memory_order_seq_cst) != 0) {
      m_writer_gate.fetch_add(1, std::memory_order_release);
      futex::wake(&m_writer_gate, 1);
    } else {
      m_reader_gate.fetch_add(1, std::memory_order_release);
      futex::wake_all(&m_reader_gate);
    }
  }

private:
  static constexpr u32 WRITER = 1u << 31;

  std::atomic<u32> m_state{0}; // WRITER bit | reader count
  std::atomic<u32> m_reader_gate{0};
  std::atomic<u32> m_writer_gate{0};
  std::atomic<u32> m_waiting_writers{0};
};

/// Counting semaphore: acquire() decrements, sleeping while the count is zero;
/// release() increments and wakes waiters.
class semaphore {
public:
  explicit semaphore(u32 initial = 0) noexcept : m_count(initial) {}
  semaphore(const semaphore &) = delete;
  semaphore &operator=(const semaphore &) = delete;

  void acquire() noexcept {
    for (i32 spin = 0;;) {
      if (try_acquire())
        return;
      if (spin++ < detail::SYNC_SPIN_COUNT) {
        detail::cpu_relax();
        continue;
      }
      futex::wait(&m_count, 0, detail::WAIT_FOREVER);
      spin = 0;
    }
  }

  [[nodiscard]] bool try_acquire() noexcept {
    u32 c = m_count.load(std::memory_order_relaxed);
    while (c != 0) {
      if (m_count.compare_exchange_weak(c, c - 1, std::memory_order_acquire,
                                        std::memory_order_relaxed))
        return true;
    }
    return false;
  }

  template <class Rep, class Period>
  [[nodiscard]] bool
  try_acquire_for(std::chrono::duration<Rep, Period> timeout) noexcept {
    if (try_acquire())
      return true;
    if (timeout <= timeout.zero())
      return false;
    const auto deadline = detail::sync_clock::now() + detail::to_ns(timeout);
    for (;;) {
      if (try_acquire())
        return true;
      if (!detail::futex_wait_until(&m_count, 0, deadline))
        return try_acquire();
    }
  }

  void release(u32 n = 1) noexcept {
    m_count.fetch_add(n, std::memory_order_release);
    if (n == 1)
      futex::wake(&m_count, 1);
    else
      futex::wake_all(&m_count);
  }

private:
  std::atomic<u32> m_count;
};

/// Win32-style event: a boolean flag threads can wait on. Manual-reset, set()
/// releases every waiter until reset(); auto-reset, it releases exactly one
/// and the flag clears itself.
class event {
public:
  explicit event(bool manual_reset = false, bool initially_set = false) noexcept
      : m_state(initially_set ? 1u : 0u), m_manual_reset(manual_reset) {}
  event(const event &) = delete;
  event &operator=(const event &) = delete;

  void set() noexcept {
    m_state.store(1, std::memory_order_release);
    if (m_manual_reset)
      futex::wake_all(&m_state);
    else
      futex::wake(&m_state, 1);
  }

  void reset() noexcept { m_state.store(0, std::memory_order_relaxed); }

  [[nodiscard]] bool is_set() const noexcept {
    return m_state.load(std::memory_order_acquire) != 0;
  }

  void wait() noexcept {
    if (m_manual_reset) {
      while (m_state.load(std::memory_order_acquire) == 0)
        futex::wait(&m_state, 0, detail::WAIT_FOREVER);
    } else {
      for (;;) {
        u32 expected = 1;
        if (m_state.compare_exchange_weak(expected, 0,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed))
          return;
        futex::wait(&m_state, 0, detail::WAIT_FOREVER);
      }
    }
  }

  /// @return false if the wait timed out before the event was set.
  template <class Rep, class Period>
  [[nodiscard]] bool
  wait_for(std::chrono::duration<Rep, Period> timeout) noexcept {
    const auto deadline = detail::sync_clock::now() + detail::to_ns(timeout);
    for (;;) {
      if (m_manual_reset) {
        if (m_state.load(std::memory_order_acquire) != 0)
          return true;
      } else {
        u32 expected = 1;
        if (m_state.compare_exchange_weak(expected, 0,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed))
          return true;
      }
      if (!detail::futex_wait_until(&m_state, 0, deadline)) {
        if (m_manual_reset)
          return is_set();
        u32 expected = 1; // auto-reset must still consume the flag
        return m_state.compare_exchange_strong(
            expected, 0, std::memory_order_acquire, std::memory_order_relaxed);
      }
    }
  }

private:
  std::atomic<u32> m_state;
  const bool m_manual_reset;
};

/**
 * @brief Reusable thread barrier.
 *
 * arrive_and_wait() blocks until @p count threads have arrived, then releases
 * them and resets. Waiters sleep rather than spin: for the memory-bound phases
 * this backs, a spinning waiter contends with the still-working stragglers for
 * bandwidth and costs far more than the syscall it saves (measured).
 */
class barrier {
public:
  explicit barrier(u32 count) noexcept : m_threshold(count) {
    assert(count > 0 && "barrier: count must be non-zero");
  }
  barrier(const barrier &) = delete;
  barrier &operator=(const barrier &) = delete;

  void arrive_and_wait() noexcept {
    const u32 gen = m_generation.load(std::memory_order_acquire);
    if (m_arrived.fetch_add(1, std::memory_order_acq_rel) + 1 == m_threshold) {
      m_arrived.store(0, std::memory_order_relaxed);
      m_generation.fetch_add(1, std::memory_order_release);
      futex::wake_all(&m_generation);
      return;
    }
    while (m_generation.load(std::memory_order_acquire) == gen)
      futex::wait(&m_generation, gen, detail::WAIT_FOREVER);
  }

private:
  const u32 m_threshold;
  std::atomic<u32> m_arrived{0};
  std::atomic<u32> m_generation{0};
};

/**
 * @brief One-time initialization flag for nx::call_once.
 */
struct once_flag {
  once_flag() = default;
  once_flag(const once_flag &) = delete;
  once_flag &operator=(const once_flag &) = delete;

  std::atomic<u32> state{0}; // 0 = idle, 1 = running, 2 = done
};

/// Runs @p fn exactly once across every thread sharing the flag; concurrent
/// callers block until the winning call finishes. @p fn must not throw.
template <typename F, typename... Args>
void call_once(once_flag &flag, F &&fn, Args &&...args) {
  if (NX_LIKELY(flag.state.load(std::memory_order_acquire) == 2))
    return;
  u32 expected = 0;
  if (flag.state.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                         std::memory_order_acquire)) {
    std::forward<F>(fn)(std::forward<Args>(args)...);
    flag.state.store(2, std::memory_order_release);
    futex::wake_all(&flag.state);
    return;
  }
  u32 s;
  while ((s = flag.state.load(std::memory_order_acquire)) != 2)
    futex::wait(&flag.state, s, detail::WAIT_FOREVER);
}

/**
 * @brief RAII exclusive lock guard for any type with lock()/unlock().
 */
template <typename Mutex> class scoped_lock {
public:
  explicit scoped_lock(Mutex &m) noexcept : m_mutex(m) { m_mutex.lock(); }
  ~scoped_lock() { m_mutex.unlock(); }
  scoped_lock(const scoped_lock &) = delete;
  scoped_lock &operator=(const scoped_lock &) = delete;

private:
  Mutex &m_mutex;
};

/**
 * @brief RAII shared (reader) lock guard for nx::rw_lock-like types.
 */
template <typename Mutex> class scoped_shared_lock {
public:
  explicit scoped_shared_lock(Mutex &m) noexcept : m_mutex(m) {
    m_mutex.lock_shared();
  }
  ~scoped_shared_lock() { m_mutex.unlock_shared(); }
  scoped_shared_lock(const scoped_shared_lock &) = delete;
  scoped_shared_lock &operator=(const scoped_shared_lock &) = delete;

private:
  Mutex &m_mutex;
};

} // namespace nx
