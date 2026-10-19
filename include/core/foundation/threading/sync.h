#pragma once

#include "core/foundation/core/foundation.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <utility>

namespace nx {

/// Raw 32-bit futex: the wait/wake primitive everything here builds on. wait()
/// blocks while @p addr still holds @p expected_value; a zero timeout means
/// wait forever.
namespace futex {
enum class Result { Success = 0, TimedOut = 1, Error = 2 };

Result wait(std::atomic<u32> *addr, u32 expected_value,
            std::chrono::nanoseconds timeout);
i32 wake(std::atomic<u32> *addr, i32 count = 1);
i32 wake_all(std::atomic<u32> *addr);
}

namespace this_thread {
u64 id() noexcept;
}

namespace detail {

inline constexpr i32 SYNC_SPIN_COUNT = 64;

inline constexpr std::chrono::nanoseconds WAIT_FOREVER =
    std::chrono::nanoseconds::zero();

using sync_clock = std::chrono::steady_clock;

#if NX_DEBUG
// Locks that block the OS thread; a fiber must not suspend while holding one.
extern thread_local i32 t_os_locks_held;
#endif

NX_FORCE_INLINE void note_os_lock(const i32 delta) noexcept {
#if NX_DEBUG
  t_os_locks_held += delta;
#else
  (void)delta;
#endif
}

/// Saturates: a duration past what nanoseconds hold, such as hours::max(),
/// becomes nanoseconds::max() instead of overflowing. A negative one becomes
/// zero.
template <class Rep, class Period>
[[nodiscard]] NX_FORCE_INLINE std::chrono::nanoseconds
to_ns(std::chrono::duration<Rep, Period> d) noexcept {
  using std::chrono::nanoseconds;
  if (d <= d.zero())
    return nanoseconds::zero();
  if (std::chrono::duration<double, std::nano>(d).count() >=
      static_cast<double>(nanoseconds::max().count()))
    return nanoseconds::max();
  return std::chrono::ceil<nanoseconds>(d);
}

/// now() + @p timeout, saturating at time_point::max().
template <class Rep, class Period>
[[nodiscard]] inline sync_clock::time_point
deadline_after(std::chrono::duration<Rep, Period> timeout) noexcept {
  const sync_clock::time_point now = sync_clock::now();
  const std::chrono::nanoseconds wait = to_ns(timeout);
  if (wait >= sync_clock::time_point::max() - now)
    return sync_clock::time_point::max();
  return now + std::chrono::ceil<sync_clock::duration>(wait);
}

inline bool futex_wait_until(std::atomic<u32> *addr, u32 expected,
                             sync_clock::time_point deadline) noexcept {
  const auto now = sync_clock::now();
  if (now >= deadline)
    return false;
  futex::wait(addr, expected, to_ns(deadline - now));
  return true;
}
}

class spin_lock {
public:
  spin_lock() = default;
  spin_lock(const spin_lock &) = delete;
  spin_lock &operator=(const spin_lock &) = delete;

  void lock() noexcept {
    for (;;) {
      if (!m_locked.exchange(1, std::memory_order_acquire)) {
        detail::note_os_lock(1);
        return;
      }
      while (m_locked.load(std::memory_order_relaxed))
        detail::cpu_relax();
    }
  }

  [[nodiscard]] bool try_lock() noexcept {
    if (m_locked.exchange(1, std::memory_order_acquire))
      return false;
    detail::note_os_lock(1);
    return true;
  }

  void unlock() noexcept {
    detail::note_os_lock(-1);
    m_locked.store(0, std::memory_order_release);
  }

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
    if (!NX_LIKELY(m_state.compare_exchange_strong(expected, LOCKED,
                                                   std::memory_order_acquire,
                                                   std::memory_order_relaxed)))
      lock_slow();
    detail::note_os_lock(1);
  }

  [[nodiscard]] bool try_lock() noexcept {
    u32 expected = 0;
    const bool locked = m_state.compare_exchange_strong(
        expected, LOCKED, std::memory_order_acquire, std::memory_order_relaxed);
    if (locked)
      detail::note_os_lock(1);
    return locked;
  }

  template <class Rep, class Period>
  [[nodiscard]] bool
  try_lock_for(std::chrono::duration<Rep, Period> timeout) noexcept {
    if (try_lock())
      return true;
    if (timeout <= timeout.zero())
      return false;
    const auto deadline = detail::deadline_after(timeout);
    while (m_state.exchange(CONTENDED, std::memory_order_acquire) != 0) {
      if (!detail::futex_wait_until(&m_state, CONTENDED, deadline))
        return false;
    }
    detail::note_os_lock(1);
    return true;
  }

  void unlock() noexcept {
    detail::note_os_lock(-1);
    if (m_state.exchange(0, std::memory_order_release) == CONTENDED)
      futex::wake(&m_state, 1);
  }

private:
  friend class cond_var;

