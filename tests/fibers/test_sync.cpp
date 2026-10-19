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
using nx::fiber::Scheduler;

namespace {

using steady = std::chrono::steady_clock;

template <typename F> Job job_of(F &fn) {
  return Job{[](void *arg) { (*static_cast<F *>(arg))(); }, &fn};
}

template <typename F> void run_many(Scheduler &scheduler, F &fn, usize count) {
  const std::vector<Job> jobs(count, job_of(fn));
  Counter counter;
  scheduler.run(jobs, &counter);
  scheduler.wait(counter);
}

void raise_max(std::atomic<u32> &max, u32 value) {
  u32 seen = max.load();
  while (value > seen && !max.compare_exchange_weak(seen, value)) {
  }
}

} // namespace

// One worker: a contender that blocked the thread instead of suspending would
// keep the holder, which yields inside the lock, from ever running again.
TEST_CASE("fiber sync: a contended mutex suspends fibers on a single worker") {
  Scheduler scheduler({.worker_count = 1});
  nx::fiber::mutex lock;
  u32 shared = 0;
  auto body = [&] {
    for (u32 i = 0; i < 200; ++i) {
      nx::scoped_lock guard(lock);
      const u32 value = shared;
      scheduler.yield();
      shared = value + 1;
    }
  };
  run_many(scheduler, body, 16);
  CHECK(shared == 3200);
}

TEST_CASE("fiber sync: fibers and foreign threads share a mutex") {
  Scheduler scheduler({.worker_count = 2});
  nx::fiber::mutex lock;
  u64 shared = 0;
  auto body = [&] {
    for (u32 i = 0; i < 5000; ++i) {
      nx::scoped_lock guard(lock);
      ++shared;
    }
  };
  const std::vector<Job> jobs(8, job_of(body));
  Counter counter;
  scheduler.run(jobs, &counter);
  nx::thread a([&] { body(); });
  nx::thread b([&] { body(); });
  a.join();
  b.join();
  scheduler.wait(counter);
  CHECK(shared == 10 * 5000);
}

TEST_CASE("fiber sync: rw_lock shares readers and excludes writers") {
  Scheduler scheduler({.worker_count = 1});
  nx::fiber::rw_lock lock;
  std::atomic<u32> readers{0};
  std::atomic<u32> max_readers{0};
  std::atomic<u32> writers{0};
  std::atomic<u32> violations{0};
  auto reader = [&] {
    for (u32 i = 0; i < 50; ++i) {
      lock.lock_shared();
      raise_max(max_readers, readers.fetch_add(1) + 1);
      if (writers.load() != 0)
        violations.fetch_add(1);
      scheduler.yield();
      readers.fetch_sub(1);
      lock.unlock_shared();
    }
  };
  auto writer = [&] {
    for (u32 i = 0; i < 50; ++i) {
      lock.lock();
      if (writers.fetch_add(1) != 0 || readers.load() != 0)
        violations.fetch_add(1);
      scheduler.yield();
      writers.fetch_sub(1);
      lock.unlock();
    }
  };
  std::vector<Job> jobs(6, job_of(reader));
  jobs.insert(jobs.end(), 2, job_of(writer));
  Counter counter;
  scheduler.run(jobs, &counter);
  scheduler.wait(counter);
  CHECK(violations.load() == 0);
  CHECK(max_readers.load() > 1);
}

TEST_CASE("fiber sync: a manual-reset event releases every waiter") {
  Scheduler scheduler({.worker_count = 1});
  nx::fiber::event ready(true);
  std::atomic<u32> passed{0};
  auto waiter = [&] {
    ready.wait();
    passed.fetch_add(1);
  };
  const std::vector<Job> jobs(8, job_of(waiter));
  Counter counter;
  scheduler.run(jobs, &counter);
  nx::this_thread::sleep_for(20ms);
  CHECK(passed.load() == 0);
  ready.set();
  scheduler.wait(counter);
  CHECK(passed.load() == 8);
  CHECK(ready.is_set());
}

