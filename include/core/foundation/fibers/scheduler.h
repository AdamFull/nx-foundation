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

enum class priority : u8 { High, Normal, Low };
inline constexpr usize PRIORITY_COUNT = 3;

/// Counts outstanding work. Jobs decrement it when they finish; waiting on it
/// suspends the waiting fiber instead of its thread. It must outlive every
/// decrement, which it does when the owner waits for zero before destroying it.
class counter {
public:
  explicit counter(u32 value = 0) noexcept : m_value(value) {}
  ~counter() noexcept;

  counter(const counter &) = delete;
  counter &operator=(const counter &) = delete;

  [[nodiscard]] u32 value() const noexcept {
    return m_value.load(std::memory_order_acquire);
  }
  void add(u32 n) noexcept { m_value.fetch_add(n, std::memory_order_relaxed); }

  /// Callable from any thread, including ones the scheduler does not own.
  void decrement() noexcept;

private:
  friend class scheduler;
  friend struct SchedulerState;
  friend void wait(counter &, u32) noexcept;

  std::atomic<u32> m_value;
  std::atomic<u32> m_decrementing{0};
  std::atomic<u32> m_waiters{0};
  spin_lock m_lock;
  Fiber *m_parked = nullptr;
};

/// Returns once counter.value() <= target. A fiber of any scheduler is
/// suspended; the main thread keeps running main-affine work; any other thread
/// blocks.
void wait(fiber::counter &counter, u32 target = 0) noexcept;

/// Which of the running fiber's pointers: User is the caller's own, and Pool
/// belongs to nx::thread_pool, so a task's own value cannot displace it.
enum class local_slot : u8 { User, Pool, Count };

/// A pointer that belongs to the running fiber and follows it across threads,
/// for what a thread_local cannot hold. Cleared when a fiber starts a job; on
/// a thread no scheduler owns it is per thread.
[[nodiscard]] void *fiber_local(local_slot slot = local_slot::User) noexcept;
void set_fiber_local(void *value, local_slot slot = local_slot::User) noexcept;

/// Whether the caller runs on a pooled fiber's stack rather than its thread's.
[[nodiscard]] bool in_fiber() noexcept;

/// Runs fn, which may block in the OS or a driver, on one of the scheduler's
/// blocking-service threads; the calling fiber moves there and back, so its
/// worker runs other jobs meanwhile. Outside a fiber, on one of those threads
/// or on a scheduler without any, fn runs in place. fn must not throw.
void run_blocking(function_ref<void()> fn) noexcept;

/// False while the caller holds an nx::mutex, nx::rw_lock or nx::spin_lock, is
/// inside scoped_no_alloc or a no_suspend_scope; suspending a fiber then
/// asserts.
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

struct job {
  void (*entry)(void *arg) = nullptr;
  void *arg = nullptr;
};

struct SchedulerState;

struct scheduler_stats {
  u32 workers = 0;
  u32 blocking_threads = 0;
  u32 fibers_created = 0;
  u32 fibers_free = 0;
  u32 fiber_limit_hits = 0;
  u32 live_jobs = 0;
  u64 jobs_run = 0;
};

/// Runs jobs on pooled fibers across worker threads. By default the
/// constructing thread becomes the main thread: it runs no general jobs, only
/// main-affine work, and only inside pump_main() or while it waits. It never
/// runs a fiber: that work runs on the thread's own stack. Without a
/// main thread, main-affine work is unavailable and the scheduler can be
/// created and destroyed on any thread.
class scheduler {
public:
  struct config {
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

  /// Deprecated: the names before foundation types were snake_case.
  using Config = config;
  using Stats = scheduler_stats;

  scheduler() : scheduler(config{}) {}
  explicit scheduler(const config &config);
  /// Runs every outstanding job to completion, then stops the workers. With
  /// a main thread, must be called on it.
  ~scheduler();

  scheduler(const scheduler &) = delete;
  scheduler &operator=(const scheduler &) = delete;

  /// Adds jobs.size() to `counter` and queues the jobs. A queue that is full
  /// makes the caller run the job itself.
  void run(std::span<const job> jobs, fiber::counter *counter = nullptr,
           fiber::priority priority = fiber::priority::Normal);
  void run_on_main(std::span<const job> jobs,
                   fiber::counter *counter = nullptr);

  void wait(fiber::counter &counter, u32 target = 0) {
    fiber::wait(counter, target);
  }

  void yield();

  /// Runs main-affine work until none is left and `budget` has passed.
  void pump_main(std::chrono::nanoseconds budget);

  [[nodiscard]] bool on_main_thread() const noexcept;
  [[nodiscard]] bool on_worker_thread() const noexcept;
  [[nodiscard]] bool in_fiber() const noexcept;
  [[nodiscard]] u32 worker_count() const noexcept;
  /// Writes the kernel thread ids (this_thread::os_id) of the workers that run
  /// jobs - not the main thread or blocking-service threads - and returns how
  /// many it wrote. A worker whose thread has not started yet is left out.
  [[nodiscard]] usize worker_os_ids(std::span<u64> out) const noexcept;
  [[nodiscard]] scheduler_stats stats() const noexcept;

private:
  SchedulerState *m_state = nullptr;
};

/// Deprecated: the names before foundation types were snake_case.
using Priority = priority;
using Counter = counter;
using LocalSlot = local_slot;
using Job = job;
using Scheduler = scheduler;

} // namespace nx::fiber