  static constexpr u32 LOCKED = 1;
  static constexpr u32 CONTENDED = 2;

  NX_NO_INLINE void lock_slow() noexcept {
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

struct defer_lock_t {
  explicit defer_lock_t() = default;
};
struct try_to_lock_t {
  explicit try_to_lock_t() = default;
};
struct adopt_lock_t {
  explicit adopt_lock_t() = default;
};
inline constexpr defer_lock_t defer_lock{};
inline constexpr try_to_lock_t try_to_lock{};
inline constexpr adopt_lock_t adopt_lock{};

/// Movable exclusive ownership of any type with lock()/unlock(). Unlike
/// scoped_lock it may unlock before scope exit and lock again, and condition
/// variables wait on it.
template <typename Mutex> class unique_lock {
public:
  using mutex_type = Mutex;

  unique_lock() noexcept = default;
  explicit unique_lock(Mutex &m) noexcept : m_mutex(&m) {
    m.lock();
    m_owns = true;
  }
  unique_lock(Mutex &m, defer_lock_t) noexcept : m_mutex(&m) {}
  unique_lock(Mutex &m, try_to_lock_t) noexcept
      : m_mutex(&m), m_owns(m.try_lock()) {}
  unique_lock(Mutex &m, adopt_lock_t) noexcept : m_mutex(&m), m_owns(true) {}
  template <class Rep, class Period>
  unique_lock(Mutex &m, std::chrono::duration<Rep, Period> timeout) noexcept
      : m_mutex(&m), m_owns(m.try_lock_for(timeout)) {}

  ~unique_lock() {
    if (m_owns)
      m_mutex->unlock();
  }

  unique_lock(const unique_lock &) = delete;
  unique_lock &operator=(const unique_lock &) = delete;

  unique_lock(unique_lock &&other) noexcept
      : m_mutex(other.m_mutex), m_owns(other.m_owns) {
    other.m_mutex = nullptr;
    other.m_owns = false;
  }
  unique_lock &operator=(unique_lock &&other) noexcept {
    if (this != &other) {
      if (m_owns)
        m_mutex->unlock();
      m_mutex = other.m_mutex;
      m_owns = other.m_owns;
      other.m_mutex = nullptr;
      other.m_owns = false;
    }
    return *this;
  }

  void lock() noexcept {
    assert(m_mutex != nullptr && !m_owns && "unique_lock: lock() misuse");
    m_mutex->lock();
    m_owns = true;
  }
  [[nodiscard]] bool try_lock() noexcept {
    assert(m_mutex != nullptr && !m_owns && "unique_lock: try_lock() misuse");
    m_owns = m_mutex->try_lock();
    return m_owns;
  }
  template <class Rep, class Period>
  [[nodiscard]] bool
  try_lock_for(std::chrono::duration<Rep, Period> timeout) noexcept {
    assert(m_mutex != nullptr && !m_owns &&
           "unique_lock: try_lock_for() misuse");
    m_owns = m_mutex->try_lock_for(timeout);
    return m_owns;
  }
  void unlock() noexcept {
    assert(m_owns && "unique_lock: unlock() without ownership");
    m_mutex->unlock();
    m_owns = false;
  }

  /// Disassociates without unlocking; the caller now owns any lock held.
  [[nodiscard]] Mutex *release() noexcept {
    Mutex *const held = m_mutex;
    m_mutex = nullptr;
    m_owns = false;
    return held;
  }

  void swap(unique_lock &other) noexcept {
    std::swap(m_mutex, other.m_mutex);
    std::swap(m_owns, other.m_owns);
  }

  [[nodiscard]] bool owns_lock() const noexcept { return m_owns; }
  [[nodiscard]] explicit operator bool() const noexcept { return m_owns; }
  [[nodiscard]] Mutex *mutex() const noexcept { return m_mutex; }

private:
  Mutex *m_mutex = nullptr;
  bool m_owns = false;
};

/// unique_lock's shared counterpart, over lock_shared()/unlock_shared().
template <typename Mutex> class shared_lock {
public:
  using mutex_type = Mutex;

  shared_lock() noexcept = default;
  explicit shared_lock(Mutex &m) noexcept : m_mutex(&m) {
    m.lock_shared();
    m_owns = true;
  }
  shared_lock(Mutex &m, defer_lock_t) noexcept : m_mutex(&m) {}
  shared_lock(Mutex &m, try_to_lock_t) noexcept
      : m_mutex(&m), m_owns(m.try_lock_shared()) {}
  shared_lock(Mutex &m, adopt_lock_t) noexcept : m_mutex(&m), m_owns(true) {}

  ~shared_lock() {
    if (m_owns)
      m_mutex->unlock_shared();
  }

  shared_lock(const shared_lock &) = delete;
  shared_lock &operator=(const shared_lock &) = delete;

  shared_lock(shared_lock &&other) noexcept
      : m_mutex(other.m_mutex), m_owns(other.m_owns) {
    other.m_mutex = nullptr;
    other.m_owns = false;
  }
  shared_lock &operator=(shared_lock &&other) noexcept {
    if (this != &other) {
      if (m_owns)
        m_mutex->unlock_shared();
      m_mutex = other.m_mutex;
      m_owns = other.m_owns;
      other.m_mutex = nullptr;
      other.m_owns = false;
    }
    return *this;
  }

  void lock() noexcept {
    assert(m_mutex != nullptr && !m_owns && "shared_lock: lock() misuse");
    m_mutex->lock_shared();
    m_owns = true;
  }
  [[nodiscard]] bool try_lock() noexcept {
    assert(m_mutex != nullptr && !m_owns && "shared_lock: try_lock() misuse");
    m_owns = m_mutex->try_lock_shared();
    return m_owns;
  }
  void unlock() noexcept {
    assert(m_owns && "shared_lock: unlock() without ownership");
    m_mutex->unlock_shared();
    m_owns = false;
  }

  [[nodiscard]] Mutex *release() noexcept {
    Mutex *const held = m_mutex;
    m_mutex = nullptr;
    m_owns = false;
    return held;
  }

  void swap(shared_lock &other) noexcept {
    std::swap(m_mutex, other.m_mutex);
    std::swap(m_owns, other.m_owns);
  }

  [[nodiscard]] bool owns_lock() const noexcept { return m_owns; }
  [[nodiscard]] explicit operator bool() const noexcept { return m_owns; }
  [[nodiscard]] Mutex *mutex() const noexcept { return m_mutex; }

private:
  Mutex *m_mutex = nullptr;
  bool m_owns = false;
};

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

  void wait(unique_lock<mutex> &lock) noexcept { wait(owned(lock)); }
  template <typename Pred>
  void wait(unique_lock<mutex> &lock, Pred pred) noexcept {
    wait(owned(lock), std::move(pred));
  }
  template <class Rep, class Period>
  bool wait_for(unique_lock<mutex> &lock,
                std::chrono::duration<Rep, Period> timeout) noexcept {
    return wait_for(owned(lock), timeout);
  }
  template <class Rep, class Period, typename Pred>
  bool wait_for(unique_lock<mutex> &lock,
                std::chrono::duration<Rep, Period> timeout,
                Pred pred) noexcept {
    return wait_for(owned(lock), timeout, std::move(pred));
  }

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

  template <class Rep, class Period, typename Pred>
  bool wait_for(mutex &m, std::chrono::duration<Rep, Period> timeout,
                Pred pred) noexcept {
    const auto deadline = detail::deadline_after(timeout);
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
  [[nodiscard]] static mutex &owned(unique_lock<mutex> &lock) noexcept {
    assert(lock.owns_lock() && "cond_var: waiting without holding the lock");
    return *lock.mutex();
  }

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
      u32 s = m_state.load(std::memory_order_relaxed);
      if (!(s & WRITER) &&
          m_waiting_writers.load(std::memory_order_relaxed) == 0)
        continue;
      if (park(s))
        spin = 0;
    }
  }