// A set with no waiter queued stays set until one arrives, and two such sets
// collapse into one, so each set here waits for its release before the next.
TEST_CASE("fiber sync: an auto-reset event releases one waiter per set") {
  Scheduler scheduler({.worker_count = 2});
  nx::fiber::event signal;
  std::atomic<u32> passed{0};
  auto waiter = [&] {
    signal.wait();
    passed.fetch_add(1);
  };
  const std::vector<Job> jobs(8, job_of(waiter));
  Counter counter;
  scheduler.run(jobs, &counter);
  bool one_per_set = true;
  for (u32 i = 1; i <= 8; ++i) {
    signal.set();
    const auto deadline = steady::now() + 5s;
    while (passed.load() < i && steady::now() < deadline)
      nx::this_thread::yield();
    nx::this_thread::sleep_for(2ms);
    one_per_set = one_per_set && passed.load() == i;
  }
  scheduler.wait(counter);
  CHECK(one_per_set);
  CHECK(passed.load() == 8);
  CHECK_FALSE(signal.is_set());

  signal.set();
  CHECK(signal.is_set());
  CHECK(signal.wait_for(0ms));
  CHECK_FALSE(signal.is_set());
}

TEST_CASE("fiber sync: a semaphore bounds concurrency to its count") {
  Scheduler scheduler({.worker_count = 1});
  nx::fiber::semaphore permits(2);
  std::atomic<u32> inside{0};
  std::atomic<u32> max_inside{0};
  auto body = [&] {
    for (u32 i = 0; i < 20; ++i) {
      permits.acquire();
      raise_max(max_inside, inside.fetch_add(1) + 1);
      scheduler.yield();
      inside.fetch_sub(1);
      permits.release();
    }
  };
  run_many(scheduler, body, 16);
  CHECK(max_inside.load() == 2);
  CHECK(permits.try_acquire());
  CHECK(permits.try_acquire());
  CHECK_FALSE(permits.try_acquire());
}

TEST_CASE("fiber sync: cond_var passes items through a bounded queue") {
  Scheduler scheduler({.worker_count = 1});
  nx::fiber::mutex lock;
  nx::fiber::cond_var changed;
  std::vector<u32> queue;
  bool done = false;
  u64 consumed = 0;
  auto producer = [&] {
    for (u32 i = 1; i <= 1000; ++i) {
      nx::scoped_lock guard(lock);
      changed.wait(lock, [&] { return queue.size() < 4; });
      queue.push_back(i);
      changed.notify_all();
    }
    nx::scoped_lock guard(lock);
    done = true;
    changed.notify_all();
  };
  auto consumer = [&] {
    for (;;) {
      nx::scoped_lock guard(lock);
      changed.wait(lock, [&] { return !queue.empty() || done; });
      if (queue.empty())
        return;
      consumed += queue.back();
      queue.pop_back();
      changed.notify_all();
    }
  };
  const Job jobs[] = {job_of(consumer), job_of(consumer), job_of(producer)};
  Counter counter;
  scheduler.run(jobs, &counter);
  scheduler.wait(counter);
  CHECK(consumed == 1000ull * 1001 / 2);
}

TEST_CASE("fiber sync: timed waits time out, and succeed when signalled "
          "first") {
  Scheduler scheduler({.worker_count = 2});
  nx::fiber::event never;
  nx::fiber::semaphore later;
  nx::fiber::mutex lock;
  nx::fiber::cond_var quiet;
  bool event_result = true;
  bool cond_result = true;
  bool semaphore_result = false;
  steady::duration event_waited{};
  auto timing_out = [&] {
    const auto start = steady::now();
    event_result = never.wait_for(30ms);
    event_waited = steady::now() - start;
    nx::scoped_lock guard(lock);
    cond_result = quiet.wait_for(lock, 10ms);
  };
  auto signalled = [&] { semaphore_result = later.try_acquire_for(5s); };
  auto releaser = [&] {
    nx::fiber::sleep_for(10ms);
    later.release();
  };
  const Job jobs[] = {job_of(timing_out), job_of(signalled), job_of(releaser)};
  const auto start = steady::now();
  Counter counter;
  scheduler.run(jobs, &counter);
  scheduler.wait(counter);
  CHECK_FALSE(event_result);
  CHECK(event_waited >= 30ms);
  CHECK_FALSE(cond_result);
  CHECK(semaphore_result);
  CHECK(steady::now() - start < 2s);
}

