#include "core/foundation/fibers/scheduler.h"

#include "core/foundation/containers/concurrent_queue.h"
#include "core/foundation/containers/fixed_vector.h"
#include "core/foundation/containers/small_vector.h"
#include "core/foundation/diagnostics/diagnostic.h"
#include "core/foundation/diagnostics/log.h"
#include "core/foundation/diagnostics/profiler.h"
#include "core/foundation/fibers/context.h"
#include "core/foundation/fibers/stack.h"
#include "core/foundation/fibers/wait.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/threading/thread.h"

#include <cstring>
#include <limits>
#include <optional>

namespace nx::fiber {

namespace {

const log::Category log_fiber = log::category("fiber");

using clock = std::chrono::steady_clock;

constexpr usize LOCAL_QUEUE = 256;
constexpr usize GLOBAL_QUEUE = 4096;
constexpr usize MAIN_QUEUE = 1024;
constexpr usize FIBER_QUEUE = 4096;
constexpr u32 IDLE_SPINS = 64;
constexpr i64 NO_TIMER = std::numeric_limits<i64>::max();

i64 ticks(const clock::time_point time) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             time.time_since_epoch())
      .count();
}

} // namespace

namespace internal {
WaitResult block_thread(Waiter &waiter, spin_lock *lock, WaitList *list,
                        clock::time_point deadline) noexcept;
}

enum class Affinity : u8 { Any, Main, Blocking };
enum class Role : u8 { Main, General, Blocking };
enum class After : u8 { Nothing, Release, Ready, Yield, Park, Enqueue };

struct Fiber {
  fiber::context context;
  fiber::stack stack;
  SchedulerState *owner = nullptr;
  Fiber *next_parked = nullptr;
  void *local[nx::cast<usize>(local_slot::Count)] = {};
#if NX_PROFILE_ENABLED
  const char *profile_name = nullptr;
#endif
  internal::Waiter waiter;
  u32 target = 0;
  fiber::priority priority = fiber::priority::Normal;
  Affinity affinity = Affinity::Any;
};

// Either a job to start or a yielded fiber to resume.
struct Work {
  fiber::job job;
  fiber::counter *counter = nullptr;
  Fiber *fiber = nullptr;
  fiber::priority priority = fiber::priority::Normal;
  Affinity affinity = Affinity::Any;
};

struct Handoff {
  After after = After::Nothing;
  Fiber *subject = nullptr;
  Fiber *to = nullptr;
  fiber::counter *counter = nullptr;
  spin_lock *lock = nullptr;
  internal::WaitList *list = nullptr;
};

struct Worker {
  SchedulerState *state = nullptr;
  usize index = 0;
  Role role = Role::General;
  Fiber thread_fiber;
  Fiber *current = nullptr;
  u32 dispatches = 0;
  char name[32] = {};
#if NX_PROFILE_ENABLED && defined(TRACY_FIBERS) && defined(TRACY_ON_DEMAND)
  // The profiler connection that has seen which fiber this thread is in.
  std::optional<u64> profiled_connection;
#endif
  mpmc::queue<Work, LOCAL_QUEUE> local[PRIORITY_COUNT];
  thread os_thread;
  std::atomic<u64> os_id{0};
};

namespace {

thread_local Worker *t_worker = nullptr;

// Fibers migrate between threads, so the thread_local must be re-read after
// every switch; a non-inlined accessor stops the compiler caching its address.
NX_NO_INLINE Worker *current_worker() noexcept { return t_worker; }
NX_NO_INLINE void set_current_worker(Worker *worker) noexcept {
  t_worker = worker;
}

} // namespace

struct SchedulerState {
  explicit SchedulerState(const scheduler::config &requested) noexcept;
  ~SchedulerState() noexcept;

  static void settle(const fiber::counter &counter) noexcept;

  Worker *this_worker() const noexcept {
    Worker *worker = current_worker();
    return worker && worker->state == this ? worker : nullptr;
  }

  Fiber *acquire_fiber() noexcept;
  Fiber *next_to_resume(Worker *worker) noexcept;
  bool pop_ready(Worker *worker, Fiber *&fiber) noexcept;
  bool pop_work(Worker *worker, usize priority, Work &work) noexcept;
  bool pop_any_work(Worker *worker, Work &work) noexcept;
  bool has_general_work() const noexcept;

  void submit(std::span<const job> jobs, fiber::counter *counter,
              fiber::priority priority, Affinity affinity) noexcept;
  void push_fiber_work(Fiber *fiber) noexcept;
  void make_ready(Fiber *fiber) noexcept;
  void park(Fiber *fiber, fiber::counter &counter) noexcept;
  void execute(const Work &work) noexcept;

  void switch_fiber(Fiber *from, Fiber *to, After after,
                    fiber::counter *counter = nullptr) noexcept;
  void switch_with(Handoff out) noexcept;
  void complete(Handoff handoff) noexcept;
  void suspend(After after) noexcept;
  void suspend_until(Worker *worker, fiber::counter &counter,
                     u32 target) noexcept;
  bool serve_main(function_ref<bool()> done,
                  clock::time_point deadline) noexcept;
  void wait_on_main(fiber::counter &counter, u32 target) noexcept;
  internal::WaitResult block_on_main(Worker *main, spin_lock *lock,
                                     internal::WaitList *list,
                                     clock::time_point deadline) noexcept;
  void help_until(fiber::counter &counter, u32 target) noexcept;
  [[noreturn]] void loop(Fiber *self) noexcept;
  Fiber *next_work(Worker *worker, Work &work) noexcept;

  void assert_may_suspend() const noexcept;
  internal::WaitResult block(Worker *worker, spin_lock *lock,
                             internal::WaitList *list,
                             clock::time_point deadline) noexcept;

  void add_timer(Fiber *fiber) noexcept;
  void remove_timer(Fiber *fiber) noexcept;
  void remove_timer_at(u32 index) noexcept;
  void sift_timer(u32 index) noexcept;
  void publish_next_deadline() noexcept;
  void fire_timers() noexcept;

