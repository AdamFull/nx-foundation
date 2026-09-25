#pragma once

#include "core/foundation/containers/concurrent_queue.h"
#include "core/foundation/containers/small_vector.h"
#include "core/foundation/core/callable.h"
#include "core/foundation/fibers/scheduler.h"
#include "core/foundation/fibers/sync.h"
#include "core/foundation/threading/cancellation.h"
#include "core/foundation/threading/sync.h"
#include "core/foundation/threading/thread.h"

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>

namespace nx {

class thread_pool;
class task_group;
template <typename T> class completion_source;

/// Scheduling urgency within a pool. Critical is intended for work on which
/// the current frame or an audio deadline waits; Background is for progressive
/// IO/decoding. Aging in the dispatcher guarantees lower lanes make progress.
enum class task_priority : u8 {
  Background,
  Normal,
  Critical,
  Count,
};

[[nodiscard]] constexpr usize
task_priority_index(const task_priority priority) noexcept {
  return static_cast<usize>(priority);
}

struct task_lane_statistics {
  u64 submitted = 0;
  u64 started = 0;
  u64 completed = 0;
  u64 stolen = 0;
  u64 overflowed = 0;
  u64 execution_ns = 0;
  u32 queued = 0;
  u32 peak_queued = 0;
  u32 active = 0;
  u32 peak_active = 0;
};

struct thread_pool_statistics {
  std::array<task_lane_statistics, static_cast<usize>(task_priority::Count)>
      lanes{};
  u32 workers = 0;
  u32 background_limit = 0;
  u32 background_active = 0;
  bool enabled = false;
  bool timings_enabled = false;
};

template <typename T> struct when_any_result {
  usize index = 0;
  T value;
};

template <> struct when_any_result<void> {
  usize index = 0;
};

namespace detail {

using task_fn = unique_function<void(), 24>;

struct pool_task {
  task_fn fn;
  task_group *group = nullptr;
  task_priority priority = task_priority::Normal;
  bool holds_background_slot = false;
};
static_assert(sizeof(pool_task) <= 48,
              "pool tasks should fit beside queue metadata in one cache line");

struct future_continuation {
  future_continuation *next = nullptr;
  thread_pool *pool = nullptr;
  task_priority priority = task_priority::Normal;
  task_fn fn;
};

struct future_state_base {
  // 0 pending, 1 value/void ready, 2 failed, 3 cancelled.
  std::atomic<u32> ready{0};
  std::atomic<u32> refs{2};
  thread_pool *pool = nullptr;
  std::exception_ptr error;
  std::atomic<future_continuation *> continuations{nullptr};
  fiber::Counter done{1};
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
    if (st->ready.load(std::memory_order_acquire) == 1)
      st->value()->~T();
  }
  release(st);
}

void future_state_finish(future_state_base *state, u32 status) noexcept;
void future_state_continue(future_state_base *state,
                           future_continuation *continuation) noexcept;

template <typename T, typename F> struct continuation_result {
  using type = std::invoke_result_t<F &, T &&>;
};

template <typename F> struct continuation_result<void, F> {
  using type = std::invoke_result_t<F &>;
};

template <typename T, typename F>
using continuation_result_t = typename continuation_result<T, F>::type;

template <typename F,
          bool WithToken = std::is_invocable_v<F &, cancellation_token>>
struct cancellable_result;

template <typename F> struct cancellable_result<F, true> {
  using type = std::invoke_result_t<F &, cancellation_token>;
};

template <typename F> struct cancellable_result<F, false> {
  using type = std::invoke_result_t<F &>;
};

template <typename F>
using cancellable_result_t = typename cancellable_result<F>::type;
} // namespace detail

/// Completion counter for a batch of tasks: submit any number against a group,
/// then wait(). Reusable afterwards, and must outlive its tasks. Waiting in a
/// task suspends its fiber rather than blocking the worker.
class task_group {
public:
  task_group() = default;
  ~task_group() {
    assert(m_pending.value() == 0 &&
           "task_group destroyed with tasks still pending");
  }
  task_group(const task_group &) = delete;
  task_group &operator=(const task_group &) = delete;

  [[nodiscard]] bool done() const noexcept { return m_pending.value() == 0; }

  void wait() noexcept { fiber::wait(m_pending); }

private:
  friend class thread_pool;

  fiber::Counter m_pending;
};