TEST_CASE("fiber sync: sleep_for frees the worker for other jobs") {
  Scheduler scheduler({.worker_count = 1});
  steady::time_point sleeper_done{};
  steady::time_point other_done{};
  const auto start = steady::now();
  auto sleeper = [&] {
    nx::fiber::sleep_for(50ms);
    sleeper_done = steady::now();
  };
  auto other = [&] { other_done = steady::now(); };
  const Job jobs[] = {job_of(sleeper), job_of(other)};
  Counter counter;
  scheduler.run(jobs, &counter);
  scheduler.wait(counter);
  CHECK(sleeper_done - start >= 50ms);
  CHECK(other_done < sleeper_done);
}

TEST_CASE("fiber sync: permits are conserved when timeouts race releases") {
  Scheduler scheduler({.worker_count = 4});
  nx::fiber::semaphore permits;
  constexpr u32 RELEASES = 3000;
  std::atomic<u32> acquired{0};
  std::atomic<u32> timeouts{0};
  std::atomic<bool> releasing{true};
  auto contender = [&] {
    while (releasing.load())
      if (permits.try_acquire_for(std::chrono::microseconds(200)))
        acquired.fetch_add(1);
      else
        timeouts.fetch_add(1);
  };
  auto releaser = [&] {
    for (u32 i = 0; i < RELEASES; ++i) {
      permits.release();
      if (i % 64 == 0)
        nx::fiber::sleep_for(std::chrono::microseconds(100));
    }
    releasing.store(false);
  };
  std::vector<Job> jobs(32, job_of(contender));
  jobs.push_back(job_of(releaser));
  Counter counter;
  scheduler.run(jobs, &counter);
  scheduler.wait(counter);
  u32 left = 0;
  while (permits.try_acquire())
    ++left;
  CHECK(acquired.load() + left == RELEASES);
  CHECK(timeouts.load() > 0);
}

TEST_CASE("fiber sync: threads outside any scheduler and the main thread can "
          "wait") {
  Scheduler scheduler({.worker_count = 2});
  nx::fiber::event from_job(true);
  nx::fiber::event from_thread(true);
  auto setter = [&] {
    nx::fiber::sleep_for(10ms);
    from_job.set();
  };
  const Job job = job_of(setter);
  Counter counter;
  scheduler.run({&job, 1}, &counter);
  from_job.wait();
  CHECK(from_job.is_set());

  bool thread_saw = false;
  bool thread_timed_out = false;
  nx::thread foreign([&] {
    thread_timed_out = !from_thread.wait_for(10ms);
    from_thread.wait();
    thread_saw = true;
  });
  nx::this_thread::sleep_for(30ms);
  from_thread.set();
  foreign.join();
  scheduler.wait(counter);
  CHECK(thread_timed_out);
  CHECK(thread_saw);
}

TEST_CASE("fiber sync: may_suspend reports OS locks and no_suspend scopes") {
  CHECK(nx::fiber::may_suspend());
  {
    nx::fiber::no_suspend_scope scope;
#if NX_DEBUG
    CHECK_FALSE(nx::fiber::may_suspend());
#endif
  }
  CHECK(nx::fiber::may_suspend());
  nx::mutex os_lock;
  {
    nx::scoped_lock guard(os_lock);
#if NX_DEBUG
    CHECK_FALSE(nx::fiber::may_suspend());
#endif
  }
  nx::spin_lock spin;
  spin.lock();
#if NX_DEBUG
  CHECK_FALSE(nx::fiber::may_suspend());
#endif
  spin.unlock();
  CHECK(nx::fiber::may_suspend());
  REQUIRE(spin.try_lock());
#if NX_DEBUG
  CHECK_FALSE(nx::fiber::may_suspend());
#endif
  spin.unlock();
  CHECK(nx::fiber::may_suspend());
  nx::fiber::mutex fiber_lock;
  {
    nx::scoped_lock guard(fiber_lock);
    CHECK(nx::fiber::may_suspend());
  }
  CHECK(nx::fiber::may_suspend());
}

