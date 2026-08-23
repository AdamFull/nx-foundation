#include "core/foundation/threading/thread_pool.h"

#include "core/foundation/diagnostics/profiler.h"

#include <cstdio>

namespace nx {

namespace {
struct pool_tls {
  thread_pool *pool = nullptr;
  u32 index = 0;
};
thread_local pool_tls t_worker;
}

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

void thread_pool::submit(task_group &group, function<void()> fn) {
  assert((group.m_pool == nullptr || group.m_pool == this) &&
         "task_group is already bound to a different pool");
  group.m_pool = this;
  enqueue({std::move(fn), &group});
}

void thread_pool::enqueue(detail::pool_task &&t) {
  m_unfinished.fetch_add(1, std::memory_order_relaxed);
  if (t.group)
    t.group->m_pending.fetch_add(1, std::memory_order_relaxed);

  worker *self = t_worker.pool == this ? m_workers[t_worker.index] : nullptr;
  if (!(self && self->queue.try_push(std::move(t))) &&
      !m_shared_queue.try_push(std::move(t))) {
    scoped_lock lock(m_overflow_mutex);
    m_overflow.push_back(std::move(t));
    m_overflow_count.fetch_add(1, std::memory_order_release);
  }

  m_signal.fetch_add(1, std::memory_order_seq_cst);
  if (m_sleepers.load(std::memory_order_seq_cst) > 0)
    futex::wake(&m_signal, 1);
}

bool thread_pool::try_get_task(detail::pool_task &out,
                               const u32 self_index) noexcept {
  if (self_index != INVALID_WORKER && m_workers[self_index]->queue.try_pop(out))
    return true;

  if (m_shared_queue.try_pop(out))
    return true;

  const u32 n = nx::cast<u32>(m_workers.size());
  if (n != 0) {
    const u32 start = m_steal_seed.fetch_add(1, std::memory_order_relaxed);
    for (u32 i = 0; i < n; ++i) {
      const u32 victim = (start + i) % n;
      if (victim == self_index)
        continue;
      if (m_workers[victim]->queue.try_pop(out))
        return true;
    }
  }

  if (m_overflow_count.load(std::memory_order_acquire) > 0) {
    scoped_lock lock(m_overflow_mutex);
    if (m_overflow_head < m_overflow.size()) {
      out = std::move(m_overflow[m_overflow_head++]);
      m_overflow_count.fetch_sub(1, std::memory_order_relaxed);
      if (m_overflow_head == m_overflow.size()) {
        m_overflow.clear();
        m_overflow_head = 0;
      }
      return true;
    }
  }
  return false;
}

void thread_pool::run_task(detail::pool_task &t) noexcept {
  if (t.fn)
    t.fn();

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
  const u32 self = t_worker.pool == this ? t_worker.index : INVALID_WORKER;
  detail::pool_task t;
  if (!try_get_task(t, self))
    return false;
  run_task(t);
  return true;
}

void thread_pool::worker_loop(worker &w) noexcept {
  t_worker.pool = this;
  t_worker.index = w.index;
  NX_PROFILE_SET_THREAD_NAME(w.name);

  detail::pool_task t;
  for (;;) {
    const u32 signal = m_signal.load(std::memory_order_acquire);
    if (try_get_task(t, w.index)) {
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