/// Move-only. get() waits (suspending a waiting fiber) and moves the result
/// out; once per valid future.
template <typename T> class future {
public:
  future() = default;
  ~future() { reset(); }

  future(const future &) = delete;
  future &operator=(const future &) = delete;

  future(future &&o) noexcept : m_state(o.m_state) { o.m_state = nullptr; }

  future &operator=(future &&o) noexcept {
    if (this != &o) {
      reset();
      m_state = o.m_state;
      o.m_state = nullptr;
    }
    return *this;
  }

  [[nodiscard]] bool valid() const noexcept { return m_state != nullptr; }

  [[nodiscard]] bool ready() const noexcept {
    return m_state && m_state->ready.load(std::memory_order_acquire) != 0;
  }

  [[nodiscard]] bool failed() const noexcept {
    return m_state && m_state->ready.load(std::memory_order_acquire) == 2;
  }

  [[nodiscard]] bool cancelled() const noexcept {
    return m_state && m_state->ready.load(std::memory_order_acquire) == 3;
  }

  void wait() noexcept;

  T get();

  /// Consumes this future and schedules @p fn only after it completes. The
  /// continuation itself never blocks a worker waiting for its predecessor.
  /// A predecessor failure bypasses fn and is propagated to the returned
  /// future.
  template <typename F>
  [[nodiscard]] auto
  then(F &&fn) && -> future<detail::continuation_result_t<T, std::decay_t<F>>>;

  template <typename F>
  [[nodiscard]] auto
  then(task_priority priority,
       F &&fn) && -> future<detail::continuation_result_t<T, std::decay_t<F>>>;

  template <typename F>
  [[nodiscard]] auto
  then(thread_pool &pool, task_priority priority,
       F &&fn) && -> future<detail::continuation_result_t<T, std::decay_t<F>>>;

private:
  friend class thread_pool;
  friend class completion_source<T>;
  template <typename> friend class future;
  explicit future(detail::future_state<T> *s) noexcept : m_state(s) {}

  void reset() noexcept {
    if (m_state) {
      detail::future_state_release(m_state);
      m_state = nullptr;
    }
  }

  detail::future_state<T> *m_state = nullptr;
};

/// Single-producer completion endpoint for asynchronous work owned by an
/// external subsystem (OS I/O, device callbacks, timers, and similar sources).
/// It creates the same future used by thread_pool, so continuations remain in
/// the engine scheduler without parking a worker until the external event.
template <typename T> class completion_source {
public:
  explicit completion_source(thread_pool *continuation_pool = nullptr) noexcept
      : m_state(allocate<detail::future_state<T>>()) {
    if (m_state != nullptr) {
      // The source owns the initial reference. get_future() adds the consumer
      // reference instead of relying on the two-owner thread_pool default.
      m_state->refs.store(1, std::memory_order_relaxed);
      m_state->pool = continuation_pool;
    }
  }

  ~completion_source() { abandon(); }

  completion_source(const completion_source &) = delete;
  completion_source &operator=(const completion_source &) = delete;

  completion_source(completion_source &&other) noexcept
      : m_state(other.m_state), m_future_taken(other.m_future_taken) {
    other.m_state = nullptr;
    other.m_future_taken = false;
  }

  completion_source &operator=(completion_source &&other) noexcept {
    if (this != &other) {
      abandon();
      m_state = other.m_state;
      m_future_taken = other.m_future_taken;
      other.m_state = nullptr;
      other.m_future_taken = false;
    }
    return *this;
  }

  [[nodiscard]] bool valid() const noexcept { return m_state != nullptr; }

  /// Returns the sole consumer handle. Calling this more than once yields an
  /// invalid future rather than manufacturing multiple consumers for a
  /// move-only result.
  [[nodiscard]] future<T> get_future() noexcept {
    if (m_state == nullptr || m_future_taken)
      return {};
    m_future_taken = true;
    m_state->refs.fetch_add(1, std::memory_order_relaxed);
    return future<T>(m_state);
  }

  template <typename... Args>
    requires(!std::is_void_v<T>)
  void set_value(Args &&...args) {
    assert(m_state != nullptr && "completion_source completed twice");
    ::new (m_state->storage) T(std::forward<Args>(args)...);
    finish(1);
  }

  void set_value()
    requires(std::is_void_v<T>)
  {
    assert(m_state != nullptr && "completion_source completed twice");
    finish(1);
  }

  void set_exception(std::exception_ptr error) noexcept {
    assert(m_state != nullptr && "completion_source completed twice");
    m_state->error = std::move(error);
    finish(2);
  }

  void set_cancelled() noexcept {
    assert(m_state != nullptr && "completion_source completed twice");
    finish(3);
  }

private:
  void finish(const u32 status) noexcept {
    detail::future_state<T> *const state = m_state;
    m_state = nullptr;
    m_future_taken = false;
    detail::future_state_finish(state, status);
    detail::future_state_release(state);
  }

  void abandon() noexcept {
    if (m_state == nullptr)
      return;
    // A destroyed producer must never leave its consumer waiting forever.
    set_cancelled();
  }

  detail::future_state<T> *m_state = nullptr;
  bool m_future_taken = false;
};

/// A task pool on an nx::fiber::Scheduler of its own. Tasks run on fibers, so
/// a task that waits on a group, a future or a fiber primitive suspends
/// instead of occupying its worker.
class thread_pool {
public:
  struct config {
    u32 thread_count = 0;
    const char *name = "nx-worker";
    /// Not honoured by the fiber scheduler, which leaves placement to the OS.
    bool pin_threads = false;
    thread_priority priority = thread_priority::Normal;
    /// Zero reserves one worker from background work when possible. An
    /// explicit value is clamped to the number of workers.
    u32 max_background_concurrency = 0;
    /// Service threads for nx::fiber::run_blocking from this pool's tasks.
    u32 blocking_threads = 2;
    /// The constructing thread becomes the pool's main thread. It runs no
    /// tasks, only run_on_main() work, inside pump_main() or while it waits,
    /// always on its own stack, and it must destroy the pool.
    bool adopt_main_thread = false;
    /// Relaxed counters are enabled by default in debug and opt-in in release.
#if defined(NX_DEBUG)
    bool collect_statistics = true;
#else
    bool collect_statistics = false;
#endif
    /// Adds a monotonic clock read around every task. Keep disabled unless
    /// profiling aggregate execution time.
    bool collect_timings = false;
  };