// Each wait races a set() from another worker; a waiter published before it
// is off its stack, or after its lock is dropped, loses a wake and hangs.
TEST_CASE("fiber sync: ping-pong across workers never loses a wake") {
  Scheduler scheduler({.worker_count = 2});
  nx::fiber::event ping;
  nx::fiber::event pong;
  constexpr u32 ROUNDS = 20000;
  u32 pings = 0;
  u32 pongs = 0;
  auto left = [&] {
    for (u32 i = 0; i < ROUNDS; ++i) {
      ping.set();
      pong.wait();
      ++pongs;
    }
  };
  auto right = [&] {
    for (u32 i = 0; i < ROUNDS; ++i) {
      ping.wait();
      ++pings;
      pong.set();
    }
  };
  const Job jobs[] = {job_of(left), job_of(right)};
  Counter counter;
  scheduler.run(jobs, &counter);
  scheduler.wait(counter);
  CHECK(pings == ROUNDS);
  CHECK(pongs == ROUNDS);
}

// With every worker idle, a timer is kept by a parked worker; on Windows a
// futex timeout would round each of these up to the ~15.6 ms system tick.
TEST_CASE("fiber sync: a short sleep on an idle scheduler is precise") {
  Scheduler scheduler({.worker_count = 2});
  std::vector<steady::duration> overshoots;
  auto body = [&] {
    for (u32 i = 0; i < 21; ++i) {
      const auto deadline = steady::now() + std::chrono::microseconds(300);
      nx::fiber::sleep_for(std::chrono::microseconds(300));
      overshoots.push_back(steady::now() - deadline);
    }
  };
  const Job job = job_of(body);
  Counter counter;
  scheduler.run({&job, 1}, &counter);
  scheduler.wait(counter);
  REQUIRE(overshoots.size() == 21);
  std::sort(overshoots.begin(), overshoots.end());
  CHECK(overshoots.front() >= steady::duration::zero());
  CHECK(overshoots[overshoots.size() / 2] < 3ms);
}

TEST_CASE("fiber sync: cond_var waits on a unique_lock off the scheduler") {
  nx::fiber::mutex m;
  nx::fiber::cond_var cv;
  bool ready = false;
  std::atomic<bool> woke{false};

  nx::thread waiter([&] {
    nx::unique_lock<nx::fiber::mutex> lock(m);
    cv.wait(lock, [&] { return ready; });
    woke.store(lock.owns_lock(), std::memory_order_release);
  });

  {
    nx::unique_lock<nx::fiber::mutex> lock(m);
    ready = true;
  }
  cv.notify_all();
  waiter.join();
  CHECK(woke.load());

  nx::unique_lock<nx::fiber::mutex> lock(m);
  CHECK_FALSE(cv.wait_for(lock, 1ms, [] { return false; }));
  CHECK(lock.owns_lock());
}

TEST_CASE("fiber sync: a waiter may destroy what woke it") {
  for (int round = 0; round < 200; ++round) {
    auto *const done = new nx::fiber::event;
    auto *const cv = new nx::fiber::cond_var;
    auto *const m = new nx::fiber::mutex;
    std::atomic<bool> waiting{false};
    bool ready = false;

    nx::thread waiter([&] {
      {
        nx::unique_lock<nx::fiber::mutex> lock(*m);
        waiting.store(true, std::memory_order_release);
        cv->wait(lock, [&] { return ready; });
      }
      done->wait();
      delete cv;
      delete done;
      delete m;
    });

    while (!waiting.load(std::memory_order_acquire))
      nx::this_thread::yield();
    {
      nx::unique_lock<nx::fiber::mutex> lock(*m);
      ready = true;
    }
    cv->notify_all();
    done->set();
    waiter.join();
  }
  CHECK(true);
}

TEST_CASE(
    "fiber sync: a contended unlock lets the next owner destroy the mutex") {
  for (int round = 0; round < 200; ++round) {
    auto *const m = new nx::fiber::mutex;
    m->lock();
    std::atomic<bool> started{false};
    nx::thread taker([&] {
      started.store(true, std::memory_order_release);
      m->lock();
      m->unlock();
      delete m;
    });
    while (!started.load(std::memory_order_acquire))
      nx::this_thread::yield();
    nx::this_thread::sleep_for(std::chrono::microseconds(50));
    m->unlock();
    taker.join();
  }
  CHECK(true);
}