  void wake_workers(usize count) noexcept;
  void wake_keeper() noexcept;
  void park_worker() noexcept;
  void wake_main() noexcept;
  void park_main(function_ref<bool()> done,
                 clock::time_point deadline) noexcept;
  void wake_blocking() noexcept;
  void park_blocking() noexcept;
  void worker_main(Worker *worker) noexcept;

  scheduler::config config;
  stack_pool stacks;
  small_vector<Worker *, 32> workers;
  Fiber **fibers = nullptr;
  std::atomic<u32> fiber_count{0};

  mpmc::queue<Fiber *, FIBER_QUEUE> free_fibers;
  mpmc::queue<Fiber *, FIBER_QUEUE> ready[PRIORITY_COUNT];
  mpmc::queue<Work, GLOBAL_QUEUE> global[PRIORITY_COUNT];
  mpmc::queue<Work, MAIN_QUEUE> main_work;
  mpmc::queue<Fiber *, FIBER_QUEUE> blocking_ready;

  std::atomic<u32> worker_epoch{0};
  std::atomic<u32> sleepers{0};
  // At most one parked worker keeps the next timer's deadline; the rest park
  // on worker_epoch with no timeout.
  thread_parker keeper_parker;
  std::atomic<u32> keeper{0};
  thread_parker main_parker;
  std::atomic<u32> main_sleeping{0};
  std::atomic<u32> blocking_epoch{0};
  std::atomic<u32> blocking_sleepers{0};
  std::atomic<u32> live_jobs{0};
  std::atomic<u32> limit_hits{0};
  std::atomic<u64> jobs_run{0};
  std::atomic<bool> stopping{false};

  spin_lock timer_lock;
  Fiber **timers = nullptr;
  u32 timer_count = 0;
  std::atomic<i64> next_deadline{NO_TIMER};
};

namespace {

[[noreturn]] void fiber_entry(fiber::transfer transfer) {
  const Handoff handoff = *static_cast<const Handoff *>(transfer.data);
  SchedulerState *state = handoff.to->owner;
  state->complete(handoff);
  state->loop(handoff.to);
}

#if NX_PROFILE_ENABLED
// Tracy keeps the pointer for the rest of the process, so names are never
// freed, and are numbered process-wide so two schedulers never share one.
const char *profile_fiber_name() noexcept {
  static std::atomic<u32> next{0};
  char buffer[24];
  format_to(buffer, "fiber {}", next.fetch_add(1, std::memory_order_relaxed));
  const usize length = std::strlen(buffer) + 1;
  auto *name = static_cast<char *>(mem_alloc(length, 1));
  if (name)
    std::memcpy(name, buffer, length);
  return name;
}

#if defined(TRACY_FIBERS) && defined(TRACY_ON_DEMAND)
// A viewer only sees switches made while it is connected. Connected is read
// before the connection id, which Tracy advances first, so the pair agrees; a
// leave is only sent to a connection that saw this thread enter a fiber.
void profile_switch(Worker &worker, const Fiber &to) noexcept {
  const tracy::Profiler &profiler = tracy::GetProfiler();
  if (!profiler.IsConnected())
    return;
  const u64 connection = profiler.ConnectionId();
  if (to.profile_name)
    NX_PROFILE_FIBER_ENTER(to.profile_name);
  else if (worker.profiled_connection == connection)
    NX_PROFILE_FIBER_LEAVE();
  worker.profiled_connection = connection;
}
#else
void profile_switch(Worker &, const Fiber &to) noexcept {
  if (to.profile_name)
    NX_PROFILE_FIBER_ENTER(to.profile_name);
  else
    NX_PROFILE_FIBER_LEAVE();
}
#endif
#endif

u32 resolve_worker_count(u32 requested) noexcept {
  if (requested)
    return requested;
  const u32 logical = cpu::logical_core_count();
  return logical > 2 ? logical - 1 : 1;
}

} // namespace

// A decrement may still be touching the counter after its waiter has been
// released; the waiter must not return, and possibly destroy it, before then.
void SchedulerState::settle(const fiber::counter &counter) noexcept {
  while (counter.m_decrementing.load(std::memory_order_acquire) != 0)
    detail::cpu_relax();
}

counter::~counter() noexcept {
  NX_ASSERT(m_parked == nullptr, "a counter was destroyed with fibers waiting");
  SchedulerState::settle(*this);
}

void counter::decrement() noexcept {
  m_decrementing.fetch_add(1, std::memory_order_seq_cst);
  const u32 previous = m_value.fetch_sub(1, std::memory_order_seq_cst);
  NX_ASSERT(previous > 0, "a counter was decremented below zero");
  const u32 value = previous - 1;
  if (m_waiters.load(std::memory_order_seq_cst) != 0) {
    Fiber *woken = nullptr;
    {
      scoped_lock guard(m_lock);
      Fiber **link = &m_parked;
      while (Fiber *fiber = *link) {
        if (value <= fiber->target) {
          *link = fiber->next_parked;
          fiber->next_parked = woken;
          woken = fiber;
          m_waiters.fetch_sub(1, std::memory_order_relaxed);
        } else {
          link = &fiber->next_parked;
        }
      }
    }
    while (woken) {
      Fiber *next = woken->next_parked;
      woken->owner->make_ready(woken);
      woken = next;
    }
    futex::wake_all(&m_value);
  }
  m_decrementing.fetch_sub(1, std::memory_order_release);
}