  thread_pool() : thread_pool(config{}) {}
  explicit thread_pool(const config &cfg);

  /// Waits for all pending tasks, then stops and joins the workers.
  /// No submissions may race with destruction.
  ~thread_pool();

  thread_pool(const thread_pool &) = delete;
  thread_pool &operator=(const thread_pool &) = delete;

  [[nodiscard]] u32 worker_count() const noexcept {
    return m_scheduler->worker_count();
  }
  [[nodiscard]] u32 blocking_thread_count() const noexcept {
    return m_scheduler->stats().blocking_threads;
  }

  [[nodiscard]] bool is_worker_thread() const noexcept;
  [[nodiscard]] bool has_main_thread() const noexcept {
    return m_has_main_thread;
  }

  /// Runs @p fn on the main thread's own stack and returns once it has. The
  /// main thread, or a pool without one, runs it in place; anything else
  /// queues it and waits, a task by suspending. An exception from @p fn is
  /// rethrown here.
  void run_on_main(function_ref<void()> fn);
  /// On the main thread outside any task: runs main-thread work until none
  /// is left and @p budget has passed.
  void pump_main(std::chrono::nanoseconds budget);

  [[nodiscard]] thread_pool_statistics statistics() const noexcept;
  /// Reset is only valid while the pool is idle.
  void reset_statistics() noexcept;

  void submit(detail::task_fn fn) {
    enqueue({std::move(fn), nullptr, task_priority::Normal});
  }

  void submit(task_priority priority, detail::task_fn fn) {
    enqueue({std::move(fn), nullptr, priority});
  }

  void submit(task_group &group, detail::task_fn fn);
  void submit(task_group &group, task_priority priority, detail::task_fn fn);

  template <typename F>
    requires(!std::is_same_v<std::decay_t<F>, detail::task_fn>)
  void submit(F &&fn) {
    submit(detail::task_fn(std::forward<F>(fn)));
  }

  template <typename F>
    requires(!std::is_same_v<std::decay_t<F>, detail::task_fn>)
  void submit(task_priority priority, F &&fn) {
    submit(priority, detail::task_fn(std::forward<F>(fn)));
  }

  template <typename F>
    requires(!std::is_same_v<std::decay_t<F>, detail::task_fn>)
  void submit(task_group &group, F &&fn) {
    submit(group, detail::task_fn(std::forward<F>(fn)));
  }

  template <typename F>
    requires(!std::is_same_v<std::decay_t<F>, detail::task_fn>)
  void submit(task_group &group, task_priority priority, F &&fn) {
    submit(group, priority, detail::task_fn(std::forward<F>(fn)));
  }

  template <typename F> void submit(cancellation_token token, F &&fn) {
    submit(task_priority::Normal, std::move(token), std::forward<F>(fn));
  }

  template <typename F>
  void submit(task_priority priority, cancellation_token token, F &&fn) {
    using Fn = std::decay_t<F>;
    static_assert(std::is_invocable_v<Fn &> ||
                  std::is_invocable_v<Fn &, cancellation_token>);
    submit(priority, [token = std::move(token),
                      callable = Fn(std::forward<F>(fn))]() mutable {
      token.throw_if_requested();
      if constexpr (std::is_invocable_v<Fn &, cancellation_token>)
        callable(token);
      else
        callable();
    });
  }

  template <typename F>
  void submit(task_group &group, cancellation_token token, F &&fn) {
    submit(group, task_priority::Normal, std::move(token), std::forward<F>(fn));
  }

  template <typename F>
  void submit(task_group &group, task_priority priority,
              cancellation_token token, F &&fn) {
    using Fn = std::decay_t<F>;
    static_assert(std::is_invocable_v<Fn &> ||
                  std::is_invocable_v<Fn &, cancellation_token>);
    submit(group, priority,
           [token = std::move(token),
            callable = Fn(std::forward<F>(fn))]() mutable {
             token.throw_if_requested();
             if constexpr (std::is_invocable_v<Fn &, cancellation_token>)
               callable(token);
             else
               callable();
           });
  }

  template <typename F, typename R = std::invoke_result_t<std::decay_t<F>>>
  [[nodiscard]] future<R> async(F &&fn) {
    return async(task_priority::Normal, std::forward<F>(fn));
  }

