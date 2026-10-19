#include "framework/nxtest.h"

#include "core/foundation/fibers/sync.h"
#include "core/foundation/threading/thread.h"
#include "core/foundation/threading/thread_pool.h"
#include "core/foundation/threading/workload.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr u32 kWorkers = 4;

nx::thread_pool::config small_config() {
  nx::thread_pool::config cfg;
  cfg.thread_count = kWorkers;
  cfg.name = "nx-test-pool";
  return cfg;
}

} // namespace

TEST_CASE("thread_pool: constructs with an explicit worker count") {
  nx::thread_pool pool(small_config());
  CHECK(pool.worker_count() == kWorkers);
  CHECK_FALSE(pool.is_worker_thread());
}

TEST_CASE("workload telemetry: records completion cancellation and rejection") {
  nx::workload_tracker tracker;

  const nx::workload_ticket completed = tracker.submit();
  CHECK(nx::measure_workload(tracker, completed, {}, [] { return 42; }) == 42);

  nx::cancellation_source cancellation;
  REQUIRE(cancellation.request_stop());
  const nx::workload_ticket cancelled = tracker.submit();
  CHECK_THROWS_AS(
      nx::measure_workload(tracker, cancelled, cancellation.token(), [] {}),
      nx::task_cancelled);

  const nx::workload_ticket rejected = tracker.submit();
  tracker.reject(rejected);

  const nx::workload_ticket failed = tracker.submit();
  CHECK_THROWS_AS(nx::measure_workload(
                      tracker, failed, {},
                      []() -> int { throw std::runtime_error("expected"); }),
                  std::runtime_error);

  const nx::workload_statistics stats = tracker.statistics();
  CHECK(stats.submitted == 4u);
  CHECK(stats.started == 3u);
  CHECK(stats.completed == 3u);
  CHECK(stats.cancelled == 1u);
  CHECK(stats.failed == 1u);
  CHECK(stats.rejected == 1u);
  CHECK(stats.queued == 0u);
  CHECK(stats.active == 0u);
  CHECK(stats.peak_queued >= 1u);
  CHECK(stats.peak_active == 1u);
}

TEST_CASE("thread_pool: the default configuration picks a sane worker count") {
  nx::thread_pool pool;
  CHECK(pool.worker_count() <= nx::cpu::logical_core_count() + 1);
  pool.wait_idle();
}

TEST_CASE("thread_pool: a single-worker pool still runs everything") {
  nx::thread_pool::config cfg;
  cfg.thread_count = 1;
  nx::thread_pool pool(cfg);
  CHECK(pool.worker_count() == 1);

  std::atomic<int> ran{0};
  for (int i = 0; i < 200; ++i)
    pool.submit([&] { ran.fetch_add(1, std::memory_order_relaxed); });
  pool.wait_idle();
  CHECK(ran.load() == 200);
}

TEST_CASE("thread_pool: submit runs every task exactly once") {
  nx::thread_pool pool(small_config());
  constexpr int count = 5000;
  std::atomic<int> ran{0};

  for (int i = 0; i < count; ++i)
    pool.submit([&] { ran.fetch_add(1, std::memory_order_relaxed); });

  pool.wait_idle();
  CHECK(ran.load() == count);
}

TEST_CASE("thread_pool: every submitted task sees its own captured value") {
  nx::thread_pool pool(small_config());
  constexpr int count = 2000;
  std::vector<std::atomic<int>> slots(count);
  for (auto &s : slots)
    s.store(-1, std::memory_order_relaxed);

  for (int i = 0; i < count; ++i)
    pool.submit([&slots, i] { slots[i].store(i, std::memory_order_relaxed); });

  pool.wait_idle();
  for (int i = 0; i < count; ++i)
    REQUIRE(slots[static_cast<usize>(i)].load() == i);
}

TEST_CASE("thread_pool: submit accepts a plain callable and an nx::function") {
  nx::thread_pool pool(small_config());
  std::atomic<int> sum{0};

  pool.submit([&] { sum.fetch_add(1, std::memory_order_relaxed); });
  nx::function<void()> fn{[&] { sum.fetch_add(2, std::memory_order_relaxed); }};
  pool.submit(std::move(fn));

  pool.wait_idle();
  CHECK(sum.load() == 3);
}

TEST_CASE("thread_pool: submitting a small task does not allocate") {
  nx::thread_pool pool(small_config());
  std::atomic<bool> ran{false};

  {
    const nx::scoped_no_alloc no_alloc;
    pool.submit([&ran] { ran.store(true, std::memory_order_release); });
  }

  pool.wait_idle();
  CHECK(ran.load(std::memory_order_acquire));
}

TEST_CASE("thread_pool: work overflows the bounded queues without loss") {
  nx::thread_pool pool(small_config());
  constexpr int count = 50000;
  std::atomic<int> ran{0};

  for (int i = 0; i < count; ++i)
    pool.submit([&] { ran.fetch_add(1, std::memory_order_relaxed); });

  pool.wait_idle();
  CHECK(ran.load() == count);
}

