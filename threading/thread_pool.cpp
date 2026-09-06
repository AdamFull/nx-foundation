#include "core/foundation/threading/thread_pool.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/diagnostics/profiler.h"

#include <cstdio>
#include <exception>

namespace nx {

namespace {
struct pool_tls {
  thread_pool *pool = nullptr;
  u32 index = 0;
  u32 dispatches = 0;
  u32 steal_cursor = 0;
};
thread_local pool_tls t_worker;
} // namespace

namespace detail {
namespace {
future_continuation *closed_continuations() noexcept {
  // Allocations are suitably aligned, so this value cannot be a real node.
  return reinterpret_cast<future_continuation *>(uintptr_t{1});
}
} // namespace

void future_state_finish(future_state_base *state, const u32 status) noexcept {
  assert(state != nullptr);
  assert((status == 1 || status == 2) && "invalid future completion status");

  state->ready.store(status, std::memory_order_release);
  futex::wake_all(&state->ready);

  future_continuation *list = state->continuations.exchange(
      closed_continuations(), std::memory_order_acq_rel);

  // Registration is a lock-free stack. Reverse it so continuations attached in
  // source order are dispatched in that order as well.
  future_continuation *ordered = nullptr;
  while (list && list != closed_continuations()) {
    future_continuation *next = list->next;
    list->next = ordered;
    ordered = list;
    list = next;
  }

  while (ordered) {
    future_continuation *continuation = ordered;
    ordered = ordered->next;
    continuation->pool->submit(continuation->priority,
                               std::move(continuation->fn));
    release(continuation);
  }
}

void future_state_continue(future_state_base *state,
                           future_continuation *continuation) noexcept {
  assert(state != nullptr);
  assert(continuation != nullptr);
  assert(continuation->pool != nullptr);

  future_continuation *head =
      state->continuations.load(std::memory_order_acquire);
  for (;;) {
    if (head == closed_continuations()) {
      continuation->pool->submit(continuation->priority,
                                 std::move(continuation->fn));
      release(continuation);
      return;
    }

    continuation->next = head;
    if (state->continuations.compare_exchange_weak(head, continuation,
                                                   std::memory_order_release,
                                                   std::memory_order_acquire))
      return;
  }
}
} // namespace detail

void task_group::wait() noexcept {
  for (;;) {
    const u32 pending = m_pending.load(std::memory_order_acquire);
    if (pending == 0)
      return;
    if (m_pool && m_pool->help_execute_one())
      continue;
    futex::wait(&m_pending, pending, detail::WAIT_FOREVER);
  }
}

thread_pool::thread_pool(const config &cfg) {
  const cpu::topology_info &topo = cpu::topology();

  u32 count = cfg.thread_count;
  if (count == 0)
    count = topo.physical_count > 1 ? topo.physical_count - 1 : 1;

  // Create every worker before starting any thread: worker_loop iterates
  // m_workers for stealing and must see the final layout.
  m_workers.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    auto *w = allocate<worker>();
    if (!w)
      break;
    w->index = i;
    w->steal_cursor = i + 1;
    snprintf(w->name, sizeof(w->name), "%s-%u", cfg.name ? cfg.name : "nx", i);
    m_workers.push_back(w);
  }

  bool start_failed = false;
  for (worker *w : m_workers) {
    thread_options opts;
    opts.name = w->name;
    opts.priority = cfg.priority;
    if (cfg.pin_threads && !topo.cores.empty()) {
      const cpu::core_info &core = topo.cores[w->index % topo.cores.size()];
      opts.affinity_mask = core.logical_mask;
    }
    w->th = thread([this, w] { worker_loop(*w); }, opts);
    if (!w->th.joinable()) {
      start_failed = true;
      break;
    }
  }