SchedulerState::SchedulerState(const scheduler::config &requested) noexcept
    : config(requested), stacks({.stack_size = requested.stack_size,
                                 .initial_commit = requested.stack_commit,
                                 .max_cached = requested.max_fibers}) {
  const u32 worker_count = resolve_worker_count(config.worker_count);
  config.worker_count = worker_count;
  const u32 threads = worker_count + config.blocking_threads;
  config.max_fibers = clamp<u32>(config.max_fibers, threads + 2,
                                 nx::cast<u32>(FIBER_QUEUE) - threads - 1);
  fibers = static_cast<Fiber **>(
      mem_alloc(sizeof(Fiber *) * config.max_fibers, alignof(Fiber *)));
  std::memset(fibers, 0, sizeof(Fiber *) * config.max_fibers);
  timers = static_cast<Fiber **>(mem_alloc(
      sizeof(Fiber *) * (config.max_fibers + threads + 1), alignof(Fiber *)));

  const char *name = config.name ? config.name : "nx-fiber";
  for (u32 i = 0; i <= threads; ++i) {
    Worker *worker = allocate<Worker>();
    worker->state = this;
    worker->index = i;
    worker->role = i == 0              ? Role::Main
                   : i <= worker_count ? Role::General
                                       : Role::Blocking;
    worker->thread_fiber.owner = this;
    worker->thread_fiber.affinity = i == 0              ? Affinity::Main
                                    : i <= worker_count ? Affinity::Any
                                                        : Affinity::Blocking;
    worker->current = &worker->thread_fiber;
    if (worker->role == Role::Blocking)
      format_to(worker->name, "{}-blocking-{}", name, i - worker_count - 1);
    else
      format_to(worker->name, "{}-{}", name, i);
    workers.push_back(worker);
  }
}

SchedulerState::~SchedulerState() noexcept {
  u32 free_count = 0;
  for (Fiber *fiber = nullptr; free_fibers.try_pop(fiber);)
    ++free_count;
  u32 created = 0;
  for (u32 i = 0; i < config.max_fibers; ++i) {
    if (Fiber *fiber = fibers[i]) {
      ++created;
      stacks.release(fiber->stack);
      release(fiber);
    }
  }
  NX_ASSERT(free_count == created, "{} fibers were still suspended",
            created - free_count);
  mem_free(fibers);
  mem_free(timers);
  for (Worker *worker : workers)
    release(worker);
}

Fiber *SchedulerState::acquire_fiber() noexcept {
  Fiber *fiber = nullptr;
  if (free_fibers.try_pop(fiber))
    return fiber;

  if (fiber_count.load(std::memory_order_relaxed) >= config.max_fibers)
    return nullptr;
  // The slot is claimed last, so a stack or allocation failure leaves no
  // index claimed and never filled.
  auto stack = stacks.acquire();
  if (!stack) {
    logw(log_fiber, "fiber: out of address space for fiber stacks");
    return nullptr;
  }
  fiber = allocate<Fiber>();
  if (!fiber) {
    stacks.release(*stack);
    return nullptr;
  }
  u32 index = fiber_count.load(std::memory_order_relaxed);
  do {
    if (index >= config.max_fibers) {
      release(fiber);
      stacks.release(*stack);
      return nullptr;
    }
  } while (!fiber_count.compare_exchange_weak(index, index + 1,
                                              std::memory_order_relaxed));
  fiber->stack = *stack;
  fiber->owner = this;
  fiber->context.reset(fiber->stack, &fiber_entry);
#if NX_PROFILE_ENABLED
  fiber->profile_name = profile_fiber_name();
#endif
  fibers[index] = fiber;
  return fiber;
}

bool SchedulerState::pop_ready(Worker *worker, Fiber *&fiber) noexcept {
  if (worker->role == Role::Blocking)
    return blocking_ready.try_pop(fiber);
  for (auto &queue : ready)
    if (queue.try_pop(fiber))
      return true;
  return false;
}

Fiber *SchedulerState::next_to_resume(Worker *worker) noexcept {
  if (Fiber *fiber = acquire_fiber())
    return fiber;
  Fiber *fiber = nullptr;
  return pop_ready(worker, fiber) ? fiber : nullptr;
}

bool SchedulerState::pop_work(Worker *worker, const usize priority,
                              Work &work) noexcept {
  if (worker->local[priority].try_pop(work) || global[priority].try_pop(work))
    return true;
  const usize count = workers.size();
  for (usize i = 1; i < count; ++i) {
    Worker *victim = workers[(worker->index + i) % count];
    if (victim->role == Role::General && victim->local[priority].try_pop(work))
      return true;
  }
  return false;
}

bool SchedulerState::pop_any_work(Worker *worker, Work &work) noexcept {
  if (worker->role == Role::Main)
    return main_work.try_pop(work);
  if (worker->role == Role::Blocking)
    return false;
  for (usize priority = 0; priority < PRIORITY_COUNT; ++priority)
    if (pop_work(worker, priority, work))
      return true;
  return false;
}

// Only what can be popped: a slot whose push was preempted between claiming
// and filling it counts in size_approx, and idle workers that stayed awake for
// it spun on every core while the preempted push waited for one.
bool SchedulerState::has_general_work() const noexcept {
  for (usize priority = 0; priority < PRIORITY_COUNT; ++priority) {
    if (ready[priority].can_pop_approx() || global[priority].can_pop_approx())
      return true;
    for (const Worker *worker : workers)
      if (worker->local[priority].can_pop_approx())
        return true;
  }
  return false;
}

void SchedulerState::submit(std::span<const job> jobs, fiber::counter *counter,
                            const fiber::priority priority,
                            const Affinity affinity) noexcept {
  if (jobs.empty())
    return;
  if (counter)
    counter->add(nx::cast<u32>(jobs.size()));
  live_jobs.fetch_add(nx::cast<u32>(jobs.size()), std::memory_order_relaxed);

  const auto lane = nx::cast<usize>(priority);
  usize queued = 0;
  for (const fiber::job &job : jobs) {
    const Work work{job, counter, nullptr, priority, affinity};
    if (affinity == Affinity::Main) {
      while (!main_work.try_push(work)) {
        wake_main();
        Worker *worker = this_worker();
        if (worker && worker->role == Role::Main) {
          execute(work);
          break;
        }
        this_thread::yield();
      }
      continue;
    }
    // Re-read per job: running an overflowing job inline may migrate us.
    Worker *worker = this_worker();
    if ((worker && worker->role == Role::General &&
         worker->local[lane].try_push(work)) ||
        global[lane].try_push(work)) {
      ++queued;
      continue;
    }
    wake_workers(queued);
    queued = 0;
    execute(work);
  }
  if (affinity == Affinity::Main)
    wake_main();
  else
    wake_workers(queued);
}