  template <typename F, typename R = std::invoke_result_t<std::decay_t<F>>>
  [[nodiscard]] future<R> async(task_priority priority, F &&fn) {
    static_assert(!std::is_reference_v<R>,
                  "thread_pool::async cannot store a reference result");
    auto *st = allocate<detail::future_state<R>>();
    if (st == nullptr)
      return {};
    st->pool = this;
    detail::task_fn task{
        [st, f = std::decay_t<F>(std::forward<F>(fn))]() mutable {
          try {
            if constexpr (std::is_void_v<R>)
              f();
            else
              ::new (st->storage) R(f());
            detail::future_state_finish(st, 1);
          } catch (const task_cancelled &) {
            st->error = std::current_exception();
            detail::future_state_finish(st, 3);
          } catch (...) {
            st->error = std::current_exception();
            detail::future_state_finish(st, 2);
          }
          detail::future_state_release(st);
        }};
    if (!task) {
      // Preserve progress even if the callable's out-of-line storage could not
      // be allocated. get() reports a task failure instead of waiting forever.
      detail::future_state_finish(st, 2);
      detail::future_state_release(st);
      return future<R>(st);
    }
    submit(priority, std::move(task));
    return future<R>(st);
  }

  template <typename F, typename Fn = std::decay_t<F>,
            typename R = detail::cancellable_result_t<Fn>>
  [[nodiscard]] future<R> async(cancellation_token token, F &&fn) {
    return async(task_priority::Normal, std::move(token), std::forward<F>(fn));
  }

  template <typename F, typename Fn = std::decay_t<F>,
            typename R = detail::cancellable_result_t<Fn>>
  [[nodiscard]] future<R> async(task_priority priority,
                                cancellation_token token, F &&fn) {
    static_assert(std::is_invocable_v<Fn &> ||
                  std::is_invocable_v<Fn &, cancellation_token>);
    return async(
        priority,
        [token = std::move(token),
         callable = Fn(std::forward<F>(fn))]() mutable -> R {
          token.throw_if_requested();
          if constexpr (std::is_invocable_v<Fn &, cancellation_token>) {
            if constexpr (std::is_void_v<R>)
              callable(token);
            else
              return callable(token);
          } else {
            if constexpr (std::is_void_v<R>)
              callable();
            else
              return callable();
          }
        });
  }

  /// Completes after every input and preserves input order. The first failure
  /// wins; cancellation is reported only when no input failed.
  template <typename T>
    requires(!std::is_void_v<T>)
  [[nodiscard]] future<nx::vector<T>>
  when_all(nx::vector<future<T>> futures,
           task_priority priority = task_priority::Normal);

  [[nodiscard]] future<void>
  when_all(nx::vector<future<void>> futures,
           task_priority priority = task_priority::Normal);

  /// Completes with the index and value of the first terminal input. A first
  /// failure or cancellation is propagated. Other inputs continue normally.
  template <typename T>
    requires(!std::is_void_v<T>)
  [[nodiscard]] future<when_any_result<T>>
  when_any(nx::vector<future<T>> futures,
           task_priority priority = task_priority::Normal);

  [[nodiscard]] future<when_any_result<void>>
  when_any(nx::vector<future<void>> futures,
           task_priority priority = task_priority::Normal);

  template <typename F> void parallel_for(usize begin, usize end, F &&body) {
    parallel_for(begin, end, 0, std::forward<F>(body));
  }

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

    std::remove_reference_t<F> *bp = std::addressof(body);
    task_group group;
    for (usize chunk = begin; chunk < end; chunk += grain) {
      const usize cb = chunk;
      const usize ce = min(chunk + grain, end);
      submit(group, task_priority::Critical, [bp, cb, ce] {
        for (usize i = cb; i < ce; ++i)
          (*bp)(i);
      });
    }
    group.wait();
  }

  template <typename F> void parallel_region(u32 requested, F &&body) {
    const u32 workers = worker_count();
    const u32 count =
        requested == 0 ? workers + 1 : nx::min(requested, workers + 1);
    fiber::barrier bar(count);
    if (count <= 1) {
      body(u32{0}, u32{1}, bar);
      return;
    }

    std::remove_reference_t<F> *bp = std::addressof(body);
    task_group group;
    for (u32 i = 1; i < count; ++i)
      submit(group, task_priority::Critical,
             [bp, i, count, &bar] { (*bp)(i, count, bar); });
    body(u32{0}, count, bar);
    group.wait();
  }

  /// Runs @p job on the low-priority lane, counted on @p counter, outside the
  /// background limit and wait_idle(). It does not allocate, so any thread
  /// may call it, including one inside scoped_no_alloc.
  void run_low(fiber::Job job, fiber::Counter &counter) noexcept;

  /// Waits until the pool has no queued or running tasks. Must not be called
  /// from inside a pool task (the calling task itself would never count as
  /// finished); use a task_group there instead.
  void wait_idle() noexcept;