TEST_CASE(
    "thread_pool: is_worker_thread identifies exactly this pool's workers") {
  nx::thread_pool pool(small_config());
  CHECK_FALSE(pool.is_worker_thread());

  const u64 main_id = nx::this_thread::id();
  std::atomic<int> mismatches{0};
  std::atomic<int> tasks{0};

  for (int i = 0; i < 200; ++i) {
    pool.submit([&] {
      const bool on_worker = pool.is_worker_thread();
      const bool is_submitter = nx::this_thread::id() == main_id;
      if (on_worker == is_submitter)
        mismatches.fetch_add(1, std::memory_order_relaxed);
      tasks.fetch_add(1, std::memory_order_relaxed);
    });
  }
  pool.wait_idle();

  CHECK(tasks.load() == 200);
  CHECK(mismatches.load() == 0);
  CHECK_FALSE(pool.is_worker_thread());

  nx::event gate(true);
  std::atomic<int> started{0};
  std::atomic<int> on_worker{0};
  nx::task_group group;
  const u32 n = pool.worker_count();
  for (u32 i = 0; i < n; ++i) {
    pool.submit(group, [&] {
      started.fetch_add(1, std::memory_order_release);
      gate.wait();
      if (pool.is_worker_thread())
        on_worker.fetch_add(1, std::memory_order_relaxed);
    });
  }
  while (started.load(std::memory_order_acquire) < static_cast<int>(n))
    nx::this_thread::yield();
  gate.set();
  group.wait();
  CHECK(on_worker.load() == static_cast<int>(n));

  nx::thread_pool other(small_config());
  std::atomic<bool> foreign{true};
  nx::task_group other_group;
  other.submit(other_group, [&] { foreign.store(pool.is_worker_thread()); });
  other_group.wait();
  CHECK_FALSE(foreign.load());
}

TEST_CASE("thread_pool: wait_idle returns only once everything has finished") {
  nx::thread_pool pool(small_config());
  constexpr int count = 200;
  std::atomic<int> ran{0};

  for (int i = 0; i < count; ++i) {
    pool.submit([&] {
      nx::this_thread::sleep_for(1ms);
      ran.fetch_add(1, std::memory_order_relaxed);
    });
  }
  pool.wait_idle();
  CHECK(ran.load() == count);

  const auto start = std::chrono::steady_clock::now();
  pool.wait_idle();
  CHECK(std::chrono::steady_clock::now() - start < 2s);

  pool.submit([&] { ran.fetch_add(1, std::memory_order_relaxed); });
  pool.wait_idle();
  CHECK(ran.load() == count + 1);
}

TEST_CASE("thread_pool: the destructor drains pending work") {
  constexpr int count = 2000;
  std::atomic<int> ran{0};
  {
    nx::thread_pool pool(small_config());
    for (int i = 0; i < count; ++i)
      pool.submit([&] { ran.fetch_add(1, std::memory_order_relaxed); });
  }
  CHECK(ran.load() == count);
}

TEST_CASE("task_group: wait blocks until every task in the group is done") {
  nx::thread_pool pool(small_config());
  constexpr int count = 2000;
  std::atomic<int> ran{0};

  nx::task_group group;
  CHECK(group.done());

  for (int i = 0; i < count; ++i) {
    pool.submit(group, [&] {
      nx::this_thread::yield();
      ran.fetch_add(1, std::memory_order_relaxed);
    });
  }
  group.wait();

  CHECK(ran.load() == count);
  CHECK(group.done());
}

// With one worker, a waiter that ran its dependency nested on the worker's
// stack would leave B blocking the only thread, and C could never run.
TEST_CASE("task_group: a waiting task frees its worker for unrelated work") {
  nx::thread_pool::config cfg;
  cfg.thread_count = 1;
  nx::thread_pool pool(cfg);
  nx::fiber::event released(true);
  std::atomic<bool> a_done{false};

  pool.submit([&] {
    nx::task_group inner;
    pool.submit(inner, [&] { released.wait(); });
    inner.wait();
    a_done.store(true, std::memory_order_release);
  });
  pool.submit([&] { released.set(); });

  pool.wait_idle();
  CHECK(a_done.load(std::memory_order_acquire));
}

TEST_CASE("task_group: is reusable across batches") {
  nx::thread_pool pool(small_config());
  nx::task_group group;
  std::atomic<int> ran{0};

  for (int round = 0; round < 10; ++round) {
    for (int i = 0; i < 200; ++i)
      pool.submit(group, [&] { ran.fetch_add(1, std::memory_order_relaxed); });
    group.wait();
    REQUIRE(group.done());
    REQUIRE(ran.load() == (round + 1) * 200);
  }
  CHECK(ran.load() == 2000);
}

TEST_CASE("task_group: waiting on an empty group returns immediately") {
  nx::thread_pool pool(small_config());
  nx::task_group group;
  const auto start = std::chrono::steady_clock::now();
  group.wait();
  CHECK(std::chrono::steady_clock::now() - start < 2s);
  CHECK(group.done());

  std::atomic<int> ran{0};
  pool.submit(group, [&] { ran.fetch_add(1, std::memory_order_relaxed); });
  group.wait();
  group.wait();
  CHECK(ran.load() == 1);
}

TEST_CASE("task_group: independent groups do not interfere") {
  nx::thread_pool pool(small_config());
  nx::task_group a, b;
  std::atomic<int> ran_a{0}, ran_b{0};
  nx::event gate(true);

  for (int i = 0; i < 100; ++i)
    pool.submit(b, [&] {
      gate.wait();
      ran_b.fetch_add(1, std::memory_order_relaxed);
    });
  for (int i = 0; i < 100; ++i)
    pool.submit(a, [&] { ran_a.fetch_add(1, std::memory_order_relaxed); });

  gate.set();
  a.wait();
  CHECK(ran_a.load() == 100);
  b.wait();
  CHECK(ran_b.load() == 100);
}

TEST_CASE("task_group: waiting from inside a pool task does not deadlock") {
  nx::thread_pool pool(small_config());
  std::atomic<int> inner_ran{0};
  std::atomic<int> outer_ran{0};

  nx::task_group outer;
  for (int i = 0; i < 8; ++i) {
    pool.submit(outer, [&] {
      nx::task_group inner;
      for (int j = 0; j < 25; ++j)
        pool.submit(inner,
                    [&] { inner_ran.fetch_add(1, std::memory_order_relaxed); });
      inner.wait();
      outer_ran.fetch_add(1, std::memory_order_relaxed);
    });
  }
  outer.wait();

  CHECK(outer_ran.load() == 8);
  CHECK(inner_ran.load() == 8 * 25);
}