void SchedulerState::push_fiber_work(Fiber *fiber) noexcept {
  Work work;
  work.fiber = fiber;
  work.priority = fiber->priority;
  if (fiber->affinity == Affinity::Blocking) {
    make_ready(fiber);
    return;
  }
  NX_ASSERT(fiber->affinity != Affinity::Main,
            "the main thread runs no fibers");
  // Behind everything already queued, or a yield loop would starve the work
  // it is waiting for. A full lane may have no other worker to drain it, so
  // the fiber goes to the ready queue, which holds every fiber, rather than
  // spinning here.
  if (!global[nx::cast<usize>(fiber->priority)].try_push(work)) {
    make_ready(fiber);
    return;
  }
  wake_workers(1);
}

void SchedulerState::make_ready(Fiber *fiber) noexcept {
  if (fiber->affinity == Affinity::Blocking) {
    while (!blocking_ready.try_push(fiber))
      detail::cpu_relax();
    wake_blocking();
    return;
  }
  // The main thread's own fiber, or a wait token on its stack: its wait
  // re-checks what it waits for once it wakes.
  if (fiber->affinity == Affinity::Main) {
    wake_main();
    return;
  }
  while (!ready[nx::cast<usize>(fiber->priority)].try_push(fiber))
    detail::cpu_relax();
  wake_workers(1);
}

void SchedulerState::park(Fiber *fiber, fiber::counter &counter) noexcept {
  {
    scoped_lock guard(counter.m_lock);
    counter.m_waiters.fetch_add(1, std::memory_order_seq_cst);
    if (counter.m_value.load(std::memory_order_seq_cst) > fiber->target) {
      fiber->next_parked = counter.m_parked;
      counter.m_parked = fiber;
      return;
    }
    counter.m_waiters.fetch_sub(1, std::memory_order_relaxed);
  }
  make_ready(fiber);
}

void SchedulerState::execute(const Work &work) noexcept {
  work.job.entry(work.job.arg);
  if (work.counter)
    work.counter->decrement();
  jobs_run.fetch_add(1, std::memory_order_relaxed);
  live_jobs.fetch_sub(1, std::memory_order_release);
}

void SchedulerState::switch_fiber(Fiber *from, Fiber *to, const After after,
                                  fiber::counter *counter) noexcept {
  switch_with(Handoff{after, from, to, counter});
}

void SchedulerState::switch_with(Handoff out) noexcept {
  const transfer in = switch_to(out.subject->context, out.to->context, &out);
  complete(*static_cast<const Handoff *>(in.data));
}

// Runs on the fiber that was just resumed, once the one that suspended is off
// its stack; only then is it safe to let another thread resume that one.
void SchedulerState::complete(const Handoff handoff) noexcept {
  current_worker()->current = handoff.to;
#if NX_PROFILE_ENABLED
  profile_switch(*current_worker(), *handoff.to);
#endif
  switch (handoff.after) {
  case After::Nothing:
    break;
  case After::Release:
    while (!free_fibers.try_push(handoff.subject))
      detail::cpu_relax();
    break;
  case After::Ready:
    make_ready(handoff.subject);
    break;
  case After::Yield:
    push_fiber_work(handoff.subject);
    break;
  case After::Park:
    park(handoff.subject, *handoff.counter);
    break;
  case After::Enqueue:
    if (handoff.list)
      handoff.list->push(&handoff.subject->waiter);
    if (handoff.subject->waiter.deadline != internal::NO_DEADLINE)
      add_timer(handoff.subject);
    if (handoff.lock)
      handoff.lock->unlock();
    break;
  }
}

void SchedulerState::suspend(const After after) noexcept {
  assert_may_suspend();
  for (;;) {
    Worker *worker = this_worker();
    Fiber *self = worker->current;
    if (Fiber *next = next_to_resume(worker)) {
      switch_fiber(self, next, after);
      return;
    }
    Work work;
    if (pop_any_work(worker, work)) {
      if (work.fiber) {
        switch_fiber(self, work.fiber, after);
        return;
      }
      execute(work);
    } else if (after == After::Yield) {
      return;
    } else {
      detail::cpu_relax();
    }
  }
}

void SchedulerState::suspend_until(Worker *worker, fiber::counter &counter,
                                   const u32 target) noexcept {
  assert_may_suspend();
  if (worker->role == Role::Main) {
    wait_on_main(counter, target);
    return;
  }
  Fiber *self = worker->current;
  self->target = target;
  Fiber *next = next_to_resume(worker);
  if (!next) {
    help_until(counter, target);
    return;
  }
  switch_fiber(self, next, After::Park, &counter);
}

// Only reached when every fiber is in use: rather than deadlock, the waiting
// fiber runs queued jobs on its own stack.
void SchedulerState::help_until(fiber::counter &counter,
                                const u32 target) noexcept {
  if (limit_hits.fetch_add(1, std::memory_order_relaxed) == 0)
    logw(log_fiber,
         "fiber: all {} fibers are in use; waits now run jobs "
         "nested on the waiting fiber",
         config.max_fibers);
  while (counter.value() > target) {
    Worker *worker = this_worker();
    Fiber *other = nullptr;
    if (pop_ready(worker, other)) {
      switch_fiber(worker->current, other, After::Ready);
      continue;
    }
    Work work;
    if (!pop_any_work(worker, work)) {
      detail::cpu_relax();
      continue;
    }
    if (work.fiber)
      switch_fiber(worker->current, work.fiber, After::Yield);
    else
      execute(work);
  }
}

