#pragma once

#include "core/foundation/fibers/sync.h"
#include "core/foundation/threading/thread_pool.h"

#include <type_traits>

namespace nx {

/// Runs tasks on a thread_pool one at a time, in the order they were posted,
/// with no thread of its own: while any are queued, one pool task runs them,
/// and nothing is scheduled while the queue is empty. For work that must not
/// overlap or reorder, such as one database's reads and writes. A task that
/// throws is logged, and the next one runs.
class serial_queue {
public:
  using task = unique_function<void()>;

  struct config {
    /// Tasks waiting at most; post() refuses past it.
    usize capacity = 256;
    task_priority priority = task_priority::Background;
    /// Runs tasks through fiber::run_blocking, for work that blocks in the
    /// OS, such as file or database access: the worker runs other jobs
    /// meanwhile.
    bool blocking = false;
  };

  /// @p pool must outlive the queue.
  serial_queue(thread_pool &pool, const config &cfg);
  /// Stops taking tasks and waits until the queued ones have run.
  ~serial_queue();

  serial_queue(const serial_queue &) = delete;
  serial_queue &operator=(const serial_queue &) = delete;

  /// Queues @p fn after every task posted before it. False when closed, or
  /// when full unless @p over_capacity: for a task whose refusal would leave
  /// the ones after it working on the wrong state.
  bool post(task fn, bool over_capacity = false);
  template <typename F>
    requires(!std::is_same_v<std::decay_t<F>, task>)
  bool post(F &&fn, const bool over_capacity = false) {
    return post(task(std::forward<F>(fn)), over_capacity);
  }
  /// Waits until nothing is queued or running. A fiber waiting here suspends;
  /// any other thread blocks. Never from one of this queue's own tasks.
  void flush() noexcept;
  /// Stops taking tasks; the queued ones still run.
  void close() noexcept;
  [[nodiscard]] bool closed() const noexcept;
  /// Tasks posted and not yet finished.
  [[nodiscard]] usize pending() const noexcept;

private:
  void drain() noexcept;

  thread_pool &m_pool;
  config m_config;
  mutable fiber::mutex m_lock;
  fiber::cond_var m_idle;
  vector<task> m_tasks;
  /// Tasks the drain has taken and not yet finished.
  usize m_running = 0;
  /// A pool task is scheduled or running to drain the queue.
  bool m_draining = false;
  bool m_closed = false;
};

} // namespace nx
