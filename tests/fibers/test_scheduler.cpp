#include "framework/nxtest.h"

#include "core/foundation/fibers/scheduler.h"
#include "core/foundation/fibers/sync.h"
#include "core/foundation/threading/thread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>

using namespace std::chrono_literals;
using nx::fiber::Counter;
using nx::fiber::Job;
using nx::fiber::Priority;
using nx::fiber::Scheduler;

namespace {

template <typename F> Job job_of(F &fn) {
  return Job{[](void *arg) { (*static_cast<F *>(arg))(); }, &fn};
}

struct Tree {
  Scheduler *scheduler = nullptr;
  std::atomic<u32> *leaves = nullptr;
  u32 depth = 0;
};

// Every inner node waits on its children while holding its fiber; with more
// waiting nodes than threads this only finishes if a wait frees the thread.
void spawn_tree(void *arg) {
  const Tree &node = *static_cast<const Tree *>(arg);
  if (node.depth == 0) {
    node.leaves->fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Tree children[4];
  Job jobs[4];
  for (u32 i = 0; i < 4; ++i) {
    children[i] = Tree{node.scheduler, node.leaves, node.depth - 1};
    jobs[i] = Job{&spawn_tree, &children[i]};
  }
  Counter counter;
  node.scheduler->run(jobs, &counter);
  node.scheduler->wait(counter);
}

} // namespace

TEST_CASE("fiber scheduler: runs a batch on worker threads and the main "
          "thread waits for it") {
  Scheduler scheduler({.worker_count = 3});
  const u64 main_id = nx::this_thread::id();
  std::atomic<u32> ran{0};
  std::atomic<u32> on_main{0};
  auto body = [&] {
    ran.fetch_add(1);
    if (nx::this_thread::id() == main_id)
      on_main.fetch_add(1);
  };
  std::vector<Job> jobs(200, job_of(body));
  Counter counter;
  scheduler.run(jobs, &counter);
  scheduler.wait(counter);
  CHECK(ran.load() == 200);
  CHECK(on_main.load() == 0);
  CHECK(counter.value() == 0);
}

TEST_CASE("fiber scheduler: waiting inside a job suspends the fiber, not the "
          "thread") {
  Scheduler scheduler({.worker_count = 2, .max_fibers = 256});
  std::atomic<u32> leaves{0};
  Tree root{&scheduler, &leaves, 3};
  Counter counter;
  const Job job{&spawn_tree, &root};
  scheduler.run({&job, 1}, &counter);
  scheduler.wait(counter);
  CHECK(leaves.load() == 64);
  CHECK(scheduler.stats().fiber_limit_hits == 0);
}

TEST_CASE("fiber scheduler: past the fiber limit, waits run work nested and "
          "still finish") {
  Scheduler scheduler({.worker_count = 2, .max_fibers = 4});
  std::atomic<u32> leaves{0};
  Tree root{&scheduler, &leaves, 3};
  Counter counter;
  const Job job{&spawn_tree, &root};
  scheduler.run({&job, 1}, &counter);
  scheduler.wait(counter);
  CHECK(leaves.load() == 64);
  CHECK(scheduler.stats().fiber_limit_hits > 0);
  CHECK(scheduler.stats().fibers_created <= 4);
}

TEST_CASE("fiber scheduler: main-affine jobs run on the main thread only "
          "when it pumps") {
  Scheduler scheduler({.worker_count = 2});
  const u64 main_id = nx::this_thread::id();
  std::atomic<u32> ran{0};
  std::atomic<u32> on_main{0};
  auto body = [&] {
    ran.fetch_add(1);
    if (nx::this_thread::id() == main_id)
      on_main.fetch_add(1);
  };
  std::vector<Job> jobs(10, job_of(body));
  Counter counter;
  scheduler.run_on_main(jobs, &counter);
  nx::this_thread::sleep_for(20ms);
  CHECK(ran.load() == 0);

  scheduler.pump_main(0ns);
  CHECK(ran.load() == 10);
  CHECK(on_main.load() == 10);
  CHECK(counter.value() == 0);
}

TEST_CASE("fiber scheduler: main-affine work a job waits for runs on the main "
          "thread's own stack") {
  Scheduler scheduler({.worker_count = 2});
  const u64 main_id = nx::this_thread::id();
  u64 during = 0;
  bool in_fiber = true;
  auto on_main = [&] {
    during = nx::this_thread::id();
    in_fiber = scheduler.in_fiber();
  };
  const Job main_job = job_of(on_main);
  u64 before = 0;
  u64 after = 0;
  auto body = [&] {
    before = nx::this_thread::id();
    Counter done;
    scheduler.run_on_main({&main_job, 1}, &done);
    scheduler.wait(done);
    after = nx::this_thread::id();
  };
  const Job job = job_of(body);
  Counter counter;
  scheduler.run({&job, 1}, &counter);
  scheduler.wait(counter);
  CHECK(before != main_id);
  CHECK(during == main_id);
  CHECK_FALSE(in_fiber);
  CHECK(after != main_id);
}

TEST_CASE("fiber scheduler: main-affine work may wait while the main thread "
          "waits") {
  Scheduler scheduler({.worker_count = 2});
  std::atomic<u32> leaves{0};
  auto leaf = [&] { leaves.fetch_add(1); };
  const Job leaf_job = job_of(leaf);
  // Runs in place inside the main thread's wait, then waits itself.
  auto on_main = [&] {
    Counter inner;
    scheduler.run({&leaf_job, 1}, &inner);
    scheduler.wait(inner);
    Counter nested;
    scheduler.run({&leaf_job, 1}, &nested);
    scheduler.wait(nested);
  };
  const Job main_job = job_of(on_main);
  auto body = [&] {
    Counter done;
    scheduler.run_on_main({&main_job, 1}, &done);
    scheduler.wait(done);
  };
  const Job job = job_of(body);
  Counter counter;
  scheduler.run({&job, 1}, &counter);
  scheduler.wait(counter);
  CHECK(leaves.load() == 2);
}

TEST_CASE("fiber scheduler: a timed wait on the main thread serves main "
          "work, then times out") {
  Scheduler scheduler({.worker_count = 2});
  const u64 main_id = nx::this_thread::id();
  nx::fiber::event never;
  std::atomic<u64> ran_on{0};
  auto on_main = [&] { ran_on = nx::this_thread::id(); };
  const Job main_job = job_of(on_main);
  auto body = [&] {
    nx::this_thread::sleep_for(5ms);
    scheduler.run_on_main({&main_job, 1});
  };
  const Job job = job_of(body);
  Counter counter;
  scheduler.run({&job, 1}, &counter);

  const auto start = std::chrono::steady_clock::now();
  CHECK_FALSE(never.wait_for(60ms));
  CHECK(std::chrono::steady_clock::now() - start >= 60ms);
  CHECK(ran_on.load() == main_id);
  scheduler.wait(counter);
}

TEST_CASE("fiber scheduler: a wait on the main thread ends when a worker "
          "wakes it") {
  Scheduler scheduler({.worker_count = 2});
  nx::fiber::event later;
  auto body = [&] {
    nx::this_thread::sleep_for(10ms);
    later.set();
  };
  const Job job = job_of(body);
  Counter counter;
  scheduler.run({&job, 1}, &counter);

  const auto start = std::chrono::steady_clock::now();
  CHECK(later.wait_for(5s));
  CHECK(std::chrono::steady_clock::now() - start < 2s);
  scheduler.wait(counter);
}

TEST_CASE("fiber scheduler: yield lets queued work run on the same worker") {
  Scheduler scheduler({.worker_count = 1});
  std::atomic<bool> released{false};
  u32 yields = 0;
  auto waiter = [&] {
    while (!released.load())
      ++yields, scheduler.yield();
  };
  auto releaser = [&] { released.store(true); };
  const Job jobs[] = {job_of(waiter), job_of(releaser)};
  Counter counter;
  scheduler.run(jobs, &counter);
  scheduler.wait(counter);
  CHECK(released.load());
  CHECK(yields > 0);
}

TEST_CASE("fiber scheduler: a counter decremented by a foreign thread wakes "
          "a waiting fiber") {
  Scheduler scheduler({.worker_count = 2});
  Counter external(1);
  std::atomic<bool> resumed{false};
  auto body = [&] {
    scheduler.wait(external);
    resumed.store(true);
  };
  const Job job = job_of(body);
  Counter done;
  scheduler.run({&job, 1}, &done);
  nx::this_thread::sleep_for(20ms);
  CHECK_FALSE(resumed.load());

  nx::thread foreign([&] { external.decrement(); });
  foreign.join();
  scheduler.wait(done);
  CHECK(resumed.load());
}

TEST_CASE("fiber scheduler: a thread the scheduler does not own can wait") {
  Scheduler scheduler({.worker_count = 2});
  std::atomic<u32> ran{0};
  auto body = [&] {
    nx::this_thread::sleep_for(1ms);
    ran.fetch_add(1);
  };
  std::vector<Job> jobs(16, job_of(body));
  Counter counter;
  scheduler.run(jobs, &counter);
  bool finished = false;
  nx::thread foreign([&] {
    scheduler.wait(counter);
    finished = ran.load() == 16;
  });
  foreign.join();
  CHECK(finished);
}

TEST_CASE("fiber scheduler: high priority jobs start before low priority "
          "ones") {
  Scheduler scheduler({.worker_count = 1});
  std::atomic<bool> gate{false};
  std::vector<u32> order;
  auto blocker = [&] {
    while (!gate.load())
      nx::detail::cpu_relax();
  };
  auto low = [&] { order.push_back(0); };
  auto high = [&] { order.push_back(1); };
  const Job first = job_of(blocker);
  Counter counter;
  scheduler.run({&first, 1}, &counter);
  nx::this_thread::sleep_for(10ms);
  const std::vector<Job> lows(4, job_of(low));
  const std::vector<Job> highs(4, job_of(high));
  scheduler.run(lows, &counter, Priority::Low);
  scheduler.run(highs, &counter, Priority::High);
  gate.store(true);
  scheduler.wait(counter);
  REQUIRE(order.size() == 8);
  CHECK((order == std::vector<u32>{1, 1, 1, 1, 0, 0, 0, 0}));
}

TEST_CASE("fiber scheduler: pump_main returns after its budget when idle") {
  Scheduler scheduler({.worker_count = 1});
  const auto start = std::chrono::steady_clock::now();
  scheduler.pump_main(5ms);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK(elapsed >= 5ms);
  CHECK(elapsed < 1s);
}

TEST_CASE("fiber scheduler: destruction finishes outstanding jobs") {
  std::atomic<u32> ran{0};
  auto body = [&] {
    nx::this_thread::sleep_for(1ms);
    ran.fetch_add(1);
  };
  {
    Scheduler scheduler({.worker_count = 2});
    std::vector<Job> jobs(32, job_of(body));
    scheduler.run(jobs);
    std::vector<Job> main_jobs(4, job_of(body));
    scheduler.run_on_main(main_jobs);
  }
  CHECK(ran.load() == 36);
}

TEST_CASE("fiber scheduler: many concurrent waits and wakes stay consistent") {
  Scheduler scheduler({.worker_count = 4});
  std::atomic<u32> leaves{0};
  constexpr u32 ROOTS = 64;
  Tree roots[ROOTS];
  Job jobs[ROOTS];
  for (u32 i = 0; i < ROOTS; ++i) {
    roots[i] = Tree{&scheduler, &leaves, 3};
    jobs[i] = Job{&spawn_tree, &roots[i]};
  }
  for (u32 round = 0; round < 5; ++round) {
    Counter counter;
    scheduler.run(jobs, &counter);
    scheduler.wait(counter);
  }
  CHECK(leaves.load() == 5 * ROOTS * 64);
  CHECK(scheduler.stats().live_jobs == 0);
  CHECK(scheduler.stats().fiber_limit_hits == 0);
}

TEST_CASE("fiber scheduler: a wake racing the suspension is neither lost nor "
          "early") {
  Scheduler scheduler({.worker_count = 3});
  constexpr u32 JOBS = 4;
  constexpr u32 ROUNDS = 20000;
  std::atomic<Counter *> slots[JOBS] = {};
  std::atomic<bool> stop{false};
  nx::thread waker([&] {
    while (!stop.load(std::memory_order_acquire))
      for (auto &slot : slots)
        if (Counter *counter = slot.exchange(nullptr))
          counter->decrement();
  });

  struct Waiter {
    Scheduler *scheduler;
    std::atomic<Counter *> *slot;
    u32 finished = 0;
  } waiters[JOBS];
  Job jobs[JOBS];
  for (u32 i = 0; i < JOBS; ++i) {
    waiters[i] = Waiter{&scheduler, &slots[i]};
    jobs[i] = Job{[](void *arg) {
                    auto &waiter = *static_cast<Waiter *>(arg);
                    for (u32 round = 0; round < ROUNDS; ++round) {
                      Counter counter(1);
                      waiter.slot->store(&counter);
                      waiter.scheduler->wait(counter);
                      waiter.finished += counter.value() == 0;
                    }
                  },
                  &waiters[i]};
  }
  Counter done;
  scheduler.run(jobs, &done);
  scheduler.wait(done);
  stop.store(true, std::memory_order_release);
  waker.join();
  for (const Waiter &waiter : waiters)
    CHECK(waiter.finished == ROUNDS);
}

TEST_CASE("fiber scheduler: schedulers without a main thread coexist beside "
          "one with") {
  Scheduler main_scheduler({.worker_count = 1});
  Scheduler a(
      {.worker_count = 2, .name = "test-a", .adopt_main_thread = false});
  Scheduler b(
      {.worker_count = 2, .name = "test-b", .adopt_main_thread = false});
  CHECK(main_scheduler.on_main_thread());
  CHECK_FALSE(a.on_main_thread());
  CHECK(a.worker_count() == 2);

  std::atomic<u32> on_a{0};
  std::atomic<u32> on_b{0};
  auto body_a = [&] { on_a.fetch_add(a.on_worker_thread() ? 1 : 0); };
  auto body_b = [&] {
    on_b.fetch_add(b.on_worker_thread() && !a.on_worker_thread() ? 1 : 0);
  };
  const std::vector<Job> jobs_a(100, job_of(body_a));
  const std::vector<Job> jobs_b(100, job_of(body_b));
  Counter done_a;
  Counter done_b;
  a.run(jobs_a, &done_a);
  b.run(jobs_b, &done_b);
  nx::fiber::wait(done_a);
  nx::fiber::wait(done_b);
  CHECK(on_a.load() == 100);
  CHECK(on_b.load() == 100);
  CHECK_FALSE(a.on_worker_thread());
}

TEST_CASE("fiber scheduler: a fiber waits on work from another scheduler "
          "without blocking its thread") {
  Scheduler home({.worker_count = 1});
  Scheduler other({.worker_count = 1, .adopt_main_thread = false});
  Counter remote(1);
  std::atomic<bool> released{false};
  std::atomic<bool> neighbour_ran_first{false};
  auto waiter = [&] { nx::fiber::wait(remote); };
  auto neighbour = [&] { neighbour_ran_first.store(!released.load()); };
  auto release = [&] {
    nx::this_thread::sleep_for(20ms);
    released.store(true);
    remote.decrement();
  };
  const Job home_jobs[] = {job_of(waiter), job_of(neighbour)};
  const Job other_job = job_of(release);
  Counter home_done;
  Counter other_done;
  home.run(home_jobs, &home_done);
  other.run({&other_job, 1}, &other_done);
  home.wait(home_done);
  home.wait(other_done);
  CHECK(neighbour_ran_first.load());
}

TEST_CASE("fiber scheduler: fiber_local follows its job across a migration") {
  Scheduler scheduler({.worker_count = 3});
  std::atomic<u32> mismatches{0};
  std::atomic<u32> dirty_starts{0};
  struct Local {
    Scheduler *scheduler;
    std::atomic<u32> *mismatches;
    std::atomic<u32> *dirty_starts;
  } shared{&scheduler, &mismatches, &dirty_starts};
  const Job job{[](void *arg) {
                  auto &local = *static_cast<Local *>(arg);
                  if (nx::fiber::fiber_local() != nullptr)
                    local.dirty_starts->fetch_add(1);
                  u32 marker = 0;
                  nx::fiber::set_fiber_local(&marker);
                  for (u32 i = 0; i < 20; ++i) {
                    local.scheduler->yield();
                    if (nx::fiber::fiber_local() != &marker)
                      local.mismatches->fetch_add(1);
                  }
                },
                &shared};
  const std::vector<Job> jobs(64, job);
  // The second wave starts on fibers recycled from the first.
  for (u32 wave = 0; wave < 2; ++wave) {
    Counter counter;
    scheduler.run(jobs, &counter);
    scheduler.wait(counter);
  }
  CHECK(mismatches.load() == 0);
  CHECK(dirty_starts.load() == 0);
  CHECK(nx::fiber::fiber_local() == nullptr);
}

namespace {

struct Flood {
  Scheduler *scheduler = nullptr;
  Counter *counter = nullptr;
  std::atomic<bool> low_ran{false};
  std::atomic<u32> high_runs{0};
  u32 high_runs_before_low = 0;
};

// Keeps the high lane non-empty until the low job has run.
void flood_high(void *arg) {
  auto &flood = *static_cast<Flood *>(arg);
  const u32 runs = flood.high_runs.fetch_add(1) + 1;
  if (!flood.low_ran.load() && runs < 100000) {
    const Job next{&flood_high, arg};
    flood.scheduler->run({&next, 1}, flood.counter, Priority::High);
  }
}

void record_low(void *arg) {
  auto &flood = *static_cast<Flood *>(arg);
  flood.high_runs_before_low = flood.high_runs.load();
  flood.low_ran.store(true);
}

} // namespace

TEST_CASE("fiber scheduler: sustained high priority work cannot starve low "
          "priority work") {
  Scheduler scheduler({.worker_count = 1});
  Counter counter;
  Flood flood;
  flood.scheduler = &scheduler;
  flood.counter = &counter;
  const Job high{&flood_high, &flood};
  const Job low{&record_low, &flood};
  scheduler.run({&high, 1}, &counter, Priority::High);
  scheduler.run({&low, 1}, &counter, Priority::Low);
  scheduler.wait(counter);
  CHECK(flood.low_ran.load());
  CHECK(flood.high_runs_before_low < 100);
}

TEST_CASE("fiber scheduler: an idle pump_main returns close to its budget") {
  Scheduler scheduler({.worker_count = 1});
  std::vector<std::chrono::steady_clock::duration> overshoots;
  for (u32 i = 0; i < 21; ++i) {
    const auto deadline = std::chrono::steady_clock::now() + 1ms;
    scheduler.pump_main(1ms);
    overshoots.push_back(std::chrono::steady_clock::now() - deadline);
  }
  std::sort(overshoots.begin(), overshoots.end());
  CHECK(overshoots.front() >= std::chrono::steady_clock::duration::zero());
  CHECK(overshoots[overshoots.size() / 2] < 3ms);
}

namespace {

// Blocks the thread, not the fiber, the way a driver call does.
bool block_thread_until(const std::atomic<bool> &flag) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!flag.load()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    nx::this_thread::sleep_for(100us);
  }
  return true;
}

