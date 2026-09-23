#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/threading/sync.h"

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

  std::atomic<u32> m_value;
  std::atomic<u32> m_decrementing{0};
  std::atomic<u32> m_waiters{0};
  spin_lock m_lock;
  Fiber *m_parked = nullptr;
};

struct Job {
  void (*entry)(void *arg) = nullptr;
  void *arg = nullptr;
};

struct SchedulerState;

/// Runs jobs on pooled fibers across worker threads. The constructing thread
/// is the main thread: it runs no general jobs, only main-affine work, and
/// only inside pump_main() or while it waits.
class Scheduler {
public:
  struct Config {
    u32 worker_count = 0;
    u32 max_fibers = 512;
    usize stack_size = 256 * 1024;
    usize stack_commit = 16 * 1024;
  };

  struct Stats {
    u32 workers = 0;
    u32 fibers_created = 0;
    u32 fibers_free = 0;
    u32 fiber_limit_hits = 0;
    u32 live_jobs = 0;
    u64 jobs_run = 0;
  };

  Scheduler() : Scheduler(Config{}) {}
  explicit Scheduler(const Config &config);
  /// Runs every outstanding job to completion, then stops the workers. Must
  /// be called on the main thread.
  ~Scheduler();

  Scheduler(const Scheduler &) = delete;
  Scheduler &operator=(const Scheduler &) = delete;

  /// Adds jobs.size() to `counter` and queues the jobs. A queue that is full
  /// makes the caller run the job itself.
  void run(std::span<const Job> jobs, Counter *counter = nullptr,
           Priority priority = Priority::Normal);
  void run_on_main(std::span<const Job> jobs, Counter *counter = nullptr);

  /// Returns once counter.value() <= target. A fiber is suspended; the main
  /// thread keeps running main-affine work; any other thread blocks.
  void wait(Counter &counter, u32 target = 0);

  void yield();
  void switch_to_main();
  void switch_to_workers();

  /// Runs main-affine work until none is left and `budget` has passed.
  void pump_main(std::chrono::nanoseconds budget);

  [[nodiscard]] bool on_main_thread() const noexcept;
  [[nodiscard]] bool in_fiber() const noexcept;
  [[nodiscard]] Stats stats() const noexcept;

private:
  SchedulerState *m_state = nullptr;
};

} // namespace nx::fiber
