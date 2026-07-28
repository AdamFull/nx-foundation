#pragma once

/**
 * @file thread_pool.h
 * @brief Work-stealing thread pool: nx::thread_pool, nx::task_group,
 * nx::future.
 *
 * A fixed set of worker threads executes submitted callables. Each worker
 * owns a bounded lock-free queue and steals from its siblings when idle;
 * external submissions go to a shared queue with an unbounded mutex-guarded
 * overflow, so submit() never fails and never blocks. Idle workers sleep on
 * a futex.
 *
 * Waiting is cooperative: wait_idle(), task_group::wait() and future::get()
 * all execute queued tasks while they wait (from any thread), so a
 * parallel_for caller contributes instead of blocking, and waiting inside a
 * task cannot deadlock the pool.
 *
 * Tasks must not throw. Callables are type-erased through nx::function and
 * therefore must be copy-constructible.
 */

#include "foundation/core/callable.h"
#include "foundation/containers/concurrent_queue.h"
#include "foundation/containers/small_vector.h"
#include "foundation/threading/sync.h"
#include "foundation/threading/thread.h"

#include <atomic>

namespace nx {

class thread_pool;
class task_group;

namespace detail {

struct pool_task {
  function<void()> fn;
  task_group *group = nullptr;
};

struct future_state_base {
  std::atomic<u32> ready{0};
  std::atomic<u32> refs{2}; // one for the producing task, one for the future
  thread_pool *pool = nullptr;
};

template <typename T> struct future_state : future_state_base {
  alignas(T) u8 storage[sizeof(T)];

  T *value() noexcept { return std::launder(reinterpret_cast<T *>(storage)); }
};

template <> struct future_state<void> : future_state_base {};

template <typename T> void future_state_release(future_state<T> *st) noexcept {
  if (st->refs.fetch_sub(1, std::memory_order_acq_rel) != 1)
    return;
  if constexpr (!std::is_void_v<T> && !std::is_trivially_destructible_v<T>) {
    if (st->ready.load(std::memory_order_acquire) != 0)
      st->value()->~T();
  }
  release(st);
}
} // namespace detail

/**
 * @brief Completion counter for a batch of thread_pool tasks.
 *
 * Submit any number of tasks against a group, then wait() until every one
 * has finished. Reusable after wait() returns. A group is bound to the
 * first pool it is used with and must outlive its tasks.
 */
class task_group {
public:
  task_group() = default;
  ~task_group() {
    assert(pending_.load(std::memory_order_acquire) == 0 &&
           "task_group destroyed with tasks still pending");
  }
  task_group(const task_group &) = delete;
  task_group &operator=(const task_group &) = delete;

  [[nodiscard]] bool done() const noexcept {
    return pending_.load(std::memory_order_acquire) == 0;
  }

  /// Blocks until all tasks in the group finished, executing queued pool
  /// tasks (not necessarily this group's) while it waits.
  void wait() noexcept;

private:
  friend class thread_pool;

  std::atomic<u32> pending_{0};
  thread_pool *pool_ = nullptr;
};

/**
 * @brief One-shot result handle returned by thread_pool::async.
 *
 * Move-only. get() waits for the task (helping the pool execute queued
 * work) and moves the result out; it may be called once on a valid future.
 * Destroying a future early is safe -- the task still runs, the result is
 * discarded.
 *
 * @tparam T Result type of the task (may be void).
 */
template <typename T> class future {
public:
  future() = default;
  ~future() { reset(); }

  future(const future &) = delete;
  future &operator=(const future &) = delete;

  future(future &&o) noexcept : state_(o.state_) { o.state_ = nullptr; }

  future &operator=(future &&o) noexcept {
    if (this != &o) {
      reset();
      state_ = o.state_;
      o.state_ = nullptr;
    }
    return *this;
  }

  [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }

  [[nodiscard]] bool ready() const noexcept {
    return state_ && state_->ready.load(std::memory_order_acquire) != 0;
  }

  void wait() noexcept;

  T get();

private:
  friend class thread_pool;
  explicit future(detail::future_state<T> *s) noexcept : state_(s) {}

  void reset() noexcept {
    if (state_) {
      detail::future_state_release(state_);
      state_ = nullptr;
    }
  }

  detail::future_state<T> *state_ = nullptr;
};

/**
 * @brief Fixed-size work-stealing thread pool.
 *
 * Workers are named, optionally pinned to physical cores, and sleep on a
 * futex when idle. Submission from a worker thread goes to that worker's
 * local queue (LIFO-friendly, no contention); external submissions go to
 * the shared queue. See the file header for the waiting model.
 */
class thread_pool {
public:
  struct config {
    /// Worker count; 0 = one per physical core, minus one for the caller.
    u32 thread_count = 0;
    /// Worker thread name prefix ("<name>-<index>").
    const char *name = "nx-worker";
    /// Pin each worker to one physical core (round-robin over the topology).
    bool pin_threads = false;
    thread_priority priority = thread_priority::Normal;
  };