private:
  friend class task_group;

  struct task_node;

  struct alignas(64) lane_counters {
    std::atomic<u64> submitted{0};
    std::atomic<u64> started{0};
    std::atomic<u64> completed{0};
    std::atomic<u64> stolen{0};
    std::atomic<u64> overflowed{0};
    std::atomic<u64> execution_ns{0};
    std::atomic<u32> queued{0};
    std::atomic<u32> peak_queued{0};
    std::atomic<u32> active{0};
    std::atomic<u32> peak_active{0};
  };

  static void run_node(void *arg) noexcept;

  void enqueue(detail::pool_task &&t);
  void dispatch(task_node *node) noexcept;
  void pump_background() noexcept;
  [[nodiscard]] task_node *acquire_node() noexcept;
  void release_node(task_node *node) noexcept;
  void record_enqueued(task_priority priority) noexcept;
  void record_started(task_priority priority) noexcept;
  void record_completed(task_priority priority, u64 execution_ns) noexcept;
  [[nodiscard]] bool is_execution_context() const noexcept;

  bool m_has_main_thread = false;
  unique_ptr<fiber::Scheduler> m_scheduler;
  fiber::Counter m_unfinished;
  mpmc::Queue<task_node *, 256> m_free_nodes;
  lane_counters m_counters[static_cast<usize>(task_priority::Count)];

  mutex m_background_lock;
  vector<task_node *> m_background_waiting;
  usize m_background_head = 0;
  std::atomic<u32> m_background_pending{0};
  std::atomic<u32> m_background_active{0};
  u32 m_background_limit = 1;
  bool m_collect_statistics = false;
  bool m_collect_timings = false;
};

template <typename T> void future<T>::wait() noexcept {
  if (m_state)
    fiber::wait(m_state->done);
}

template <typename T> T future<T>::get() {
  assert(valid() && "future::get on an invalid future");
  wait();
  if (m_state->ready.load(std::memory_order_acquire) == 2) {
    std::exception_ptr error = m_state->error;
    reset();
    if (error)
      std::rethrow_exception(error);
    throw std::runtime_error("thread_pool task failed without an exception");
  }
  if (m_state->ready.load(std::memory_order_acquire) == 3) {
    std::exception_ptr error = m_state->error;
    reset();
    if (error)
      std::rethrow_exception(error);
    throw task_cancelled{};
  }
  if constexpr (std::is_void_v<T>) {
    reset();
  } else {
    T result = std::move(*m_state->value());
    reset();
    return result;
  }
}

template <typename T>
template <typename F>
auto future<T>::then(
    F &&fn) && -> future<detail::continuation_result_t<T, std::decay_t<F>>> {
  if (m_state == nullptr || m_state->pool == nullptr)
    return {};
  return std::move(*this).then(*m_state->pool, task_priority::Normal,
                               std::forward<F>(fn));
}

template <typename T>
template <typename F>
auto future<T>::then(const task_priority priority, F &&fn)
    && -> future<detail::continuation_result_t<T, std::decay_t<F>>> {
  if (m_state == nullptr || m_state->pool == nullptr)
    return {};
  return std::move(*this).then(*m_state->pool, priority, std::forward<F>(fn));
}

template <typename T>
template <typename F>
auto future<T>::then(thread_pool &pool, const task_priority priority, F &&fn)
    && -> future<detail::continuation_result_t<T, std::decay_t<F>>> {
  using Fn = std::decay_t<F>;
  using R = detail::continuation_result_t<T, Fn>;
  static_assert(!std::is_reference_v<R>,
                "future::then cannot store a reference result");
  if (m_state == nullptr)
    return {};

  auto *const output = allocate<detail::future_state<R>>();
  if (output == nullptr)
    return {};
  output->pool = &pool;

  auto *const continuation = allocate<detail::future_continuation>();
  if (continuation == nullptr) {
    detail::future_state_release(output);
    detail::future_state_release(output);
    return {};
  }

  detail::future_state<T> *const input = m_state;
  continuation->pool = &pool;
  continuation->priority = priority;
  continuation->fn = detail::task_fn(
      [input, output, callable = Fn(std::forward<F>(fn))]() mutable {
        const u32 input_status = input->ready.load(std::memory_order_acquire);
        if (input_status == 2 || input_status == 3) {
          output->error = input->error;
          detail::future_state_finish(output, input_status);
        } else {
          try {
            if constexpr (std::is_void_v<T>) {
              if constexpr (std::is_void_v<R>)
                callable();
              else
                ::new (output->storage) R(callable());
            } else {
              if constexpr (std::is_void_v<R>)
                callable(std::move(*input->value()));
              else
                ::new (output->storage) R(callable(std::move(*input->value())));
            }
            detail::future_state_finish(output, 1);
          } catch (...) {
            output->error = std::current_exception();
            detail::future_state_finish(output, 2);
          }
        }
        detail::future_state_release(input);
        detail::future_state_release(output);
      });
  if (!continuation->fn) {
    release(continuation);
    detail::future_state_release(output);
    detail::future_state_release(output);
    return {};
  }

  // Transfer this handle's input reference to the continuation before it can
  // be scheduled inline by a zero-worker executor.
  m_state = nullptr;
  detail::future_state_continue(input, continuation);
  return future<R>(output);
}