void wait_for_flag(const std::atomic<bool> &flag) {
  while (!flag.load())
    nx::this_thread::yield();
}

} // namespace

TEST_CASE("fiber scheduler: run_blocking moves the job to a service thread "
          "and back") {
  Scheduler scheduler({.worker_count = 2, .blocking_threads = 1});
  CHECK(scheduler.worker_count() == 2u);
  CHECK(scheduler.stats().blocking_threads == 1u);
  u64 before = 0;
  u64 during = 0;
  bool during_on_worker = true;
  bool during_on_main = true;
  bool after_on_worker = false;
  bool local_followed = false;
  auto body = [&] {
    u32 marker = 0;
    nx::fiber::set_fiber_local(&marker);
    before = nx::this_thread::id();
    nx::fiber::run_blocking([&] {
      during = nx::this_thread::id();
      during_on_worker = scheduler.on_worker_thread();
      during_on_main = scheduler.on_main_thread();
      local_followed = nx::fiber::fiber_local() == &marker;
    });
    after_on_worker = scheduler.on_worker_thread();
  };
  const Job job = job_of(body);
  Counter counter;
  scheduler.run({&job, 1}, &counter);
  scheduler.wait(counter);
  CHECK(during != before);
  CHECK_FALSE(during_on_worker);
  CHECK_FALSE(during_on_main);
  CHECK(local_followed);
  CHECK(after_on_worker);
}

