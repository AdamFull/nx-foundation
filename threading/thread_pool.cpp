#include "core/foundation/threading/thread_pool.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/diagnostics/profiler.h"
#include "core/foundation/platform/os.h"

#include <exception>

namespace nx {

namespace detail {
namespace {
future_continuation *closed_continuations() noexcept {
  // Allocations are suitably aligned, so this value cannot be a real node.
  return reinterpret_cast<future_continuation *>(uintptr_t{1});
}
} // namespace

void future_state_finish(future_state_base *state, const u32 status) noexcept {
  assert(state != nullptr);
  assert((status >= 1 && status <= 3) && "invalid future completion status");

  state->ready.store(status, std::memory_order_release);
  state->done.decrement();

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

struct thread_pool::task_node {
  detail::pool_task task;
  thread_pool *pool = nullptr;
  bool pooled = true;
};

namespace {

constexpr u32 PRESEEDED_NODES = 128;

fiber::Priority lane(const task_priority priority) noexcept {
  switch (priority) {
  case task_priority::Critical:
    return fiber::Priority::High;
  case task_priority::Background:
    return fiber::Priority::Low;
  default:
    return fiber::Priority::Normal;
  }
}

template <typename T>
void update_peak(std::atomic<T> &peak, const T value) noexcept {
  T observed = peak.load(std::memory_order_relaxed);
  while (observed < value &&
         !peak.compare_exchange_weak(observed, value, std::memory_order_relaxed,
                                     std::memory_order_relaxed)) {
  }
}

} // namespace

thread_pool::thread_pool(const config &cfg) {
  m_collect_statistics = cfg.collect_statistics || cfg.collect_timings;
  m_collect_timings = cfg.collect_timings;

  u32 count = cfg.thread_count;
  if (count == 0) {
    const cpu::topology_info &topo = cpu::topology();
    count = topo.physical_count > 1 ? topo.physical_count - 1 : 1;
  }

  // Tasks used to run on 1 MiB OS thread stacks; fiber stacks only commit
  // what they touch, so matching that costs address space, not memory.
  m_scheduler = make_unique<fiber::Scheduler>(fiber::Scheduler::Config{
      .worker_count = count,
      .stack_size = 1024 * 1024,
      .name = cfg.name ? cfg.name : "nx-worker",
      .priority = cfg.priority,
      .adopt_main_thread = false,
      .blocking_threads = cfg.blocking_threads,
  });

  const u32 workers = worker_count();
  if (cfg.max_background_concurrency == 0)
    m_background_limit = workers > 1 ? workers - 1 : 1;
  else
    m_background_limit = nx::min(cfg.max_background_concurrency, workers);

  for (u32 i = 0; i < PRESEEDED_NODES; ++i)
    if (task_node *node = allocate<task_node>();
        !node || !m_free_nodes.try_push(node))
      release(node);
}

thread_pool::~thread_pool() {
  wait_idle();
  m_scheduler.reset();
  for (task_node *node = nullptr; m_free_nodes.try_pop(node);)
    release(node);
}

bool thread_pool::is_worker_thread() const noexcept {
  return m_scheduler->on_worker_thread();
}

bool thread_pool::is_execution_context() const noexcept {
  const auto *node = static_cast<const task_node *>(fiber::fiber_local());
  return node && node->pool == this;
}

thread_pool_statistics thread_pool::statistics() const noexcept {
  thread_pool_statistics out;
  out.workers = worker_count();
  out.background_limit = m_background_limit;
  out.background_active = m_background_active.load(std::memory_order_relaxed);
  out.enabled = m_collect_statistics;
  out.timings_enabled = m_collect_timings;
  if (!m_collect_statistics)
    return out;

  for (usize i = 0; i < task_priority_index(task_priority::Count); ++i) {
    const lane_counters &source = m_counters[i];
    task_lane_statistics &lane_stats = out.lanes[i];
    lane_stats.submitted = source.submitted.load(std::memory_order_relaxed);
    lane_stats.started = source.started.load(std::memory_order_relaxed);
    lane_stats.completed = source.completed.load(std::memory_order_relaxed);
    lane_stats.stolen = source.stolen.load(std::memory_order_relaxed);
    lane_stats.overflowed = source.overflowed.load(std::memory_order_relaxed);
    lane_stats.execution_ns =
        source.execution_ns.load(std::memory_order_relaxed);
    lane_stats.queued = source.queued.load(std::memory_order_relaxed);
    lane_stats.peak_queued = source.peak_queued.load(std::memory_order_relaxed);
    lane_stats.active = source.active.load(std::memory_order_relaxed);
    lane_stats.peak_active = source.peak_active.load(std::memory_order_relaxed);
  }
  return out;
}

void thread_pool::reset_statistics() noexcept {
  assert(m_unfinished.value() == 0 &&
         "thread-pool statistics may only be reset while idle");
  if (!m_collect_statistics)
    return;
  for (lane_counters &counters : m_counters) {
    counters.submitted.store(0, std::memory_order_relaxed);
    counters.started.store(0, std::memory_order_relaxed);
    counters.completed.store(0, std::memory_order_relaxed);
    counters.stolen.store(0, std::memory_order_relaxed);
    counters.overflowed.store(0, std::memory_order_relaxed);
    counters.execution_ns.store(0, std::memory_order_relaxed);
    counters.queued.store(0, std::memory_order_relaxed);
    counters.peak_queued.store(0, std::memory_order_relaxed);
    counters.active.store(0, std::memory_order_relaxed);
    counters.peak_active.store(0, std::memory_order_relaxed);
  }
}

void thread_pool::record_enqueued(const task_priority priority) noexcept {
  if (!m_collect_statistics)
    return;
  lane_counters &counters = m_counters[task_priority_index(priority)];
  counters.submitted.fetch_add(1, std::memory_order_relaxed);
  const u32 queued =
      counters.queued.fetch_add(1, std::memory_order_relaxed) + 1;
  update_peak(counters.peak_queued, queued);
}

void thread_pool::record_started(const task_priority priority) noexcept {
  if (!m_collect_statistics)
    return;
  lane_counters &counters = m_counters[task_priority_index(priority)];
  counters.queued.fetch_sub(1, std::memory_order_relaxed);
  counters.started.fetch_add(1, std::memory_order_relaxed);
  const u32 active =
      counters.active.fetch_add(1, std::memory_order_relaxed) + 1;
  update_peak(counters.peak_active, active);
}

void thread_pool::record_completed(const task_priority priority,
                                   const u64 execution_ns) noexcept {
  if (!m_collect_statistics)
    return;
  lane_counters &counters = m_counters[task_priority_index(priority)];
  counters.completed.fetch_add(1, std::memory_order_relaxed);
  counters.active.fetch_sub(1, std::memory_order_relaxed);
  if (m_collect_timings)
    counters.execution_ns.fetch_add(execution_ns, std::memory_order_relaxed);
}

void thread_pool::submit(task_group &group, detail::task_fn fn) {
  submit(group, task_priority::Normal, std::move(fn));
}

void thread_pool::submit(task_group &group, const task_priority priority,
                         detail::task_fn fn) {
  enqueue({std::move(fn), &group, priority});
}

thread_pool::task_node *thread_pool::acquire_node() noexcept {
  task_node *node = nullptr;
  if (m_free_nodes.try_pop(node))
    return node;
  return allocate<task_node>();
}

void thread_pool::release_node(task_node *node) noexcept {
  if (node->pooled && !m_free_nodes.try_push(node))
    release(node);
}

void thread_pool::enqueue(detail::pool_task &&t) {
  if (static_cast<usize>(t.priority) >=
      static_cast<usize>(task_priority::Count)) {
    assert(false && "invalid thread-pool task priority");
    t.priority = task_priority::Normal;
  }

  m_unfinished.add(1);
  if (t.group)
    t.group->m_pending.add(1);
  record_enqueued(t.priority);

  task_node *node = acquire_node();
  if (!node) {
    // Out of memory: run it here rather than lose it.
    task_node local{std::move(t), this, false};
    run_node(&local);
    return;
  }
  node->task = std::move(t);
  node->pool = this;
  node->task.holds_background_slot = false;

  if (node->task.priority != task_priority::Background) {
    dispatch(node);
    return;
  }
  // Background work nested in background work skips the limit: the outer task
  // holds a slot while it waits on the inner one.
  const auto *current = static_cast<const task_node *>(fiber::fiber_local());
  if (current && current->pool == this &&
      current->task.priority == task_priority::Background) {
    dispatch(node);
    return;
  }
  {
    scoped_lock guard(m_background_lock);
    m_background_waiting.push_back(node);
  }
  m_background_pending.fetch_add(1, std::memory_order_seq_cst);
  pump_background();
}

void thread_pool::dispatch(task_node *node) noexcept {
  const fiber::Job job{&thread_pool::run_node, node};
  m_scheduler->run({&job, 1}, nullptr, lane(node->task.priority));
}

// Pushes and slot releases both end here, and a pump that takes a slot but
// finds nothing re-checks, so a queued background task is never stranded.
void thread_pool::pump_background() noexcept {
  for (;;) {
    u32 active = m_background_active.load(std::memory_order_relaxed);
    do {
      if (active >= m_background_limit)
        return;
    } while (!m_background_active.compare_exchange_weak(
        active, active + 1, std::memory_order_seq_cst));

    task_node *node = nullptr;
    {
      scoped_lock guard(m_background_lock);
      if (m_background_head < m_background_waiting.size()) {
        node = m_background_waiting[m_background_head++];
        if (m_background_head == m_background_waiting.size()) {
          m_background_waiting.clear();
          m_background_head = 0;
        }
      }
    }
    if (node) {
      m_background_pending.fetch_sub(1, std::memory_order_seq_cst);
      node->task.holds_background_slot = true;
      dispatch(node);
      continue;
    }
    m_background_active.fetch_sub(1, std::memory_order_seq_cst);
    if (m_background_pending.load(std::memory_order_seq_cst) == 0)
      return;
  }
}

void thread_pool::run_node(void *arg) noexcept {
  auto *const node = static_cast<task_node *>(arg);
  thread_pool &pool = *node->pool;
  detail::pool_task &t = node->task;

  void *const outer = fiber::fiber_local();
  fiber::set_fiber_local(node);
  pool.record_started(t.priority);
  const u64 started_ns = pool.m_collect_timings ? os::ticks_ns() : 0;
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
    } catch (const task_cancelled &) {
      // Cancellation is an expected terminal state for cooperative work.
    } catch (const std::exception &e) {
      nx::loge("thread pool: unhandled task exception: {}", e.what());
    } catch (...) {
      nx::loge("thread pool: unhandled non-standard task exception");
    }
  }
  const u64 execution_ns =
      pool.m_collect_timings ? os::ticks_ns() - started_ns : 0;
  t.fn = {};
  pool.record_completed(t.priority, execution_ns);
  fiber::set_fiber_local(outer);

  task_group *const group = t.group;
  const bool held_slot = t.holds_background_slot;
  pool.release_node(node);
  if (held_slot) {
    pool.m_background_active.fetch_sub(1, std::memory_order_seq_cst);
    pool.pump_background();
  }
  if (group)
    group->m_pending.decrement();
  // Last: a waiter may destroy the pool as soon as this reaches zero.
  pool.m_unfinished.decrement();
}

void thread_pool::wait_idle() noexcept {
  assert(!is_execution_context() &&
         "wait_idle from inside a pool task can never finish; wait on a "
         "task_group instead");
  fiber::wait(m_unfinished);
}

} // namespace nx