  if (start_failed) {
    m_stop.store(true, std::memory_order_release);
    m_signal.fetch_add(1, std::memory_order_seq_cst);
    futex::wake_all(&m_signal);
    for (worker *w : m_workers)
      if (w->th.joinable())
        w->th.join();
    for (worker *w : m_workers)
      release(w);
    m_workers.clear();
    m_stop.store(false, std::memory_order_relaxed);
  }
}

thread_pool::~thread_pool() {
  wait_idle();

  m_stop.store(true, std::memory_order_release);
  m_signal.fetch_add(1, std::memory_order_seq_cst);
  futex::wake_all(&m_signal);

  for (worker *w : m_workers)
    if (w->th.joinable())
      w->th.join();
  for (worker *w : m_workers)
    release(w);
  m_workers.clear();
}

bool thread_pool::is_worker_thread() const noexcept {
  return t_worker.pool == this;
}

void thread_pool::submit(task_group &group, detail::task_fn fn) {
  submit(group, task_priority::Normal, std::move(fn));
}

void thread_pool::submit(task_group &group, const task_priority priority,
                         detail::task_fn fn) {
  assert((group.m_pool == nullptr || group.m_pool == this) &&
         "task_group is already bound to a different pool");
  group.m_pool = this;
  enqueue({std::move(fn), &group, priority});
}

void thread_pool::enqueue(detail::pool_task &&t) {
  m_unfinished.fetch_add(1, std::memory_order_relaxed);
  if (t.group)
    t.group->m_pending.fetch_add(1, std::memory_order_relaxed);

  // Construction can legitimately end with no workers when the platform
  // refuses a thread or the worker allocation fails. Running inline is slower,
  // but it preserves async progress instead of leaving futures pending forever.
  if (m_workers.empty()) {
    run_task(t);
    return;
  }

  worker *self = t_worker.pool == this ? m_workers[t_worker.index] : nullptr;
  usize priority = static_cast<usize>(t.priority);
  if (priority >= static_cast<usize>(task_priority::Count)) {
    assert(false && "invalid thread-pool task priority");
    t.priority = task_priority::Normal;
    priority = static_cast<usize>(t.priority);
  }
  if (!(self && self->queues[priority].try_push(std::move(t))) &&
      !m_shared_queues[priority].try_push(std::move(t))) {
    overflow_queue &overflow = m_overflow[priority];
    scoped_lock lock(overflow.lock);
    overflow.tasks.push_back(std::move(t));
    overflow.count.fetch_add(1, std::memory_order_release);
  }

  m_signal.fetch_add(1, std::memory_order_seq_cst);
  if (m_sleepers.load(std::memory_order_seq_cst) > 0)
    futex::wake(&m_signal, 1);
}

bool thread_pool::try_get_priority(detail::pool_task &out, const u32 self_index,
                                   const task_priority priority,
                                   u32 &steal_cursor) noexcept {
  const usize p = static_cast<usize>(priority);
  if (self_index != INVALID_WORKER &&
      m_workers[self_index]->queues[p].try_pop(out))
    return true;

  if (m_shared_queues[p].try_pop(out))
    return true;

  overflow_queue &overflow = m_overflow[p];
  if (overflow.count.load(std::memory_order_acquire) > 0) {
    scoped_lock lock(overflow.lock);
    if (overflow.head < overflow.tasks.size()) {
      out = std::move(overflow.tasks[overflow.head++]);
      overflow.count.fetch_sub(1, std::memory_order_relaxed);
      if (overflow.head == overflow.tasks.size()) {
        overflow.tasks.clear();
        overflow.head = 0;
      }
      return true;
    }
  }

  const u32 n = nx::cast<u32>(m_workers.size());
  if (n != 0) {
    const u32 start = steal_cursor++;
    for (u32 i = 0; i < n; ++i) {
      const u32 victim = (start + i) % n;
      if (victim == self_index)
        continue;
      if (m_workers[victim]->queues[p].try_pop(out))
        return true;
    }
  }

  return false;
}