namespace detail {

template <typename T> struct when_all_state {
  std::atomic<usize> remaining{0};
  nx::vector<std::optional<T>> values;
  nx::mutex terminal_lock;
  std::exception_ptr error;
  bool cancelled = false;
  future_state<nx::vector<T>> *output = nullptr;
};

struct when_all_void_state {
  std::atomic<usize> remaining{0};
  nx::mutex terminal_lock;
  std::exception_ptr error;
  bool cancelled = false;
  future_state<void> *output = nullptr;
};

template <typename State>
void note_combiner_terminal(State &state, const u32 status,
                            std::exception_ptr error) noexcept {
  const nx::scoped_lock<nx::mutex> held(state.terminal_lock);
  if (status == 2 && !state.error)
    state.error = std::move(error);
  else if (status == 3)
    state.cancelled = true;
}

template <typename T>
void finish_when_all(const nx::shared_ptr<when_all_state<T>> &state) noexcept {
  std::exception_ptr error;
  bool cancelled = false;
  {
    const nx::scoped_lock<nx::mutex> held(state->terminal_lock);
    error = state->error;
    cancelled = state->cancelled;
  }

  if (error) {
    state->output->error = std::move(error);
    future_state_finish(state->output, 2);
  } else if (cancelled) {
    state->output->error = std::make_exception_ptr(task_cancelled{});
    future_state_finish(state->output, 3);
  } else {
    try {
      nx::vector<T> values;
      values.reserve(state->values.size());
      for (std::optional<T> &value : state->values)
        values.push_back(std::move(*value));
      ::new (state->output->storage) nx::vector<T>(std::move(values));
      future_state_finish(state->output, 1);
    } catch (...) {
      state->output->error = std::current_exception();
      future_state_finish(state->output, 2);
    }
  }
  future_state_release(state->output);
}

inline void
finish_when_all(const nx::shared_ptr<when_all_void_state> &state) noexcept {
  std::exception_ptr error;
  bool cancelled = false;
  {
    const nx::scoped_lock<nx::mutex> held(state->terminal_lock);
    error = state->error;
    cancelled = state->cancelled;
  }
  if (error) {
    state->output->error = std::move(error);
    future_state_finish(state->output, 2);
  } else if (cancelled) {
    state->output->error = std::make_exception_ptr(task_cancelled{});
    future_state_finish(state->output, 3);
  } else {
    future_state_finish(state->output, 1);
  }
  future_state_release(state->output);
}

template <typename T>
void arrive_when_all(const nx::shared_ptr<when_all_state<T>> &state,
                     future_state<T> *input, const usize index) noexcept {
  const u32 status = input->ready.load(std::memory_order_acquire);
  if (status == 1) {
    try {
      state->values[index].emplace(std::move(*input->value()));
    } catch (...) {
      note_combiner_terminal(*state, 2, std::current_exception());
    }
  } else {
    note_combiner_terminal(*state, status, input->error);
  }
  future_state_release(input);
  if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
    finish_when_all(state);
}

inline void arrive_when_all(const nx::shared_ptr<when_all_void_state> &state,
                            future_state<void> *input) noexcept {
  const u32 status = input->ready.load(std::memory_order_acquire);
  if (status != 1)
    note_combiner_terminal(*state, status, input->error);
  future_state_release(input);
  if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
    finish_when_all(state);
}

template <typename T> struct when_any_state {
  std::atomic<bool> won{false};
  future_state<when_any_result<T>> *output = nullptr;
};

template <typename T>
void arrive_when_any(const nx::shared_ptr<when_any_state<T>> &state,
                     future_state<T> *input, const usize index) noexcept {
  bool expected = false;
  if (state->won.compare_exchange_strong(expected, true,
                                         std::memory_order_acq_rel)) {
    const u32 status = input->ready.load(std::memory_order_acquire);
    if (status == 1) {
      try {
        ::new (state->output->storage)
            when_any_result<T>{index, std::move(*input->value())};
        future_state_finish(state->output, 1);
      } catch (...) {
        state->output->error = std::current_exception();
        future_state_finish(state->output, 2);
      }
    } else {
      state->output->error = input->error;
      future_state_finish(state->output, status);
    }
    future_state_release(state->output);
  }
  future_state_release(input);
}

template <>
inline void arrive_when_any(const nx::shared_ptr<when_any_state<void>> &state,
                            future_state<void> *input,
                            const usize index) noexcept {
  bool expected = false;
  if (state->won.compare_exchange_strong(expected, true,
                                         std::memory_order_acq_rel)) {
    const u32 status = input->ready.load(std::memory_order_acquire);
    if (status == 1) {
      ::new (state->output->storage) when_any_result<void>{index};
      future_state_finish(state->output, 1);
    } else {
      state->output->error = input->error;
      future_state_finish(state->output, status);
    }
    future_state_release(state->output);
  }
  future_state_release(input);
}

template <typename R> future_state<R> *make_combiner_output(thread_pool *pool) {
  future_state<R> *const output = allocate<future_state<R>>();
  if (output != nullptr)
    output->pool = pool;
  return output;
}

template <typename R>
void fail_combiner_output(future_state<R> *output,
                          std::exception_ptr error) noexcept {
  output->error = std::move(error);
  future_state_finish(output, 2);
  future_state_release(output);
}

} // namespace detail

template <typename T>
  requires(!std::is_void_v<T>)
