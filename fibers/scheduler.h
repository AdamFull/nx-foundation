#pragma once

#include "core/foundation/core/callable.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/threading/sync.h"
#include "core/foundation/threading/thread.h"

#include <atomic>
#include <chrono>
#include <span>

namespace nx::fiber {

struct Fiber;

enum class Priority : u8 { High, Normal, Low };
inline constexpr usize PRIORITY_COUNT = 3;

/// Counts outstanding work. Jobs decrement it when they finish; waiting on it
/// suspends the waiting fiber instead of its thread. It must outlive every
/// decrement, which it does when the owner waits for zero before destroying it.
class Counter {
public:
  explicit Counter(u32 value = 0) noexcept : m_value(value) {}
  ~Counter() noexcept;

  Counter(const Counter &) = delete;
  Counter &operator=(const Counter &) = delete;

  [[nodiscard]] u32 value() const noexcept {
    return m_value.load(std::memory_order_acquire);
  }
  void add(u32 n) noexcept { m_value.fetch_add(n, std::memory_order_relaxed); }

  /// Callable from any thread, including ones the scheduler does not own.
  void decrement() noexcept;

private:
  friend class Scheduler;
  friend struct SchedulerState;
  friend void wait(Counter &, u32) noexcept;

  std::atomic<u32> m_value;
  std::atomic<u32> m_decrementing{0};
  std::atomic<u32> m_waiters{0};
  spin_lock m_lock;
  Fiber *m_parked = nullptr;
};

/// Returns once counter.value() <= target. A fiber of any scheduler is
/// suspended; the main thread keeps running main-affine work; any other thread
/// blocks.
void wait(Counter &counter, u32 target = 0) noexcept;

/// A pointer that belongs to the running fiber and follows it across threads,
/// for what a thread_local cannot hold. Cleared when a fiber starts a job; on
/// a thread no scheduler owns it is per thread.
[[nodiscard]] void *fiber_local() noexcept;
void set_fiber_local(void *value) noexcept;

/// Runs fn, which may block in the OS or a driver, on one of the scheduler's
/// blocking-service threads; the calling fiber moves there and back, so its
/// worker runs other jobs meanwhile. Outside a fiber, on one of those threads
/// or on a scheduler without any, fn runs in place. fn must not throw.
void run_blocking(function_ref<void()> fn) noexcept;

/// False while the caller holds an nx::mutex or nx::rw_lock, is inside
/// scoped_no_alloc or a no_suspend_scope; suspending a fiber then asserts.
/// Always true in release builds.
[[nodiscard]] bool may_suspend() noexcept;

/// Marks code that holds a thread-affine resource, such as a per-thread
/// command pool, so a fiber cannot suspend and migrate while it is held.
class no_suspend_scope {
public:
  no_suspend_scope() noexcept { nx::detail::note_os_lock(1); }
  ~no_suspend_scope() noexcept { nx::detail::note_os_lock(-1); }
  no_suspend_scope(const no_suspend_scope &) = delete;
  no_suspend_scope &operator=(const no_suspend_scope &) = delete;
};

struct Job {
  void (*entry)(void *arg) = nullptr;
  void *arg = nullptr;
};

struct SchedulerState;

/// Runs jobs on pooled fibers across worker threads. By default the
/// constructing thread becomes the main thread: it runs no general jobs, only
/// main-affine work, and only inside pump_main() or while it waits. Without a
/// main thread, main-affine work is unavailable and the scheduler can be
/// created and destroyed on any thread.
class Scheduler {
public:
  struct Config {
    u32 worker_count = 0;
    u32 max_fibers = 512;
    usize stack_size = 256 * 1024;
    usize stack_commit = 16 * 1024;
    const char *name = "nx-fiber";
    thread_priority priority = thread_priority::Normal;
    bool adopt_main_thread = true;
    /// Threads that run only run_blocking calls, on top of the workers.
    u32 blocking_threads = 0;
  };

  struct Stats {
    u32 workers = 0;
    u32 blocking_threads = 0;
    u32 fibers_created = 0;
    u32 fibers_free = 0;
    u32 fiber_limit_hits = 0;
    u32 live_jobs = 0;
    u64 jobs_run = 0;
  };

  Scheduler() : Scheduler(Config{}) {}
  explicit Scheduler(const Config &config);
  /// Runs every outstanding job to completion, then stops the workers. With
  /// a main thread, must be called on it.
  ~Scheduler();

  Scheduler(const Scheduler &) = delete;
  Scheduler &operator=(const Scheduler &) = delete;

  /// Adds jobs.size() to `counter` and queues the jobs. A queue that is full
  /// makes the caller run the job itself.
  void run(std::span<const Job> jobs, Counter *counter = nullptr,
           Priority priority = Priority::Normal);
  void run_on_main(std::span<const Job> jobs, Counter *counter = nullptr);

  void wait(Counter &counter, u32 target = 0) { fiber::wait(counter, target); }

  void yield();
  void switch_to_main();
  void switch_to_workers();

  /// Runs main-affine work until none is left and `budget` has passed.
  void pump_main(std::chrono::nanoseconds budget);

  [[nodiscard]] bool on_main_thread() const noexcept;
  [[nodiscard]] bool on_worker_thread() const noexcept;
  [[nodiscard]] bool in_fiber() const noexcept;
  [[nodiscard]] u32 worker_count() const noexcept;
  [[nodiscard]] Stats stats() const noexcept;

private:
  SchedulerState *m_state = nullptr;
};

} // namespace nx::fiber