void SchedulerState::loop(Fiber *self) noexcept {
  for (;;) {
    Work work;
    if (Fiber *next = next_work(current_worker(), work)) {
      switch_fiber(self, next, After::Release);
      continue;
    }
    self->priority = work.priority;
    self->affinity = work.affinity;
    for (void *&slot : self->local)
      slot = nullptr;
    execute(work);
  }
}

Fiber *SchedulerState::next_work(Worker *worker, Work &work) noexcept {
  for (u32 idle = 0;; ++idle) {
    Fiber *fiber = nullptr;
    if (worker->role == Role::Blocking) {
      if (blocking_ready.try_pop(fiber))
        return fiber;
      if (stopping.load(std::memory_order_acquire))
        return &worker->thread_fiber;
      if (idle < IDLE_SPINS)
        detail::cpu_relax();
      else
        park_blocking();
      continue;
    }
    fire_timers();
    // Strict priority, except that every 8th and 32nd pick probes a lower
    // lane first, so sustained high-priority load cannot starve the rest.
    static constexpr usize ORDERS[3][PRIORITY_COUNT] = {
        {0, 1, 2}, {1, 0, 2}, {2, 0, 1}};
    const u32 pick = worker->dispatches;
    const usize *order = (pick & 31u) == 31u ? ORDERS[2]
                         : (pick & 7u) == 7u ? ORDERS[1]
                                             : ORDERS[0];
    for (usize i = 0; i < PRIORITY_COUNT; ++i) {
      const usize priority = order[i];
      if (ready[priority].try_pop(fiber)) {
        ++worker->dispatches;
        return fiber;
      }
      if (pop_work(worker, priority, work)) {
        ++worker->dispatches;
        return work.fiber;
      }
    }
    if (stopping.load(std::memory_order_acquire))
      return &worker->thread_fiber;
    if (idle < IDLE_SPINS)
      detail::cpu_relax();
    else
      park_worker();
  }
}

void SchedulerState::wake_workers(const usize count) noexcept {
  if (count == 0)
    return;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  const u32 asleep = sleepers.load(std::memory_order_seq_cst);
  if (asleep != 0) {
    worker_epoch.fetch_add(1, std::memory_order_seq_cst);
    if (count >= asleep)
      futex::wake_all(&worker_epoch);
    else
      futex::wake(&worker_epoch, nx::cast<i32>(count));
  }
  if (count > asleep)
    wake_keeper();
}

void SchedulerState::wake_keeper() noexcept {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (keeper.load(std::memory_order_seq_cst) != 0)
    keeper_parker.unpark();
}

// A Windows futex timeout is rounded to the system tick (about 15.6 ms), so
// the worker that waits for the next timer parks on a precise parker instead.
void SchedulerState::park_worker() noexcept {
  if (next_deadline.load(std::memory_order_acquire) != NO_TIMER &&
      keeper.exchange(1, std::memory_order_seq_cst) == 0) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const i64 deadline = next_deadline.load(std::memory_order_seq_cst);
    if (deadline != NO_TIMER && !has_general_work() &&
        !stopping.load(std::memory_order_seq_cst))
      keeper_parker.park_until(
          clock::time_point(std::chrono::duration_cast<clock::duration>(
              std::chrono::nanoseconds(deadline))));
    keeper.store(0, std::memory_order_seq_cst);
    return;
  }
  const u32 epoch = worker_epoch.load(std::memory_order_seq_cst);
  sleepers.fetch_add(1, std::memory_order_seq_cst);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (!has_general_work() && !stopping.load(std::memory_order_seq_cst))
    futex::wait(&worker_epoch, epoch, detail::WAIT_FOREVER);
  sleepers.fetch_sub(1, std::memory_order_seq_cst);
}

void SchedulerState::wake_main() noexcept {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (main_sleeping.load(std::memory_order_seq_cst) != 0)
    main_parker.unpark();
}

void SchedulerState::park_main(const function_ref<bool()> done,
                               const clock::time_point deadline) noexcept {
  main_sleeping.store(1, std::memory_order_seq_cst);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (!main_work.can_pop_approx() && !done())
    main_parker.park_until(deadline);
  main_sleeping.store(0, std::memory_order_relaxed);
}

// The main thread never leaves its own stack: code it calls may check which
// stack it runs on, and Android's runtime fails every Java call made from a
// fiber stack. It waits here instead, running main-affine jobs in place.
// Returns whether `done` held before the deadline.
bool SchedulerState::serve_main(const function_ref<bool()> done,
                                const clock::time_point deadline) noexcept {
  for (;;) {
    if (done())
      return true;
    Work work;
    if (main_work.try_pop(work)) {
      execute(work);
      continue;
    }
    if (clock::now() >= deadline)
      return done();
    park_main(done, deadline);
  }
}

// A token on this stack rather than the thread's own fiber, so a job that
// waits inside another wait parks separately. counter::decrement releases it
// before settle() lets this frame end.
void SchedulerState::wait_on_main(fiber::counter &counter,
                                  const u32 target) noexcept {
  Fiber token;
  token.owner = this;
  token.affinity = Affinity::Main;
  token.target = target;
  park(&token, counter);
  (void)serve_main([&] { return counter.value() <= target; },
                   internal::NO_DEADLINE);
  settle(counter);
}