future<nx::vector<T>> thread_pool::when_all(nx::vector<future<T>> futures,
                                            const task_priority priority) {
  auto *const output = detail::make_combiner_output<nx::vector<T>>(this);
  if (output == nullptr)
    return {};
  if (futures.empty()) {
    ::new (output->storage) nx::vector<T>();
    detail::future_state_finish(output, 1);
    detail::future_state_release(output);
    return future<nx::vector<T>>(output);
  }
  for (const future<T> &input : futures) {
    if (!input.valid()) {
      detail::fail_combiner_output(
          output, std::make_exception_ptr(std::invalid_argument(
                      "when_all contains invalid future")));
      return future<nx::vector<T>>(output);
    }
  }

  auto state = nx::make_shared<detail::when_all_state<T>>();
  if (!state) {
    detail::future_state_release(output);
    detail::future_state_release(output);
    return {};
  }
  state->remaining.store(futures.size(), std::memory_order_relaxed);
  state->output = output;

  nx::vector<detail::future_continuation *> nodes;
  try {
    state->values.resize(futures.size());
    nodes.reserve(futures.size());
  } catch (...) {
    detail::fail_combiner_output(output, std::current_exception());
    return future<nx::vector<T>>(output);
  }
  for (usize i = 0; i < futures.size(); ++i) {
    auto *const node = allocate<detail::future_continuation>();
    if (node != nullptr) {
      detail::future_state<T> *const input = futures[i].m_state;
      node->pool = this;
      node->priority = priority;
      node->fn = detail::task_fn(
          [state, input, i] { detail::arrive_when_all(state, input, i); });
    }
    if (node == nullptr || !node->fn) {
      if (node != nullptr)
        release(node);
      for (detail::future_continuation *created : nodes)
        release(created);
      detail::fail_combiner_output(output,
                                   std::make_exception_ptr(std::bad_alloc{}));
      return future<nx::vector<T>>(output);
    }
    nodes.push_back(node);
  }

  for (usize i = 0; i < futures.size(); ++i) {
    detail::future_state<T> *const input = futures[i].m_state;
    futures[i].m_state = nullptr;
    detail::future_state_continue(input, nodes[i]);
  }
  return future<nx::vector<T>>(output);
}

inline future<void> thread_pool::when_all(nx::vector<future<void>> futures,
                                          const task_priority priority) {
  auto *const output = detail::make_combiner_output<void>(this);
  if (output == nullptr)
    return {};
  if (futures.empty()) {
    detail::future_state_finish(output, 1);
    detail::future_state_release(output);
    return future<void>(output);
  }
  for (const future<void> &input : futures) {
    if (!input.valid()) {
      detail::fail_combiner_output(
          output, std::make_exception_ptr(std::invalid_argument(
                      "when_all contains invalid future")));
      return future<void>(output);
    }
  }

  auto state = nx::make_shared<detail::when_all_void_state>();
  if (!state) {
    detail::future_state_release(output);
    detail::future_state_release(output);
    return {};
  }
  state->remaining.store(futures.size(), std::memory_order_relaxed);
  state->output = output;

  nx::vector<detail::future_continuation *> nodes;
  try {
    nodes.reserve(futures.size());
  } catch (...) {
    detail::fail_combiner_output(output, std::current_exception());
    return future<void>(output);
  }
  for (future<void> &source : futures) {
    auto *const node = allocate<detail::future_continuation>();
    if (node != nullptr) {
      detail::future_state<void> *const input = source.m_state;
      node->pool = this;
      node->priority = priority;
      node->fn = detail::task_fn(
          [state, input] { detail::arrive_when_all(state, input); });
    }
    if (node == nullptr || !node->fn) {
      if (node != nullptr)
        release(node);
      for (detail::future_continuation *created : nodes)
        release(created);
      detail::fail_combiner_output(output,
                                   std::make_exception_ptr(std::bad_alloc{}));
      return future<void>(output);
    }
    nodes.push_back(node);
  }

  for (usize i = 0; i < futures.size(); ++i) {
    detail::future_state<void> *const input = futures[i].m_state;
    futures[i].m_state = nullptr;
    detail::future_state_continue(input, nodes[i]);
  }
  return future<void>(output);
}

template <typename T>
  requires(!std::is_void_v<T>)