TEST_CASE("future: a default-constructed future is invalid") {
  nx::future<int> f;
  CHECK_FALSE(f.valid());
  CHECK_FALSE(f.ready());
  f.wait();
}

TEST_CASE("future: async returns the task's result") {
  nx::thread_pool pool(small_config());

  auto f = pool.async([] { return 42; });
  CHECK(f.valid());
  CHECK(f.get() == 42);

  auto s = pool.async([] { return std::string("hello"); });
  CHECK(s.get() == "hello");

  std::atomic<bool> ran{false};
  auto v = pool.async([&] { ran.store(true, std::memory_order_release); });
  v.get();
  CHECK(ran.load(std::memory_order_acquire));
}

TEST_CASE("future: wait and ready track completion") {
  nx::thread_pool pool(small_config());
  nx::event gate(true);

  auto f = pool.async([&] {
    gate.wait();
    return 7;
  });
  CHECK(f.valid());
  CHECK_FALSE(f.ready());

  gate.set();
  f.wait();
  CHECK(f.ready());
  CHECK(f.get() == 7);
}

TEST_CASE("future: many concurrent futures each return their own value") {
  nx::thread_pool pool(small_config());
  constexpr int count = 1000;

  std::vector<nx::future<int>> futures;
  futures.reserve(count);
  for (int i = 0; i < count; ++i)
    futures.push_back(pool.async([i] { return i * 3; }));

  for (int i = 0; i < count; ++i)
    REQUIRE(futures[static_cast<usize>(i)].get() == i * 3);
}

TEST_CASE("future: is move-only and moving transfers the state") {
  nx::thread_pool pool(small_config());

  static_assert(!std::is_copy_constructible_v<nx::future<int>>);
  static_assert(!std::is_copy_assignable_v<nx::future<int>>);

  auto a = pool.async([] { return 5; });
  nx::future<int> b = std::move(a);
  CHECK_FALSE(a.valid());
  CHECK(b.valid());
  CHECK(b.get() == 5);

  auto c = pool.async([] { return 1; });
  auto d = pool.async([] { return 2; });
  c = std::move(d);
  CHECK_FALSE(d.valid());
  CHECK(c.get() == 2);

  auto e = pool.async([] { return 9; });
  nx::future<int> *alias = &e;
  e = std::move(*alias);
  CHECK(e.valid());
  CHECK(e.get() == 9);
}

TEST_CASE("future: get invalidates the future") {
  nx::thread_pool pool(small_config());
  auto f = pool.async([] { return 3; });
  CHECK(f.get() == 3);
  CHECK_FALSE(f.valid());
}

TEST_CASE("future: dropping a future early still runs the task") {
  nx::thread_pool pool(small_config());
  std::atomic<int> ran{0};

  for (int i = 0; i < 500; ++i) {
    auto f = pool.async([&] {
      ran.fetch_add(1, std::memory_order_relaxed);
      return 1;
    });
  }
  pool.wait_idle();
  CHECK(ran.load() == 500);
}

TEST_CASE("future: carries a move-only result type") {
  nx::thread_pool pool(small_config());
  auto f = pool.async([] { return std::make_unique<int>(11); });
  const auto p = f.get();
  REQUIRE(p != nullptr);
  CHECK(*p == 11);
}

TEST_CASE("future: propagates task failures and leaves the pool usable") {
  nx::thread_pool pool(small_config());

  auto failed =
      pool.async([]() -> int { throw std::runtime_error("expected failure"); });
  failed.wait();
  CHECK(failed.ready());
  CHECK(failed.failed());
  CHECK_THROWS_AS(failed.get(), std::runtime_error);
  CHECK_FALSE(failed.valid());

  auto healthy = pool.async([] { return 42; });
  CHECK(healthy.get() == 42);
}

TEST_CASE("future: continuations transform values without blocking a worker") {
  nx::thread_pool pool(small_config());

  auto result = std::move(pool.async([] { return 20; }))
                    .then([](int value) { return value + 1; })
                    .then([](int value) { return value * 2; });
  REQUIRE(result.valid());
  CHECK(result.get() == 42);

  std::atomic<bool> ran{false};
  auto from_void = std::move(pool.async([] {})).then([&] {
    ran.store(true, std::memory_order_release);
    return std::string("done");
  });
  CHECK(from_void.get() == "done");
  CHECK(ran.load(std::memory_order_acquire));
}

TEST_CASE("future: a failed predecessor bypasses its continuation") {
  nx::thread_pool pool(small_config());
  std::atomic<bool> ran{false};

  auto result = std::move(pool.async([]() -> int {
                  throw std::logic_error("stop");
                })).then([&](int) {
    ran.store(true, std::memory_order_release);
    return 1;
  });
  CHECK_THROWS_AS(result.get(), std::logic_error);
  CHECK_FALSE(ran.load(std::memory_order_acquire));
}

TEST_CASE("future: a continuation failure propagates to its result") {
  nx::thread_pool pool(small_config());
  auto result = std::move(pool.async([] { return 42; })).then([](int) -> int {
    throw std::range_error("continuation failed");
  });
  CHECK_THROWS_AS(result.get(), std::range_error);

  auto healthy = pool.async([] { return 7; });
  CHECK(healthy.get() == 7);
}

TEST_CASE("future: completion and continuation registration may race") {
  nx::thread_pool pool(small_config());
  constexpr int rounds = 1000;

  for (int i = 0; i < rounds; ++i) {
    auto result = std::move(pool.async([i] {
                    if ((i & 1) == 0)
                      nx::this_thread::yield();
                    return i;
                  })).then([](const int value) { return value + 1; });
    REQUIRE(result.get() == i + 1);
  }
}