// The waiter names the thread's own fiber, which outlives it, since a waker
// may still be resuming it after this returns.
internal::WaitResult
SchedulerState::block_on_main(Worker *main, spin_lock *lock,
                              internal::WaitList *list,
                              const clock::time_point deadline) noexcept {
  internal::Waiter waiter;
  waiter.fiber = &main->thread_fiber;
  waiter.list = list;
  waiter.lock = lock;
  waiter.deadline = deadline;
  waiter.state.store(internal::Waiter::WAITING, std::memory_order_relaxed);
  if (list)
    list->push(&waiter);
  if (lock)
    lock->unlock();
  const auto woken = [&] {
    return waiter.state.load(std::memory_order_acquire) !=
           internal::Waiter::WAITING;
  };
  if (serve_main(woken, deadline))
    return internal::WaitResult::Woken;
  if (lock)
    lock->lock();
  u32 waiting = internal::Waiter::WAITING;
  const bool timed_out = waiter.state.compare_exchange_strong(
      waiting, internal::Waiter::TIMED_OUT, std::memory_order_acq_rel);
  if (timed_out && list)
    list->remove(&waiter);
  if (lock)
    lock->unlock();
  return timed_out ? internal::WaitResult::TimedOut
                   : internal::WaitResult::Woken;
}

void SchedulerState::wake_blocking() noexcept {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (blocking_sleepers.load(std::memory_order_seq_cst) != 0) {
    blocking_epoch.fetch_add(1, std::memory_order_seq_cst);
    futex::wake(&blocking_epoch, 1);
  }
}

void SchedulerState::park_blocking() noexcept {
  const u32 epoch = blocking_epoch.load(std::memory_order_seq_cst);
  blocking_sleepers.fetch_add(1, std::memory_order_seq_cst);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (!blocking_ready.can_pop_approx() &&
      !stopping.load(std::memory_order_seq_cst))
    futex::wait(&blocking_epoch, epoch, detail::WAIT_FOREVER);
  blocking_sleepers.fetch_sub(1, std::memory_order_seq_cst);
}

void SchedulerState::worker_main(Worker *worker) noexcept {
  worker->os_id.store(this_thread::os_id(), std::memory_order_release);
  set_current_worker(worker);
  NX_PROFILE_SET_THREAD_NAME(worker->name);
  Fiber *first = acquire_fiber();
  if (first)
    switch_fiber(&worker->thread_fiber, first, After::Nothing);
  else
    loge(log_fiber, "fiber: {} could not get a fiber and is not running",
         worker->name);
  set_current_worker(nullptr);
}

bool may_suspend() noexcept {
#if NX_DEBUG
  if (nx::detail::t_os_locks_held != 0)
    return false;
#endif
#if NX_ALLOC_GUARD
  if (!allocation_allowed())
    return false;
#endif
  return true;
}

void SchedulerState::assert_may_suspend() const noexcept {
  NX_ASSERT(may_suspend(), "a fiber must not suspend while it holds an OS "
                           "lock or is inside a no_suspend_scope");
}

internal::WaitResult
SchedulerState::block(Worker *worker, spin_lock *lock, internal::WaitList *list,
                      const clock::time_point deadline) noexcept {
  // The lock handed over is released once this fiber has switched out.
  nx::detail::note_os_lock(lock ? -1 : 0);
  assert_may_suspend();
  nx::detail::note_os_lock(lock ? 1 : 0);
  if (worker->role == Role::Main)
    return block_on_main(worker, lock, list, deadline);
  Fiber *self = worker->current;
  internal::Waiter &waiter = self->waiter;
  Fiber *next = next_to_resume(worker);
  if (!next) {
    if (limit_hits.fetch_add(1, std::memory_order_relaxed) == 0)
      logw(log_fiber,
           "fiber: all {} fibers are in use; a blocked fiber now blocks its "
           "thread",
           config.max_fibers);
    return internal::block_thread(waiter, lock, list, deadline);
  }
  waiter.fiber = self;
  waiter.next = nullptr;
  waiter.list = list;
  waiter.lock = lock;
  waiter.deadline = deadline;
  waiter.state.store(internal::Waiter::WAITING, std::memory_order_relaxed);

  Handoff out{After::Enqueue, self, next};
  out.lock = lock;
  out.list = list;
  switch_with(out);

  if (deadline != internal::NO_DEADLINE)
    remove_timer(self);
  return waiter.state.load(std::memory_order_acquire) ==
                 internal::Waiter::TIMED_OUT
             ? internal::WaitResult::TimedOut
             : internal::WaitResult::Woken;
}

void SchedulerState::publish_next_deadline() noexcept {
  next_deadline.store(timer_count ? ticks(timers[0]->waiter.deadline)
                                  : NO_TIMER,
                      std::memory_order_release);
}

void SchedulerState::sift_timer(u32 index) noexcept {
  const auto earlier = [&](u32 a, u32 b) {
    return timers[a]->waiter.deadline < timers[b]->waiter.deadline;
  };
  const auto swap = [&](u32 a, u32 b) {
    Fiber *held = timers[a];
    timers[a] = timers[b];
    timers[b] = held;
    timers[a]->waiter.heap_index = a;
    timers[b]->waiter.heap_index = b;
  };
  while (index > 0 && earlier(index, (index - 1) / 2)) {
    swap(index, (index - 1) / 2);
    index = (index - 1) / 2;
  }
  for (;;) {
    u32 first = index;
    for (const u32 child : {2 * index + 1, 2 * index + 2})
      if (child < timer_count && earlier(child, first))
        first = child;
    if (first == index)
      return;
    swap(index, first);
    index = first;
  }
}

void SchedulerState::add_timer(Fiber *fiber) noexcept {
  bool earliest = false;
  {
    scoped_lock guard(timer_lock);
    const u32 index = timer_count++;
    timers[index] = fiber;
    fiber->waiter.heap_index = index;
    sift_timer(index);
    earliest = timers[0] == fiber;
    publish_next_deadline();
  }
  if (earliest) {
    wake_keeper();
    wake_workers(1);
  }
}

void SchedulerState::remove_timer_at(const u32 index) noexcept {
  timers[index]->waiter.heap_index = internal::Waiter::NOT_IN_HEAP;
  const u32 last = --timer_count;
  if (index != last) {
    timers[index] = timers[last];
    timers[index]->waiter.heap_index = index;
    sift_timer(index);
  }
}

