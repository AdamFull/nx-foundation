#pragma once

#include "core/foundation/containers/concurrent_queue.h"
#include "core/foundation/containers/small_vector.h"
#include "core/foundation/core/callable.h"
#include "core/foundation/threading/sync.h"
#include "core/foundation/threading/thread.h"

#include <atomic>
#include <exception>
#include <stdexcept>

namespace nx {

class thread_pool;
class task_group;

/// Scheduling urgency within a pool. Critical is intended for work on which
/// the current frame or an audio deadline waits; Background is for progressive
/// IO/decoding. Aging in the dispatcher guarantees lower lanes make progress.
enum class task_priority : u8 {
  Background,
  Normal,
  Critical,
  Count,
};

namespace detail {

using task_fn = unique_function<void(), 24>;

struct pool_task {
  task_fn fn;
  task_group *group = nullptr;
  task_priority priority = task_priority::Normal;
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
  // 0 pending, 1 value/void ready, 2 failed with error populated.
  std::atomic<u32> ready{0};
  std::atomic<u32> refs{2};
  thread_pool *pool = nullptr;
  std::exception_ptr error;
  std::atomic<future_continuation *> continuations{nullptr};
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
}

/// Completion counter for a batch of tasks: submit any number against a group,
/// then wait(). Reusable afterwards. A group is bound to the first pool it is
/// used with and must outlive its tasks.
class task_group {
public:
  task_group() = default;
  ~task_group() {
    assert(m_pending.load(std::memory_order_acquire) == 0 &&
           "task_group destroyed with tasks still pending");
  }
  task_group(const task_group &) = delete;
  task_group &operator=(const task_group &) = delete;

  [[nodiscard]] bool done() const noexcept {
    return m_pending.load(std::memory_order_acquire) == 0;
  }

  void wait() noexcept;

private:
  friend class thread_pool;

  std::atomic<u32> m_pending{0};
  thread_pool *m_pool = nullptr;
};

/// Move-only. get() waits (helping the pool) and moves the result out; once per valid future.
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

class thread_pool {
public:
  struct config {
    u32 thread_count = 0;
    const char *name = "nx-worker";
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
    return nx::cast<u32>(m_workers.size());
  }

  [[nodiscard]] bool is_worker_thread() const noexcept;

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

    std::remove_reference_t<F> *bp = std::addressof(body);
    task_group group;
    for (u32 i = 1; i < count; ++i)
      submit(group, task_priority::Critical,
             [bp, i, count, &bar] { (*bp)(i, count, bar); });
    body(u32{0}, count, bar);
    group.wait();
  }

  /// Blocks until the pool has no queued or running tasks, executing queued
  /// tasks while it waits. Must not be called from inside a pool task (the
  /// calling task itself would never count as finished); use a task_group there
  /// instead.
  void wait_idle() noexcept;

  bool help_execute_one() noexcept;

private:
  friend class task_group;

  static constexpr usize LOCAL_QUEUE_SIZE = 256;
  static constexpr usize SHARED_QUEUE_SIZE = 1024;
  static constexpr u32 INVALID_WORKER = ~0u;

  struct worker {
    mpmc::Queue<detail::pool_task, LOCAL_QUEUE_SIZE>
        queues[static_cast<usize>(task_priority::Count)];
    thread th;
    u32 index = 0;
    u32 dispatches = 0;
    u32 steal_cursor = 0;
    char name[32] = {};
  };

  struct overflow_queue {
    mutex lock;
    vector<detail::pool_task> tasks;
    usize head = 0;
    std::atomic<u32> count{0};
  };

  void enqueue(detail::pool_task &&t);
  bool try_get_task(detail::pool_task &out, u32 self_index, u32 &dispatches,
                    u32 &steal_cursor) noexcept;
  bool try_get_priority(detail::pool_task &out, u32 self_index,
                        task_priority priority, u32 &steal_cursor) noexcept;
  void run_task(detail::pool_task &t) noexcept;
  void worker_loop(worker &w) noexcept;

  small_vector<worker *, 32> m_workers;
  mpmc::Queue<detail::pool_task, SHARED_QUEUE_SIZE>
      m_shared_queues[static_cast<usize>(task_priority::Count)];
  overflow_queue m_overflow[static_cast<usize>(task_priority::Count)];

  std::atomic<u32> m_unfinished{0};
  std::atomic<u32> m_signal{0};
  std::atomic<u32> m_sleepers{0};
  std::atomic<bool> m_stop{false};
};

template <typename T> void future<T>::wait() noexcept {
  if (!m_state)
    return;
  std::atomic<u32> &ready = m_state->ready;
  while (ready.load(std::memory_order_acquire) == 0) {
    if (m_state->pool && m_state->pool->help_execute_one())
      continue;
    futex::wait(&ready, 0, detail::WAIT_FOREVER);
  }
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
        if (input->ready.load(std::memory_order_acquire) == 2) {
          output->error = input->error;
          detail::future_state_finish(output, 2);
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
  pool->parallel_region(team, [&](const u32 c, u32, nx::barrier &) {
    const usize first = min(nx::cast<usize>(c) * per_chunk, count);
    body(first, min(first + per_chunk, count), bins[c]);
  });

  for (auto &bin : bins)
    for (const T &value : bin)
      out.push_back(value);
}

}