TEST_CASE("future: continuation may be attached after completion") {
  nx::thread_pool pool(small_config());
  auto source = pool.async([] { return 6; });
  source.wait();
  REQUIRE(source.ready());

  auto result = std::move(source).then([](int value) { return value * 7; });
  CHECK_FALSE(source.valid());
  CHECK(result.get() == 42);
}

TEST_CASE("future: continuation can target another pool and priority") {
  nx::thread_pool source_pool(small_config());
  nx::thread_pool target_pool(small_config());
  std::atomic<bool> used_target_worker{false};

  auto result =
      std::move(source_pool.async([] { return 40; }))
          .then(target_pool, nx::task_priority::Critical,
                [&target_pool, &used_target_worker](int value) {
                  used_target_worker.store(target_pool.is_worker_thread(),
                                           std::memory_order_release);
                  return value + 2;
                });
  while (!result.ready())
    nx::this_thread::yield();
  CHECK(result.get() == 42);
  CHECK(used_target_worker.load(std::memory_order_acquire));
}

TEST_CASE("cancellation: queued async work completes as cancelled") {
  nx::event release_blocker(true);
  std::atomic<bool> blocker_started{false};
  nx::thread_pool::config cfg;
  cfg.thread_count = 1;
  nx::thread_pool pool(cfg);

  pool.submit([&] {
    blocker_started.store(true, std::memory_order_release);
    release_blocker.wait();
  });
  while (!blocker_started.load(std::memory_order_acquire))
    nx::this_thread::yield();

  nx::cancellation_source source;
  auto result = pool.async(source.token(), [] { return 42; });
  CHECK(source.request_stop());
  CHECK_FALSE(source.request_stop());
  release_blocker.set();

  result.wait();
  CHECK(result.cancelled());
  CHECK_FALSE(result.failed());
  CHECK_THROWS_AS(result.get(), nx::task_cancelled);
}

TEST_CASE("cancellation: running work can observe its token") {
  nx::thread_pool pool(small_config());
  nx::cancellation_source source;
  std::atomic<bool> started{false};

  auto result = pool.async(source.token(), [&](nx::cancellation_token token) {
    started.store(true, std::memory_order_release);
    for (;;) {
      token.throw_if_requested();
      nx::this_thread::yield();
    }
  });
  while (!started.load(std::memory_order_acquire))
    nx::this_thread::yield();
  REQUIRE(source.request_stop());

  CHECK_THROWS_AS(result.get(), nx::task_cancelled);
}

TEST_CASE(
    "cancellation: continuations propagate cancellation without running") {
  nx::event release_blocker(true);
  std::atomic<bool> blocker_started{false};
  std::atomic<bool> continued{false};
  nx::thread_pool::config cfg;
  cfg.thread_count = 1;
  nx::thread_pool pool(cfg);

  pool.submit([&] {
    blocker_started.store(true, std::memory_order_release);
    release_blocker.wait();
  });
  while (!blocker_started.load(std::memory_order_acquire))
    nx::this_thread::yield();

  nx::cancellation_source source;
  auto result =
      std::move(pool.async(source.token(), [] { return 1; })).then([&](int) {
        continued.store(true, std::memory_order_release);
        return 2;
      });
  source.request_stop();
  release_blocker.set();

  CHECK_THROWS_AS(result.get(), nx::task_cancelled);
  CHECK_FALSE(continued.load(std::memory_order_acquire));
}

TEST_CASE("future: when_all preserves input order") {
  nx::thread_pool pool(small_config());
  nx::vector<nx::future<int>> inputs;
  for (int i = 0; i < 8; ++i) {
    inputs.push_back(pool.async([i] {
      nx::this_thread::sleep_for(std::chrono::milliseconds(8 - i));
      return i * 3;
    }));
  }

  nx::vector<int> values = nx::when_all(pool, std::move(inputs)).get();
  REQUIRE(values.size() == 8);
  for (int i = 0; i < 8; ++i)
    CHECK(values[static_cast<usize>(i)] == i * 3);
}

TEST_CASE("future: when_all handles void, empty and failed inputs") {
  nx::thread_pool pool(small_config());
  std::atomic<int> ran{0};
  nx::vector<nx::future<void>> voids;
  for (int i = 0; i < 8; ++i)
    voids.push_back(
        pool.async([&] { ran.fetch_add(1, std::memory_order_relaxed); }));
  nx::when_all(pool, std::move(voids)).get();
  CHECK(ran.load(std::memory_order_relaxed) == 8);

  nx::vector<nx::future<int>> empty;
  CHECK(nx::when_all(pool, std::move(empty)).get().empty());

  nx::vector<nx::future<int>> failed;
  failed.push_back(pool.async([] { return 1; }));
  failed.push_back(pool.async(
      []() -> int { throw std::range_error("expected aggregate failure"); }));
  CHECK_THROWS_AS(nx::when_all(pool, std::move(failed)).get(),
                  std::range_error);
}

TEST_CASE("future: when_any returns the first completed input") {
  nx::thread_pool pool(small_config());
  nx::event slow_gate(true);
  nx::event fast_gate(true);
  nx::vector<nx::future<int>> inputs;
  inputs.push_back(pool.async([&] {
    slow_gate.wait();
    return 10;
  }));
  inputs.push_back(pool.async([&] {
    fast_gate.wait();
    return 20;
  }));

  auto first = nx::when_any(pool, std::move(inputs));
  fast_gate.set();
  while (!first.ready())
    nx::this_thread::yield();
  slow_gate.set();

  const nx::when_any_result<int> value = first.get();
  CHECK(value.index == 1);
  CHECK(value.value == 20);
  pool.wait_idle();
}