TEST_CASE("fiber scheduler: a worker keeps running jobs while another job "
          "blocks in run_blocking") {
  Scheduler scheduler({.worker_count = 1, .blocking_threads = 1});
  std::atomic<bool> blocking{false};
  std::atomic<bool> released{false};
  bool saw_release = false;
  auto blocker = [&] {
    nx::fiber::run_blocking([&] {
      blocking.store(true);
      saw_release = block_thread_until(released);
    });
  };
  auto releaser = [&] { released.store(true); };
  const Job first = job_of(blocker);
  const Job second = job_of(releaser);
  Counter counter;
  scheduler.run({&first, 1}, &counter);
  wait_for_flag(blocking);
  scheduler.run({&second, 1}, &counter);
  scheduler.wait(counter);
  CHECK(saw_release);
}

TEST_CASE("fiber scheduler: blocking calls beyond the service threads queue "
          "and all finish") {
  Scheduler scheduler({.worker_count = 4, .blocking_threads = 2});
  std::atomic<u32> inside{0};
  std::atomic<u32> peak{0};
  std::atomic<u32> finished{0};
  auto body = [&] {
    nx::fiber::run_blocking([&] {
      const u32 now = inside.fetch_add(1) + 1;
      u32 seen = peak.load();
      while (seen < now && !peak.compare_exchange_weak(seen, now)) {
      }
      nx::this_thread::sleep_for(10ms);
      inside.fetch_sub(1);
    });
    finished.fetch_add(1);
  };
  const std::vector<Job> jobs(12, job_of(body));
  Counter counter;
  scheduler.run(jobs, &counter);
  scheduler.wait(counter);
  CHECK(finished.load() == 12u);
  CHECK(peak.load() == 2u);
}