  thread_pool() : thread_pool(config{}) {}
  explicit thread_pool(const config &cfg);

  /// Waits for all pending tasks, then stops and joins the workers.
  /// No submissions may race with destruction.
  ~thread_pool();

  thread_pool(const thread_pool &) = delete;
  thread_pool &operator=(const thread_pool &) = delete;

  [[nodiscard]] u32 worker_count() const noexcept {
    return nx::cast<u32>(workers_.size());
  }

  /// True if the calling thread is one of this pool's workers.
  [[nodiscard]] bool is_worker_thread() const noexcept;

  /// Queues a fire-and-forget task.
  void submit(function<void()> fn) { enqueue({std::move(fn), nullptr}); }

  /// Queues a task tracked by @p group.
  void submit(task_group &group, function<void()> fn);

  template <typename F>
    requires(!std::is_same_v<std::decay_t<F>, function<void()>>)
  void submit(F &&fn) {
    submit(function<void()>(std::forward<F>(fn)));
  }

  template <typename F>
    requires(!std::is_same_v<std::decay_t<F>, function<void()>>)
  void submit(task_group &group, F &&fn) {
    submit(group, function<void()>(std::forward<F>(fn)));
  }

  /// Queues @p fn and returns a future for its result.
  template <typename F, typename R = std::invoke_result_t<std::decay_t<F>>>
  [[nodiscard]] future<R> async(F &&fn) {
    auto *st = allocate<detail::future_state<R>>();
    st->pool = this;
    submit(function<void()>(
        [st, f = std::decay_t<F>(std::forward<F>(fn))]() mutable {
          if constexpr (std::is_void_v<R>)
            f();
          else
            ::new (st->storage) R(f());
          st->ready.store(1, std::memory_order_release);
          futex::wake_all(&st->ready);
          detail::future_state_release(st);
        }));
    return future<R>(st);
  }

  /// Calls body(i) for every i in [begin, end) across the pool. Blocks
  /// until complete; the calling thread executes chunks too.
  template <typename F>
  void parallel_for(usize begin, usize end, F &&body) {
    parallel_for(begin, end, 0, std::forward<F>(body));
  }

  /// @param grain Indices per task; 0 picks worker_count * 4 chunks.
  template <typename F>
  void parallel_for(usize begin, usize end, usize grain, F &&body) {
    if (begin >= end)
      return;
    const usize n = end - begin;
    if (grain == 0)
      grain = max<usize>(1, n / (nx::cast<usize>(worker_count()) * 4 + 1));
    if (n <= grain || worker_count() == 0) {
      for (usize i = begin; i < end; ++i)
        body(i);
      return;
    }

    // The body outlives every chunk because wait() below returns only
    // after the last task finished, so capturing a pointer is safe.
    std::remove_reference_t<F> *bp = std::addressof(body);
    task_group group;
    for (usize chunk = begin; chunk < end; chunk += grain) {
      const usize cb = chunk;
      const usize ce = min(chunk + grain, end);
      submit(group, [bp, cb, ce] {
        for (usize i = cb; i < ce; ++i)
          (*bp)(i);
      });
    }
    group.wait();
  }