TEST_CASE("future: when_any propagates the first terminal failure") {
  nx::thread_pool pool(small_config());
  nx::event slow_gate(true);
  nx::event failed_gate(true);
  nx::vector<nx::future<void>> inputs;
  inputs.push_back(pool.async([&] { slow_gate.wait(); }));
  inputs.push_back(pool.async([&] {
    failed_gate.wait();
    throw std::logic_error("first completion failed");
  }));

  auto first = pool.when_any(std::move(inputs));
  failed_gate.set();
  while (!first.ready())
    nx::this_thread::yield();
  slow_gate.set();

  CHECK_THROWS_AS(first.get(), std::logic_error);
  pool.wait_idle();
}

TEST_CASE("thread_pool: background concurrency leaves critical capacity") {
  nx::thread_pool::config cfg;
  cfg.thread_count = 3;
  cfg.max_background_concurrency = 1;
  cfg.collect_statistics = true;
  cfg.collect_timings = true;
  nx::thread_pool pool(cfg);
  nx::event release_background(true);
  nx::event critical_done(true);
  std::atomic<int> background_started{0};

  for (int i = 0; i < 4; ++i) {
    pool.submit(nx::task_priority::Background, [&] {
      background_started.fetch_add(1, std::memory_order_release);
      release_background.wait();
    });
  }
  while (background_started.load(std::memory_order_acquire) == 0)
    nx::this_thread::yield();
  nx::this_thread::sleep_for(5ms);
  CHECK(background_started.load(std::memory_order_acquire) == 1);

  pool.submit(nx::task_priority::Critical, [&] { critical_done.set(); });
  critical_done.wait();
  release_background.set();
  pool.wait_idle();

  const nx::thread_pool_statistics stats = pool.statistics();
  const auto &background =
      stats.lanes[nx::task_priority_index(nx::task_priority::Background)];
  const auto &critical =
      stats.lanes[nx::task_priority_index(nx::task_priority::Critical)];
  CHECK(stats.enabled);
  CHECK(stats.timings_enabled);
  CHECK(stats.background_limit == 1);
  CHECK(stats.background_active == 0);
  CHECK(background.submitted == 4);
  CHECK(background.completed == 4);
  CHECK(background.peak_active == 1);
  CHECK(background.execution_ns > 0);
  CHECK(critical.submitted == 1);
  CHECK(critical.completed == 1);
  CHECK(background.queued == 0);
  CHECK(critical.queued == 0);
}

TEST_CASE("thread_pool: background throttling permits nested group progress") {
  nx::thread_pool::config cfg;
  cfg.thread_count = 2;
  cfg.max_background_concurrency = 1;
  nx::thread_pool pool(cfg);
  nx::task_group outer;
  std::atomic<int> children{0};

  pool.submit(outer, nx::task_priority::Background, [&] {
    nx::task_group inner;
    for (int i = 0; i < 16; ++i) {
      pool.submit(inner, nx::task_priority::Background,
                  [&] { children.fetch_add(1, std::memory_order_relaxed); });
    }
    inner.wait();
  });

  outer.wait();
  CHECK(children.load(std::memory_order_relaxed) == 16);
  CHECK(pool.statistics().background_active == 0);
}

TEST_CASE("thread_pool: critical work overtakes queued background work") {
  nx::thread_pool::config cfg;
  cfg.thread_count = 1;
  nx::thread_pool pool(cfg);
  nx::event gate(true);
  std::atomic<bool> blocker_started{false};
  std::atomic<int> sequence{0};
  std::atomic<int> critical_order{0};
  std::atomic<int> background_order{0};

  pool.submit([&] {
    blocker_started.store(true, std::memory_order_release);
    gate.wait();
  });
  while (!blocker_started.load(std::memory_order_acquire))
    nx::this_thread::yield();

  pool.submit(nx::task_priority::Background, [&] {
    background_order.store(sequence.fetch_add(1) + 1,
                           std::memory_order_release);
  });
  pool.submit(nx::task_priority::Critical, [&] {
    critical_order.store(sequence.fetch_add(1) + 1, std::memory_order_release);
  });
  gate.set();

  while (sequence.load(std::memory_order_acquire) != 2)
    nx::this_thread::yield();
  pool.wait_idle();
  CHECK(critical_order.load() == 1);
  CHECK(background_order.load() == 2);
}

TEST_CASE("future: destroys a non-trivial result that was never collected") {
  struct Tracked {
    static std::atomic<int> &live() {
      static std::atomic<int> n{0};
      return n;
    }
    int value;
    explicit Tracked(int v) : value(v) { live().fetch_add(1); }
    Tracked(const Tracked &o) : value(o.value) { live().fetch_add(1); }
    Tracked(Tracked &&o) noexcept : value(o.value) { live().fetch_add(1); }
    ~Tracked() { live().fetch_sub(1); }
  };

  {
    nx::thread_pool pool(small_config());
    for (int i = 0; i < 200; ++i) {
      auto f = pool.async([i] { return Tracked{i}; });
      if (i % 2 == 0)
        CHECK(f.get().value == i);
    }
    pool.wait_idle();
  }
  CHECK(Tracked::live().load() == 0);
}

TEST_CASE("parallel_for: visits every index exactly once") {
  nx::thread_pool pool(small_config());
  constexpr usize n = 20000;
  std::vector<std::atomic<int>> hits(n);
  for (auto &h : hits)
    h.store(0, std::memory_order_relaxed);

  pool.parallel_for(
      0, n, [&](usize i) { hits[i].fetch_add(1, std::memory_order_relaxed); });

  for (usize i = 0; i < n; ++i)
    REQUIRE(hits[i].load() == 1);
}