void SchedulerState::remove_timer(Fiber *fiber) noexcept {
  scoped_lock guard(timer_lock);
  if (fiber->waiter.heap_index != internal::Waiter::NOT_IN_HEAP) {
    remove_timer_at(fiber->waiter.heap_index);
    publish_next_deadline();
  }
}

// The state transition happens under timer_lock: a fiber removes its own
// timer under the same lock before it can wait again, so an expiry can never
// time out a later wait.
void SchedulerState::fire_timers() noexcept {
  if (next_deadline.load(std::memory_order_acquire) > ticks(clock::now()))
    return;
  fixed_vector<Fiber *, 32> expired;
  {
    scoped_lock guard(timer_lock);
    const clock::time_point now = clock::now();
    while (timer_count && timers[0]->waiter.deadline <= now &&
           expired.size() < expired.capacity()) {
      Fiber *fiber = timers[0];
      remove_timer_at(0);
      u32 waiting = internal::Waiter::WAITING;
      if (fiber->waiter.state.compare_exchange_strong(
              waiting, internal::Waiter::TIMED_OUT, std::memory_order_acq_rel))
        expired.push_back(fiber);
    }
    publish_next_deadline();
  }
  for (Fiber *fiber : expired) {
    internal::Waiter &waiter = fiber->waiter;
    if (waiter.lock) {
      scoped_lock guard(*waiter.lock);
      waiter.list->remove(&waiter);
    }
    make_ready(fiber);
  }
}

namespace internal {

void WaitList::push(Waiter *waiter) noexcept {
  waiter->next = nullptr;
  if (tail)
    tail->next = waiter;
  else
    head = waiter;
  tail = waiter;
}

bool WaitList::remove(Waiter *waiter) noexcept {
  Waiter *previous = nullptr;
  for (Waiter *it = head; it; previous = it, it = it->next) {
    if (it != waiter)
      continue;
    (previous ? previous->next : head) = it->next;
    if (tail == it)
      tail = previous;
    it->next = nullptr;
    return true;
  }
  return false;
}

WaitResult block_thread(Waiter &waiter, spin_lock *lock, WaitList *list,
                        const clock::time_point deadline) noexcept {
  waiter.fiber = nullptr;
  waiter.list = list;
  waiter.lock = lock;
  waiter.deadline = deadline;
  waiter.state.store(Waiter::WAITING, std::memory_order_relaxed);
  if (list)
    list->push(&waiter);
  if (lock)
    lock->unlock();
  for (;;) {
    const u32 state = waiter.state.load(std::memory_order_acquire);
    if (state != Waiter::WAITING)
      return WaitResult::Woken;
    if (deadline == NO_DEADLINE) {
      futex::wait(&waiter.state, Waiter::WAITING, nx::detail::WAIT_FOREVER);
      continue;
    }
    const clock::time_point now = clock::now();
    if (now < deadline) {
      futex::wait(&waiter.state, Waiter::WAITING,
                  nx::detail::to_ns(deadline - now));
      continue;
    }
    if (lock)
      lock->lock();
    u32 waiting = Waiter::WAITING;
    const bool timed_out = waiter.state.compare_exchange_strong(
        waiting, Waiter::TIMED_OUT, std::memory_order_acq_rel);
    if (timed_out && list)
      list->remove(&waiter);
    if (lock)
      lock->unlock();
    if (timed_out)
      return WaitResult::TimedOut;
  }
}

WaitResult block(spin_lock *lock, WaitList *list,
                 const clock::time_point deadline) noexcept {
  if (Worker *worker = current_worker())
    return worker->state->block(worker, lock, list, deadline);
  Waiter waiter;
  return block_thread(waiter, lock, list, deadline);
}

Claimed claim(WaitList &list) noexcept {
  while (Waiter *waiter = list.head) {
    list.head = waiter->next;
    if (!list.head)
      list.tail = nullptr;
    waiter->next = nullptr;
    u32 waiting = Waiter::WAITING;
    if (waiter->state.compare_exchange_strong(waiting, Waiter::WOKEN,
                                              std::memory_order_acq_rel))
      return Claimed{waiter->fiber, &waiter->state};
  }
  return {};
}

void resume(const Claimed &claimed) noexcept {
  if (claimed.fiber)
    claimed.fiber->owner->make_ready(claimed.fiber);
  else
    futex::wake(claimed.word, 1);
}

} // namespace internal

scheduler::scheduler(const config &config) {
  NX_ASSERT(!config.adopt_main_thread || current_worker() == nullptr,
            "a thread can be the main thread of only one fiber scheduler");
  m_state = allocate<SchedulerState>(config);
  SchedulerState &state = *m_state;
  if (config.adopt_main_thread)
    set_current_worker(state.workers[0]);
  for (usize i = 1; i < state.workers.size(); ++i) {
    Worker *worker = state.workers[i];
    worker->os_thread =
        thread([&state, worker] { state.worker_main(worker); },
               {.name = worker->name, .priority = config.priority});
  }
}

scheduler::~scheduler() {
  SchedulerState &state = *m_state;
  if (state.config.adopt_main_thread) {
    Worker *main = state.workers[0];
    NX_ASSERT(state.this_worker() == main &&
                  main->current == &main->thread_fiber,
              "the fiber scheduler must be destroyed by its main thread");
    while (state.live_jobs.load(std::memory_order_acquire) != 0)
      pump_main(std::chrono::milliseconds(1));
  } else {
    NX_ASSERT(state.this_worker() == nullptr,
              "a fiber scheduler cannot be destroyed from its own workers");
    while (state.live_jobs.load(std::memory_order_acquire) != 0)
      this_thread::sleep_for(std::chrono::microseconds(200));
  }

  state.stopping.store(true, std::memory_order_seq_cst);
  state.worker_epoch.fetch_add(1, std::memory_order_seq_cst);
  futex::wake_all(&state.worker_epoch);
  state.blocking_epoch.fetch_add(1, std::memory_order_seq_cst);
  futex::wake_all(&state.blocking_epoch);
  state.keeper_parker.unpark();
  for (usize i = 1; i < state.workers.size(); ++i)
    if (state.workers[i]->os_thread.joinable())
      state.workers[i]->os_thread.join();

  if (state.config.adopt_main_thread)
    set_current_worker(nullptr);
  release(m_state);
}