future<when_any_result<T>> thread_pool::when_any(nx::vector<future<T>> futures,
                                                 const task_priority priority) {
  auto *const output = detail::make_combiner_output<when_any_result<T>>(this);
  if (output == nullptr)
    return {};
  if (futures.empty()) {
    detail::fail_combiner_output(
        output,
        std::make_exception_ptr(std::invalid_argument("when_any is empty")));
    return future<when_any_result<T>>(output);
  }
  for (const future<T> &input : futures) {
    if (!input.valid()) {
      detail::fail_combiner_output(
          output, std::make_exception_ptr(std::invalid_argument(
                      "when_any contains invalid future")));
      return future<when_any_result<T>>(output);
    }
  }

  auto state = nx::make_shared<detail::when_any_state<T>>();
  if (!state) {
    detail::future_state_release(output);
    detail::future_state_release(output);
    return {};
  }
  state->output = output;

  nx::vector<detail::future_continuation *> nodes;
  try {
    nodes.reserve(futures.size());
  } catch (...) {
    detail::fail_combiner_output(output, std::current_exception());
    return future<when_any_result<T>>(output);
  }
  for (usize i = 0; i < futures.size(); ++i) {
    auto *const node = allocate<detail::future_continuation>();
    if (node != nullptr) {
      detail::future_state<T> *const input = futures[i].m_state;
      node->pool = this;
      node->priority = priority;
      node->fn = detail::task_fn(
          [state, input, i] { detail::arrive_when_any(state, input, i); });
    }
    if (node == nullptr || !node->fn) {
      if (node != nullptr)
        release(node);
      for (detail::future_continuation *created : nodes)
        release(created);
      detail::fail_combiner_output(output,
                                   std::make_exception_ptr(std::bad_alloc{}));
      return future<when_any_result<T>>(output);
    }
    nodes.push_back(node);
  }

  for (usize i = 0; i < futures.size(); ++i) {
    detail::future_state<T> *const input = futures[i].m_state;
    futures[i].m_state = nullptr;
    detail::future_state_continue(input, nodes[i]);
  }
  return future<when_any_result<T>>(output);
}

inline future<when_any_result<void>>
thread_pool::when_any(nx::vector<future<void>> futures,
                      const task_priority priority) {
  auto *const output =
      detail::make_combiner_output<when_any_result<void>>(this);
  if (output == nullptr)
    return {};
  if (futures.empty()) {
    detail::fail_combiner_output(
        output,
        std::make_exception_ptr(std::invalid_argument("when_any is empty")));
    return future<when_any_result<void>>(output);
  }
  for (const future<void> &input : futures) {
    if (!input.valid()) {
      detail::fail_combiner_output(
          output, std::make_exception_ptr(std::invalid_argument(
                      "when_any contains invalid future")));
      return future<when_any_result<void>>(output);
    }
  }

  auto state = nx::make_shared<detail::when_any_state<void>>();
  if (!state) {
    detail::future_state_release(output);
    detail::future_state_release(output);
    return {};
  }
  state->output = output;

  nx::vector<detail::future_continuation *> nodes;
  try {
    nodes.reserve(futures.size());
  } catch (...) {
    detail::fail_combiner_output(output, std::current_exception());
    return future<when_any_result<void>>(output);
  }
  for (usize i = 0; i < futures.size(); ++i) {
    auto *const node = allocate<detail::future_continuation>();
    if (node != nullptr) {
      detail::future_state<void> *const input = futures[i].m_state;
      node->pool = this;
      node->priority = priority;
      node->fn = detail::task_fn(
          [state, input, i] { detail::arrive_when_any(state, input, i); });
    }
    if (node == nullptr || !node->fn) {
      if (node != nullptr)
        release(node);
      for (detail::future_continuation *created : nodes)
        release(created);
      detail::fail_combiner_output(output,
                                   std::make_exception_ptr(std::bad_alloc{}));
      return future<when_any_result<void>>(output);
    }
    nodes.push_back(node);
  }

  for (usize i = 0; i < futures.size(); ++i) {
    detail::future_state<void> *const input = futures[i].m_state;
    futures[i].m_state = nullptr;
    detail::future_state_continue(input, nodes[i]);
  }
  return future<when_any_result<void>>(output);
}

template <typename T>
  requires(!std::is_void_v<T>)
[[nodiscard]] future<nx::vector<T>>
when_all(thread_pool &pool, nx::vector<future<T>> futures,
         const task_priority priority = task_priority::Normal) {
  return pool.when_all(std::move(futures), priority);
}

[[nodiscard]] inline future<void>
when_all(thread_pool &pool, nx::vector<future<void>> futures,
         const task_priority priority = task_priority::Normal) {
  return pool.when_all(std::move(futures), priority);
}

template <typename T>
  requires(!std::is_void_v<T>)
[[nodiscard]] future<when_any_result<T>>
when_any(thread_pool &pool, nx::vector<future<T>> futures,
         const task_priority priority = task_priority::Normal) {
  return pool.when_any(std::move(futures), priority);
}

[[nodiscard]] inline future<when_any_result<void>>
when_any(thread_pool &pool, nx::vector<future<void>> futures,
         const task_priority priority = task_priority::Normal) {
  return pool.when_any(std::move(futures), priority);
}

template <class T, class Fn>
void parallel_collect(thread_pool *pool, const usize count,
                      const usize parallel_min, nx::vector<nx::vector<T>> &bins,
                      nx::vector<T> &out, Fn &&body) {
  if (count == 0)
    return;
  if (pool == nullptr || count < parallel_min) {
    body(usize{0}, count, out);
    return;
  }

  const u32 team = pool->worker_count() + 1;
  if (bins.size() < team)
    bins.resize(team);
  for (auto &bin : bins)
    bin.clear();

  const usize per_chunk = (count + team - 1) / team;
  pool->parallel_region(team, [&](const u32 c, u32, fiber::barrier &) {
    const usize first = min(nx::cast<usize>(c) * per_chunk, count);
    body(first, min(first + per_chunk, count), bins[c]);
  });

  for (auto &bin : bins)
    for (const T &value : bin)
      out.push_back(value);
}

} // namespace nx