TEST_CASE("parallel_for: honours a non-zero start and an explicit grain") {
  nx::thread_pool pool(small_config());
  constexpr usize begin = 1000, end = 5000;
  std::vector<std::atomic<int>> hits(end);
  for (auto &h : hits)
    h.store(0, std::memory_order_relaxed);

  pool.parallel_for(begin, end, 64, [&](usize i) {
    hits[i].fetch_add(1, std::memory_order_relaxed);
  });

  for (usize i = 0; i < end; ++i)
    REQUIRE(hits[i].load() == (i >= begin ? 1 : 0));

  std::atomic<int> ran{0};
  pool.parallel_for(
      0, 10, 1000, [&](usize) { ran.fetch_add(1, std::memory_order_relaxed); });
  CHECK(ran.load() == 10);

  std::vector<std::atomic<int>> fine(500);
  for (auto &h : fine)
    h.store(0, std::memory_order_relaxed);
  pool.parallel_for(0, 500, 1, [&](usize i) {
    fine[i].fetch_add(1, std::memory_order_relaxed);
  });
  for (usize i = 0; i < 500; ++i)
    REQUIRE(fine[i].load() == 1);
}

TEST_CASE("parallel_for: an empty or inverted range does nothing") {
  nx::thread_pool pool(small_config());
  std::atomic<int> ran{0};

  pool.parallel_for(0, 0, [&](usize) { ran.fetch_add(1); });
  pool.parallel_for(10, 10, [&](usize) { ran.fetch_add(1); });
  pool.parallel_for(10, 5, [&](usize) { ran.fetch_add(1); });
  CHECK(ran.load() == 0);

  pool.parallel_for(7, 8, [&](usize i) {
    CHECK(i == 7);
    ran.fetch_add(1);
  });
  CHECK(ran.load() == 1);
}

TEST_CASE("parallel_for: blocks until every chunk has finished") {
  nx::thread_pool pool(small_config());
  constexpr usize n = 4000;
  std::vector<int> data(n, 0);

  pool.parallel_for(0, n, [&](usize i) { data[i] = static_cast<int>(i) + 1; });

  for (usize i = 0; i < n; ++i)
    REQUIRE(data[i] == static_cast<int>(i) + 1);
  CHECK(std::accumulate(data.begin(), data.end(), i64{0}) ==
        static_cast<i64>(n) * (n + 1) / 2);
}

TEST_CASE("parallel_for: nested inside a pool task completes") {
  nx::thread_pool pool(small_config());
  std::atomic<i64> total{0};

  nx::task_group group;
  for (int outer = 0; outer < 8; ++outer) {
    pool.submit(group, [&] {
      pool.parallel_for(0, 500, [&](usize) {
        total.fetch_add(1, std::memory_order_relaxed);
      });
    });
  }
  group.wait();
  CHECK(total.load() == 8 * 500);
}

TEST_CASE("parallel_for: a zero-worker pool still runs the body serially") {
  nx::thread_pool::config cfg;
  cfg.thread_count = 1;
  nx::thread_pool pool(cfg);

  constexpr usize n = 1000;
  std::vector<int> data(n, 0);
  pool.parallel_for(0, n, [&](usize i) { data[i] = 1; });
  for (usize i = 0; i < n; ++i)
    REQUIRE(data[i] == 1);
}

TEST_CASE(
    "parallel_region: runs one body per participant with unique indices") {
  nx::thread_pool pool(small_config());
  const u32 expected = pool.worker_count() + 1;

  std::vector<std::atomic<int>> seen(expected + 4);
  for (auto &s : seen)
    s.store(0, std::memory_order_relaxed);
  std::atomic<u32> reported{0};

  pool.parallel_region(0, [&](u32 index, u32 count, nx::fiber::barrier &) {
    reported.store(count, std::memory_order_relaxed);
    seen[index].fetch_add(1, std::memory_order_relaxed);
  });

  CHECK(reported.load() == expected);
  for (u32 i = 0; i < expected; ++i)
    REQUIRE(seen[i].load() == 1);
  for (u32 i = expected; i < seen.size(); ++i)
    REQUIRE(seen[i].load() == 0);
}

TEST_CASE("parallel_region: clamps the team to the pool size") {
  nx::thread_pool pool(small_config());
  std::atomic<u32> reported{0};

  pool.parallel_region(1000, [&](u32, u32 count, nx::fiber::barrier &) {
    reported.store(count, std::memory_order_relaxed);
  });
  CHECK(reported.load() == pool.worker_count() + 1);
}

TEST_CASE("parallel_region: a single participant runs inline on the caller") {
  nx::thread_pool pool(small_config());
  const u64 caller = nx::this_thread::id();

  std::atomic<int> ran{0};
  u64 body_thread = 0;
  pool.parallel_region(1, [&](u32 index, u32 count, nx::fiber::barrier &bar) {
    CHECK(index == 0u);
    CHECK(count == 1u);
    body_thread = nx::this_thread::id();
    bar.arrive_and_wait();
    ran.fetch_add(1, std::memory_order_relaxed);
  });

  CHECK(ran.load() == 1);
  CHECK(body_thread == caller);
}

TEST_CASE("parallel_region: the barrier synchronises every phase") {
  nx::thread_pool pool(small_config());
  const u32 parts = pool.worker_count() + 1;
  constexpr int PHASES = 200;

  std::vector<std::atomic<int>> stage(parts);
  for (auto &s : stage)
    s.store(-1, std::memory_order_relaxed);
  std::atomic<int> violations{0};

  pool.parallel_region(0, [&](u32 index, u32 count, nx::fiber::barrier &bar) {
    for (int p = 0; p < PHASES; ++p) {
      stage[index].store(p, std::memory_order_release);
      bar.arrive_and_wait();
      for (u32 j = 0; j < count; ++j)
        if (stage[j].load(std::memory_order_acquire) != p)
          violations.fetch_add(1, std::memory_order_relaxed);
      bar.arrive_and_wait();
    }
  });

  CHECK(violations.load() == 0);
}