void scheduler::run(std::span<const job> jobs, fiber::counter *counter,
                    const fiber::priority priority) {
  m_state->submit(jobs, counter, priority, Affinity::Any);
}

void scheduler::run_on_main(std::span<const job> jobs,
                            fiber::counter *counter) {
  NX_ASSERT(m_state->config.adopt_main_thread,
            "run_on_main needs a scheduler with a main thread");
  m_state->submit(jobs, counter, fiber::priority::Normal, Affinity::Main);
}

// The fiber itself moves, so fn runs on its stack with its fiber_local, and
// may suspend there without holding the service thread.
#if NX_PROFILE_ENABLED && defined(TRACY_FIBERS) && defined(TRACY_ON_DEMAND)
void announce_to_profiler() noexcept {
  Worker *const worker = current_worker();
  if (worker == nullptr)
    return;
  const tracy::Profiler &profiler = tracy::GetProfiler();
  if (!profiler.IsConnected())
    return;
  const u64 connection = profiler.ConnectionId();
  if (worker->profiled_connection == connection)
    return;
  if (worker->current != nullptr && worker->current->profile_name)
    NX_PROFILE_FIBER_ENTER(worker->current->profile_name);
  worker->profiled_connection = connection;
}
#endif

void run_blocking(const function_ref<void()> fn) noexcept {
  Worker *worker = current_worker();
  if (!worker || worker->role == Role::Blocking ||
      worker->current == &worker->thread_fiber ||
      worker->state->config.blocking_threads == 0) {
    fn();
    return;
  }
  Fiber *self = worker->current;
  SchedulerState *state = self->owner;
  const Affinity home = self->affinity;
  self->affinity = Affinity::Blocking;
  state->suspend(After::Ready);
  fn();
  self->affinity = home;
  state->suspend(After::Ready);
}

void wait(fiber::counter &counter, const u32 target) noexcept {
  if (counter.value() > target) {
    if (Worker *worker = current_worker()) {
      worker->state->suspend_until(worker, counter, target);
    } else {
      for (;;) {
        if (counter.m_value.load(std::memory_order_seq_cst) <= target)
          break;
        counter.m_waiters.fetch_add(1, std::memory_order_seq_cst);
        const u32 value = counter.m_value.load(std::memory_order_seq_cst);
        if (value > target)
          futex::wait(&counter.m_value, value, detail::WAIT_FOREVER);
        counter.m_waiters.fetch_sub(1, std::memory_order_seq_cst);
      }
    }
  }
  SchedulerState::settle(counter);
}

void scheduler::yield() {
  if (in_fiber())
    m_state->suspend(After::Yield);
  else
    this_thread::yield();
}

void scheduler::pump_main(const std::chrono::nanoseconds budget) {
  SchedulerState &state = *m_state;
  Worker *main = state.this_worker();
  NX_ASSERT(main && main->role == Role::Main &&
                main->current == &main->thread_fiber,
            "pump_main belongs to the main thread outside any job");
  (void)state.serve_main([] { return false; }, clock::now() + budget);
}

namespace {
thread_local void *t_local[nx::cast<usize>(local_slot::Count)] = {};

NX_NO_INLINE void **slot_address(const local_slot slot) noexcept {
  const auto index = nx::cast<usize>(slot);
  Worker *worker = current_worker();
  return worker ? &worker->current->local[index] : &t_local[index];
}
} // namespace

void *fiber_local(const local_slot slot) noexcept {
  return *slot_address(slot);
}

void set_fiber_local(void *value, const local_slot slot) noexcept {
  *slot_address(slot) = value;
}

bool in_fiber() noexcept {
  const Worker *const worker = current_worker();
  return worker && worker->current != &worker->thread_fiber;
}

bool scheduler::on_worker_thread() const noexcept {
  const Worker *worker = m_state->this_worker();
  return worker && worker->role == Role::General;
}

u32 scheduler::worker_count() const noexcept {
  return m_state->config.worker_count;
}

usize scheduler::worker_os_ids(const std::span<u64> out) const noexcept {
  usize written = 0;
  for (const Worker *worker : m_state->workers) {
    if (worker->role != Role::General || written == out.size())
      continue;
    const u64 id = worker->os_id.load(std::memory_order_acquire);
    if (id != 0)
      out[written++] = id;
  }
  return written;
}

bool scheduler::on_main_thread() const noexcept {
  const Worker *worker = m_state->this_worker();
  return worker && worker->role == Role::Main;
}

bool scheduler::in_fiber() const noexcept {
  const Worker *worker = m_state->this_worker();
  return worker && worker->current != &worker->thread_fiber;
}

scheduler_stats scheduler::stats() const noexcept {
  const SchedulerState &state = *m_state;
  scheduler_stats stats;
  stats.workers = state.config.worker_count;
  stats.blocking_threads = state.config.blocking_threads;
  stats.fibers_created = min(state.fiber_count.load(std::memory_order_relaxed),
                             state.config.max_fibers);
  stats.fibers_free =
      nx::cast<u32>(max<isize>(state.free_fibers.size_approx(), 0));
  stats.fiber_limit_hits = state.limit_hits.load(std::memory_order_relaxed);
  stats.live_jobs = state.live_jobs.load(std::memory_order_relaxed);
  stats.jobs_run = state.jobs_run.load(std::memory_order_relaxed);
  return stats;
}

} // namespace nx::fiber