  [[nodiscard]] bool try_lock_shared() noexcept {
    if (m_waiting_writers.load(std::memory_order_relaxed) != 0)
      return false;
    u32 s = m_state.load(std::memory_order_relaxed);
    while (!(s & WRITER)) {
      if (m_state.compare_exchange_weak(s, s + 1, std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
        detail::note_os_lock(1);
        return true;
      }
    }
    return false;
  }

  void unlock_shared() noexcept {
    detail::note_os_lock(-1);
    u32 s = m_state.load(std::memory_order_relaxed);
    for (;;) {
      const u32 next = (s & READERS) == 1 ? 0u : s - 1;
      if (m_state.compare_exchange_weak(s, next, std::memory_order_release,
                                        std::memory_order_relaxed)) {
        if (next == 0 && (s & PARKED))
          futex::wake_all(&m_state);
        return;
      }
    }
  }

  void lock() noexcept {
    m_waiting_writers.fetch_add(1, std::memory_order_seq_cst);
    for (i32 spin = 0;;) {
      u32 s = m_state.load(std::memory_order_relaxed);
      if ((s & ~PARKED) == 0 &&
          m_state.compare_exchange_weak(s, WRITER | (s & PARKED),
                                        std::memory_order_acquire,
                                        std::memory_order_relaxed))
        break;
      if (spin++ < detail::SYNC_SPIN_COUNT) {
        detail::cpu_relax();
        continue;
      }
      if ((s & ~PARKED) == 0)
        continue;
      if (park(s))
        spin = 0;
    }
    m_waiting_writers.fetch_sub(1, std::memory_order_relaxed);
    detail::note_os_lock(1);
  }

  [[nodiscard]] bool try_lock() noexcept {
    u32 s = m_state.load(std::memory_order_relaxed);
    const bool locked = (s & ~PARKED) == 0 &&
                        m_state.compare_exchange_strong(
                            s, WRITER | (s & PARKED), std::memory_order_acquire,
                            std::memory_order_relaxed);
    if (locked)
      detail::note_os_lock(1);
    return locked;
  }

  void unlock() noexcept {
    detail::note_os_lock(-1);
    if (m_state.exchange(0, std::memory_order_release) & PARKED)
      futex::wake_all(&m_state);
  }

private:
  // Releasing is one atomic operation on m_state followed by nothing but a
  // futex wake, so a waiter that takes the lock next may destroy it at once.
  static constexpr u32 WRITER = 1u << 31;
  static constexpr u32 PARKED = 1u << 30;
  static constexpr u32 READERS = PARKED - 1;

  // Sleeps until m_state moves on from @p seen, marking it parked first.
  // False when it changed before the mark landed.
  bool park(u32 seen) noexcept {
    if (!(seen & PARKED) && !m_state.compare_exchange_weak(
                                seen, seen | PARKED, std::memory_order_relaxed,
                                std::memory_order_relaxed))
      return false;
    futex::wait(&m_state, seen | PARKED, detail::WAIT_FOREVER);
    return true;
  }

  std::atomic<u32> m_state{0}; // WRITER | PARKED | reader count
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
    const auto deadline = detail::deadline_after(timeout);
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

class event {
public:
  explicit event(bool manual_reset = false, bool initially_set = false) noexcept
      : m_state(initially_set ? 1u : 0u), m_manual_reset(manual_reset) {}
  event(const event &) = delete;
  event &operator=(const event &) = delete;

  void set() noexcept {
    const bool wake_all = m_manual_reset;
    m_state.store(1, std::memory_order_release);
    if (wake_all)
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

  template <class Rep, class Period>
  [[nodiscard]] bool
  wait_for(std::chrono::duration<Rep, Period> timeout) noexcept {
    const auto deadline = detail::deadline_after(timeout);
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

/// Reusable thread barrier. arrive_and_wait() blocks until @p count threads have arrived, then
/// releases them and resets.
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

struct once_flag {
  once_flag() = default;
  once_flag(const once_flag &) = delete;
  once_flag &operator=(const once_flag &) = delete;

  std::atomic<u32> state{0};
};

/// Runs @p fn exactly once across every thread sharing the flag; concurrent
/// callers block until the winning call finishes. If @p fn throws, the flag
/// stays unset and the next caller runs it.
template <typename F, typename... Args>
void call_once(once_flag &flag, F &&fn, Args &&...args) {
  if (NX_LIKELY(flag.state.load(std::memory_order_acquire) == 2))
    return;
  for (;;) {
    u32 expected = 0;
    if (flag.state.compare_exchange_strong(expected, 1,
                                           std::memory_order_acquire,
                                           std::memory_order_acquire)) {
      try {
        std::forward<F>(fn)(std::forward<Args>(args)...);
      } catch (...) {
        flag.state.store(0, std::memory_order_release);
        futex::wake_all(&flag.state);
        throw;
      }
      flag.state.store(2, std::memory_order_release);
      futex::wake_all(&flag.state);
      return;
    }
    if (expected == 2)
      return;
    futex::wait(&flag.state, 1, detail::WAIT_FOREVER);
    if (flag.state.load(std::memory_order_acquire) == 2)
      return;
  }
}

/// RAII exclusive lock guard for any type with lock()/unlock().
template <typename Mutex> class scoped_lock {
public:
  explicit scoped_lock(Mutex &m) noexcept : m_mutex(m) { m_mutex.lock(); }
  scoped_lock(Mutex &m, adopt_lock_t) noexcept : m_mutex(m) {}
  ~scoped_lock() { m_mutex.unlock(); }
  scoped_lock(const scoped_lock &) = delete;
  scoped_lock &operator=(const scoped_lock &) = delete;

private:
  Mutex &m_mutex;
};

template <typename Mutex> class scoped_shared_lock {
public:
  explicit scoped_shared_lock(Mutex &m) noexcept : m_mutex(m) {
    m_mutex.lock_shared();
  }
  scoped_shared_lock(Mutex &m, adopt_lock_t) noexcept : m_mutex(m) {}
  ~scoped_shared_lock() { m_mutex.unlock_shared(); }
  scoped_shared_lock(const scoped_shared_lock &) = delete;
  scoped_shared_lock &operator=(const scoped_shared_lock &) = delete;

private:
  Mutex &m_mutex;
};

}