TEST_CASE("parallel_region: chunks partition a data-parallel reduction") {
  nx::thread_pool pool(small_config());
  constexpr usize N = 100000;
  std::vector<int> data(N, 1);
  std::atomic<i64> total{0};

  pool.parallel_region(0, [&](u32 index, u32 count, nx::fiber::barrier &) {
    const usize per = (N + count - 1) / count;
    const usize begin = nx::min(static_cast<usize>(index) * per, N);
    const usize end = nx::min(begin + per, N);
    i64 sum = 0;
    for (usize i = begin; i < end; ++i)
      sum += data[i];
    total.fetch_add(sum, std::memory_order_relaxed);
  });

  CHECK(total.load() == static_cast<i64>(N));
}

TEST_CASE("parallel_region: back-to-back regions each field a full team") {
  nx::thread_pool pool(small_config());
  const u32 parts = pool.worker_count() + 1;

  for (int round = 0; round < 20; ++round) {
    std::atomic<int> arrived{0};
    pool.parallel_region(0, [&](u32, u32, nx::fiber::barrier &bar) {
      arrived.fetch_add(1, std::memory_order_relaxed);
      bar.arrive_and_wait();
    });
    REQUIRE(arrived.load() == static_cast<int>(parts));
  }
}

TEST_CASE("parallel_region: a team started from inside a pool task meets at "
          "its barrier") {
  nx::thread_pool pool(small_config());
  const u32 parts = pool.worker_count() + 1;
  std::atomic<int> arrived{0}, passed{0};
  nx::task_group group;

  pool.submit(group, [&] {
    for (int phase = 0; phase < 3; ++phase)
      pool.parallel_region(0, [&](u32, u32, nx::fiber::barrier &bar) {
        arrived.fetch_add(1, std::memory_order_relaxed);
        bar.arrive_and_wait();
        passed.fetch_add(1, std::memory_order_relaxed);
      });
  });
  group.wait();

  CHECK(arrived.load() == static_cast<int>(3 * parts));
  CHECK(passed.load() == static_cast<int>(3 * parts));
}

TEST_CASE("thread_pool: mixes submit, groups, futures and parallel_for") {
  nx::thread_pool pool(small_config());
  std::atomic<i64> counter{0};
  std::vector<nx::future<int>> futures;
  nx::task_group group;

  for (int round = 0; round < 20; ++round) {
    for (int i = 0; i < 50; ++i)
      pool.submit([&] { counter.fetch_add(1, std::memory_order_relaxed); });
    for (int i = 0; i < 50; ++i)
      pool.submit(group,
                  [&] { counter.fetch_add(1, std::memory_order_relaxed); });
    futures.push_back(pool.async([round] { return round; }));
    pool.parallel_for(0, 200, [&](usize) {
      counter.fetch_add(1, std::memory_order_relaxed);
    });
  }

  group.wait();
  for (int round = 0; round < 20; ++round)
    REQUIRE(futures[static_cast<usize>(round)].get() == round);
  pool.wait_idle();

  CHECK(counter.load() == 20LL * (50 + 50 + 200));
}

TEST_CASE("thread_pool: several pools coexist without interfering") {
  nx::thread_pool a(small_config());
  nx::thread_pool b(small_config());
  std::atomic<int> ran_a{0}, ran_b{0};

  for (int i = 0; i < 1000; ++i) {
    a.submit([&] { ran_a.fetch_add(1, std::memory_order_relaxed); });
    b.submit([&] { ran_b.fetch_add(1, std::memory_order_relaxed); });
  }
  a.wait_idle();
  b.wait_idle();

  CHECK(ran_a.load() == 1000);
  CHECK(ran_b.load() == 1000);
}

TEST_CASE("thread_pool: is neither copyable nor movable") {
  static_assert(!std::is_copy_constructible_v<nx::thread_pool>);
  static_assert(!std::is_copy_assignable_v<nx::thread_pool>);
  static_assert(!std::is_copy_constructible_v<nx::task_group>);
  CHECK(true);
}

TEST_CASE("completion_source: external completion uses ordinary futures") {
  nx::thread_pool pool(small_config());
  auto source = nx::make_shared<nx::completion_source<int>>(&pool);
  REQUIRE(source != nullptr);
  REQUIRE(source->valid());
  auto input = source->get_future();
  REQUIRE(input.valid());
  CHECK_FALSE(source->get_future().valid());
  auto continued = std::move(input).then([](int value) { return value * 2; });
  REQUIRE(continued.valid());

  nx::thread producer([source] { source->set_value(21); });
  producer.join();
  CHECK(continued.get() == 42);
}

TEST_CASE("completion_source: abandoning a producer cancels its future") {
  nx::future<void> future;
  {
    nx::completion_source<void> source;
    future = source.get_future();
    REQUIRE(future.valid());
  }
  CHECK(future.cancelled());
  CHECK_THROWS_AS(future.get(), nx::task_cancelled);
}

TEST_CASE("thread_pool: a task blocking through run_blocking leaves its "
          "worker free") {
  nx::thread_pool pool({.thread_count = 1, .name = "nx-test-pool"});
  std::atomic<bool> blocking{false};
  std::atomic<bool> released{false};
  bool saw_release = false;
  nx::task_group group;
  pool.submit(group, [&] {
    nx::fiber::run_blocking([&] {
      blocking.store(true);
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while (!released.load() && std::chrono::steady_clock::now() < deadline)
        nx::this_thread::sleep_for(100us);
      saw_release = released.load();
    });
  });
  while (!blocking.load())
    nx::this_thread::yield();
  pool.submit(group, [&] { released.store(true); });
  group.wait();
  CHECK(saw_release);
}