bool thread_pool::try_get_task(detail::pool_task &out, const u32 self_index,
                               u32 &dispatches, u32 &steal_cursor) noexcept {
  task_priority order[3];
  // Priority is strict most of the time, with deterministic aging probes so a
  // sustained stream of critical work cannot permanently starve normal or
  // background maintenance.
  if ((dispatches & 31u) == 31u) {
    order[0] = task_priority::Background;
    order[1] = task_priority::Critical;
    order[2] = task_priority::Normal;
  } else if ((dispatches & 7u) == 7u) {
    order[0] = task_priority::Normal;
    order[1] = task_priority::Critical;
    order[2] = task_priority::Background;
  } else {
    order[0] = task_priority::Critical;
    order[1] = task_priority::Normal;
    order[2] = task_priority::Background;
  }
  for (const task_priority priority : order)
    if (try_get_priority(out, self_index, priority, steal_cursor)) {
      ++dispatches;
      return true;
    }
  return false;
}

void thread_pool::run_task(detail::pool_task &t) noexcept {
  if (t.fn) {
    try {
      switch (t.priority) {
      case task_priority::Critical: {
        NX_PROFILE_ZONE_COLOR("thread_pool::critical", 0xd05050);
        t.fn();
        break;
      }
      case task_priority::Background: {
        NX_PROFILE_ZONE_COLOR("thread_pool::background", 0x5070a0);
        t.fn();
        break;
      }
      default: {
        NX_PROFILE_ZONE("thread_pool::normal");
        t.fn();
        break;
      }
      }
    } catch (const std::exception &e) {
      nx::loge("thread pool: unhandled task exception: {}", e.what());
    } catch (...) {
      nx::loge("thread pool: unhandled non-standard task exception");
    }
  }

  t.fn = {};

  if (task_group *group = t.group) {
    t.group = nullptr;
    if (group->m_pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
      futex::wake_all(&group->m_pending);
  }
  if (m_unfinished.fetch_sub(1, std::memory_order_acq_rel) == 1)
    futex::wake_all(&m_unfinished);
}

bool thread_pool::help_execute_one() noexcept {
  const bool own_worker = t_worker.pool == this;
  const u32 self = own_worker ? t_worker.index : INVALID_WORKER;
  detail::pool_task t;
  u32 &dispatches =
      own_worker ? m_workers[self]->dispatches : t_worker.dispatches;
  u32 &steal_cursor =
      own_worker ? m_workers[self]->steal_cursor : t_worker.steal_cursor;
  if (!try_get_task(t, self, dispatches, steal_cursor))
    return false;
  run_task(t);
  return true;
}

void thread_pool::worker_loop(worker &w) noexcept {
  t_worker.pool = this;
  t_worker.index = w.index;
  t_worker.dispatches = 0;
  t_worker.steal_cursor = w.steal_cursor;
  NX_PROFILE_SET_THREAD_NAME(w.name);

  detail::pool_task t;
  for (;;) {
    const u32 signal = m_signal.load(std::memory_order_acquire);
    if (try_get_task(t, w.index, w.dispatches, w.steal_cursor)) {
      run_task(t);
      continue;
    }
    if (m_stop.load(std::memory_order_acquire))
      break;

    m_sleepers.fetch_add(1, std::memory_order_seq_cst);
    futex::wait(&m_signal, signal, detail::WAIT_FOREVER);
    m_sleepers.fetch_sub(1, std::memory_order_relaxed);
  }

  t_worker.pool = nullptr;
  t_worker.index = 0;
  t_worker.dispatches = 0;
  t_worker.steal_cursor = 0;
}

void thread_pool::wait_idle() noexcept {
  assert(!is_worker_thread() &&
         "wait_idle from inside a pool task can never finish; wait on a "
         "task_group instead");
  for (;;) {
    const u32 unfinished = m_unfinished.load(std::memory_order_acquire);
    if (unfinished == 0)
      return;
    if (help_execute_one())
      continue;
    futex::wait(&m_unfinished, unfinished, detail::WAIT_FOREVER);
  }
}

}