  template <typename F> void parallel_region(u32 requested, F &&body) {
    assert(!is_worker_thread() &&
           "parallel_region must run from a non-worker thread");
    const u32 workers = worker_count();
    const u32 count =
        requested == 0 ? workers + 1 : nx::min(requested, workers + 1);
    barrier bar(count);
    if (count <= 1) {
      body(u32{0}, u32{1}, bar);
      return;
    }

    // bp/bar outlive every task: group.wait() below returns only after the
    // last body has finished, so a pointer into this frame is safe.
    std::remove_reference_t<F> *bp = std::addressof(body);
    task_group group;
    for (u32 i = 1; i < count; ++i)
      submit(group, [bp, i, count, &bar] { (*bp)(i, count, bar); });
    body(u32{0}, count, bar);
    group.wait();
  }

  /// Blocks until the pool has no queued or running tasks, executing
  /// queued tasks while it waits. Must not be called from inside a pool
  /// task (the calling task itself would never count as finished); use a
  /// task_group there instead.
  void wait_idle() noexcept;

  /// Runs one queued task on the calling thread if any is available.
  /// Safe to call from any thread; returns false when nothing ran.
  bool help_execute_one() noexcept;

private:
  friend class task_group;

  static constexpr usize LOCAL_QUEUE_SIZE = 256;
  static constexpr usize SHARED_QUEUE_SIZE = 1024;
  static constexpr u32 INVALID_WORKER = ~0u;

  struct worker {
    mpmc::Queue<detail::pool_task, LOCAL_QUEUE_SIZE> queue;
    thread th;
    u32 index = 0;
    char name[32] = {};
  };

  void enqueue(detail::pool_task &&t);
  bool try_get_task(detail::pool_task &out, u32 self_index) noexcept;
  void run_task(detail::pool_task &t) noexcept;
  void worker_loop(worker &w) noexcept;

  small_vector<worker *, 32> workers_;
  mpmc::Queue<detail::pool_task, SHARED_QUEUE_SIZE> shared_queue_;

  mutex overflow_mutex_;
  vector<detail::pool_task> overflow_;
  usize overflow_head_ = 0;
  std::atomic<u32> overflow_count_{0};

  std::atomic<u32> unfinished_{0}; // queued + running tasks
  std::atomic<u32> signal_{0};     // futex word idle workers sleep on
  std::atomic<u32> sleepers_{0};
  std::atomic<u32> steal_seed_{0};
  std::atomic<bool> stop_{false};
};

template <typename T> void future<T>::wait() noexcept {
  if (!state_)
    return;
  std::atomic<u32> &ready = state_->ready;
  while (ready.load(std::memory_order_acquire) == 0) {
    if (state_->pool && state_->pool->help_execute_one())
      continue;
    futex::wait(&ready, 0, detail::WAIT_FOREVER);
  }
}

template <typename T> T future<T>::get() {
  assert(valid() && "future::get on an invalid future");
  wait();
  if constexpr (std::is_void_v<T>) {
    reset();
  } else {
    T result = std::move(*state_->value());
    reset();
    return result;
  }
}

template <class T, class Fn>
void parallel_collect(thread_pool *pool, const usize count,
                      const usize parallel_min,
                      nx::vector<nx::vector<T>> &bins, nx::vector<T> &out,
                      Fn &&body) {
  if (count == 0)
    return;
  if (pool == nullptr || count < parallel_min) {
    body(usize{0}, count, out);
    return;
  }

  // The caller's thread takes a chunk too rather than waiting on the others.
  const u32 team = pool->worker_count() + 1;
  if (bins.size() < team)
    bins.resize(team);
  for (auto &bin : bins)
    bin.clear();

  const usize per_chunk = (count + team - 1) / team;
  pool->parallel_region(team, [&](const u32 c, u32, nx::barrier &) {
    const usize first = min(nx::cast<usize>(c) * per_chunk, count);
    body(first, min(first + per_chunk, count), bins[c]);
  });

  for (auto &bin : bins)
    for (const T &value : bin)
      out.push_back(value);
}

} // namespace nx