TEST_CASE("thread_pool: a task's run_on_main call runs on the main thread") {
  nx::thread_pool::config cfg = small_config();
  cfg.adopt_main_thread = true;
  nx::thread_pool pool(cfg);
  REQUIRE(pool.has_main_thread());
  const u64 main_id = nx::this_thread::id();

  std::atomic<u64> before{0};
  std::atomic<u64> inside{0};
  std::atomic<u64> after{0};
  nx::task_group group;
  pool.submit(group, [&] {
    before = nx::this_thread::id();
    pool.run_on_main([&] { inside = nx::this_thread::id(); });
    after = nx::this_thread::id();
  });
  group.wait();

  CHECK(inside.load() == main_id);
  CHECK(before.load() != main_id);
  CHECK(after.load() != main_id);
}

TEST_CASE("thread_pool: run_on_main from another thread waits for pump_main") {
  nx::thread_pool::config cfg = small_config();
  cfg.adopt_main_thread = true;
  nx::thread_pool pool(cfg);
  const u64 main_id = nx::this_thread::id();

  std::atomic<u64> inside{0};
  std::atomic<bool> returned{false};
  nx::thread caller([&] {
    pool.run_on_main([&] { inside = nx::this_thread::id(); });
    returned = true;
  });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!returned && std::chrono::steady_clock::now() < deadline)
    pool.pump_main(1ms);
  caller.join();

  CHECK(returned.load());
  CHECK(inside.load() == main_id);
}

TEST_CASE("thread_pool: run_on_main runs in place without a main thread") {
  nx::thread_pool pool(small_config());
  CHECK_FALSE(pool.has_main_thread());
  std::atomic<u64> inside{0};
  std::atomic<u64> caller{0};
  nx::task_group group;
  pool.submit(group, [&] {
    caller = nx::this_thread::id();
    pool.run_on_main([&] { inside = nx::this_thread::id(); });
  });
  group.wait();
  CHECK(inside.load() == caller.load());

  bool ran = false;
  pool.run_on_main([&] { ran = true; });
  CHECK(ran);
}

TEST_CASE(
    "thread_pool: an exception thrown on the main thread reaches the task") {
  nx::thread_pool::config cfg = small_config();
  cfg.adopt_main_thread = true;
  nx::thread_pool pool(cfg);
  const u64 main_id = nx::this_thread::id();

  std::atomic<bool> caught{false};
  std::atomic<u64> after{0};
  nx::task_group group;
  pool.submit(group, [&] {
    try {
      pool.run_on_main([] { throw std::runtime_error("from main"); });
    } catch (const std::runtime_error &e) {
      caught = std::string(e.what()) == "from main";
    }
    after = nx::this_thread::id();
  });
  group.wait();
  CHECK(caught.load());
  CHECK(after.load() != main_id);
}

TEST_CASE("thread_pool: worker_os_ids names every thread its tasks run on") {
  nx::thread_pool pool(small_config());
  std::vector<u64> ids(pool.worker_count());
  for (u32 i = 0; i < 1000 && pool.worker_os_ids(ids) != ids.size(); ++i)
    nx::this_thread::sleep_for(1ms);
  REQUIRE(pool.worker_os_ids(ids) == ids.size());

  const std::set<u64> named(ids.begin(), ids.end());
  CHECK(named.size() == ids.size());
  CHECK(named.count(0u) == 0u);
  // The pool did not adopt this thread, so it runs no tasks and is not named.
  CHECK(named.count(nx::this_thread::os_id()) == 0u);

  std::vector<u64> ran(256, 0u);
  nx::task_group group;
  for (usize i = 0; i < ran.size(); ++i)
    pool.submit(group, [&ran, i] {
      ran[i] = nx::this_thread::os_id();
      nx::this_thread::sleep_for(50us);
    });
  group.wait();
  for (const u64 thread : ran)
    CHECK(named.count(thread) == 1u);

  std::vector<u64> tiny(1);
  CHECK(pool.worker_os_ids(tiny) == 1u);
}

TEST_CASE("parallel_for: a chunk's exception reaches the caller") {
  nx::thread_pool pool(small_config());
  REQUIRE(pool.worker_count() > 0);
  std::atomic<u32> ran{0};
  CHECK_THROWS_AS(pool.parallel_for(0, 4096, 16,
                                    [&](const usize i) {
                                      ran.fetch_add(1);
                                      if (i == 2000)
                                        throw std::runtime_error("chunk");
                                    }),
                  std::runtime_error);
  CHECK(ran.load() <= 4096u);

  std::atomic<u32> after{0};
  pool.parallel_for(0, 4096, 16, [&](usize) { after.fetch_add(1); });
  CHECK(after.load() == 4096u);
}

TEST_CASE("parallel_region: a participant that throws does not strand the "
          "others at the barrier") {
  nx::thread_pool pool(small_config());
  const u32 expected = pool.worker_count() + 1;
  REQUIRE(expected > 2);

  std::atomic<u32> finished{0};
  CHECK_THROWS_AS(
      pool.parallel_region(0,
                           [&](const u32 index, u32, nx::fiber::barrier &bar) {
                             if (index == 0)
                               throw std::logic_error("caller");
                             bar.arrive_and_wait();
                             if (index == 1)
                               throw std::logic_error("worker");
                             bar.arrive_and_wait();
                             finished.fetch_add(1);
                           }),
      std::logic_error);
  CHECK(finished.load() == expected - 2);
}