TEST_CASE("fiber scheduler: a fiber that suspends inside run_blocking frees "
          "its service thread") {
  Scheduler scheduler({.worker_count = 2, .blocking_threads = 1});
  nx::fiber::event ready(true);
  std::atomic<bool> waiting{false};
  bool woken = false;
  auto waiter = [&] {
    nx::fiber::run_blocking([&] {
      waiting.store(true);
      woken = ready.wait_for(2s);
    });
  };
  auto setter = [&] { nx::fiber::run_blocking([&] { ready.set(); }); };
  const Job first = job_of(waiter);
  const Job second = job_of(setter);
  Counter counter;
  scheduler.run({&first, 1}, &counter);
  wait_for_flag(waiting);
  scheduler.run({&second, 1}, &counter);
  scheduler.wait(counter);
  CHECK(woken);
}

TEST_CASE("fiber scheduler: run_blocking runs in place without a service "
          "thread to go to") {
  const u64 caller = nx::this_thread::id();
  u64 outside = 0;
  nx::fiber::run_blocking([&] { outside = nx::this_thread::id(); });
  CHECK(outside == caller);

  Scheduler scheduler({.worker_count = 1});
  u64 before = 0;
  u64 during = 0;
  auto body = [&] {
    before = nx::this_thread::id();
    nx::fiber::run_blocking([&] { during = nx::this_thread::id(); });
  };
  const Job job = job_of(body);
  Counter counter;
  scheduler.run({&job, 1}, &counter);
  scheduler.wait(counter);
  CHECK(during == before);
}
