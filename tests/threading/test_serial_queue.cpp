#include "framework/nxtest.h"

#include "core/foundation/threading/serial_queue.h"
#include "core/foundation/threading/sync.h"
#include "core/foundation/threading/thread.h"

#include <atomic>
#include <chrono>

namespace {

nx::thread_pool::config pool_config(const u32 workers, const u32 blocking = 2) {
  nx::thread_pool::config cfg;
  cfg.thread_count = workers;
  cfg.blocking_threads = blocking;
  cfg.max_background_concurrency = workers;
  return cfg;
}

} // namespace

TEST_CASE("serial queue: tasks run one at a time, in the order posted") {
  nx::thread_pool pool(pool_config(4));
  nx::serial_queue queue(pool, {.capacity = 4096});
  nx::vector<u32> order;
  std::atomic<i32> inside{0};
  std::atomic<i32> overlaps{0};
  std::atomic<i32> on_workers{0};
  for (u32 i = 0; i < 1000; ++i)
    REQUIRE(queue.post([&, i] {
      if (inside.fetch_add(1) != 0)
        overlaps.fetch_add(1);
      // No thread of its own: the pool's workers run it.
      if (pool.is_worker_thread())
        on_workers.fetch_add(1);
      order.push_back(i);
      inside.fetch_sub(1);
    }));
  queue.flush();
  CHECK(overlaps.load() == 0);
  CHECK(on_workers.load() == 1000);
  REQUIRE(order.size() == usize{1000});
  bool in_order = true;
  for (u32 i = 0; i < 1000; ++i)
    in_order = in_order && order[i] == i;
  CHECK(in_order);
  CHECK(queue.pending() == 0u);
}

TEST_CASE("serial queue: posts from several threads keep each one's order") {
  nx::thread_pool pool(pool_config(4));
  nx::serial_queue queue(pool, {.capacity = 4096});
  constexpr u32 PER = 300;
  nx::vector<u32> seen;
  std::atomic<i32> inside{0};
  std::atomic<i32> overlaps{0};
  {
    nx::vector<nx::thread> posters;
    for (u32 t = 0; t < 3; ++t)
      posters.emplace_back(nx::thread::entry_fn([&, t] {
        for (u32 i = 0; i < PER; ++i)
          (void)queue.post([&, t, i] {
            if (inside.fetch_add(1) != 0)
              overlaps.fetch_add(1);
            seen.push_back(t * 10000 + i);
            inside.fetch_sub(1);
          });
      }));
  }
  queue.flush();
  CHECK(overlaps.load() == 0);
  REQUIRE(seen.size() == usize{3 * PER});
  u32 next[3] = {};
  bool in_order = true;
  for (const u32 value : seen) {
    const u32 poster = value / 10000;
    in_order = in_order && value % 10000 == next[poster];
    ++next[poster];
  }
  CHECK(in_order);
}

TEST_CASE("serial queue: blocking tasks leave the worker free for other "
          "jobs") {
  // One worker: were the task to sleep on it, nothing else could run.
  nx::thread_pool pool(pool_config(1, 1));
  nx::serial_queue queue(pool, {.blocking = true});
  std::atomic<bool> sleeping{false};
  std::atomic<bool> slept{false};
  REQUIRE(queue.post([&] {
    sleeping.store(true);
    nx::this_thread::sleep_for(std::chrono::milliseconds(300));
    slept.store(true);
  }));
  while (!sleeping.load())
    nx::this_thread::sleep_for(std::chrono::milliseconds(1));
  auto other = pool.async([&] { return slept.load(); });
  CHECK_FALSE(other.get());
  queue.flush();
  CHECK(slept.load());
}

TEST_CASE("serial queue: a full queue refuses, unless told to go over") {
  nx::thread_pool pool(pool_config(2));
  nx::serial_queue queue(pool, {.capacity = 3});
  nx::event release;
  std::atomic<bool> started{false};
  std::atomic<u32> ran{0};
  REQUIRE(queue.post([&] {
    started.store(true);
    release.wait();
    ran.fetch_add(1);
  }));
  // Once the first task runs, the queue holds what follows.
  while (!started.load())
    nx::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(queue.post([&] { ran.fetch_add(1); }));
  CHECK(queue.post([&] { ran.fetch_add(1); }));
  CHECK(queue.post([&] { ran.fetch_add(1); }));
  CHECK_FALSE(queue.post([&] { ran.fetch_add(1); }));
  CHECK(queue.post([&] { ran.fetch_add(1); }, true));
  CHECK(queue.pending() == 5u);
  release.set();
  queue.flush();
  CHECK(ran.load() == 5u);
}

TEST_CASE("serial queue: closing keeps what was queued, and going waits for "
          "it") {
  nx::thread_pool pool(pool_config(2));
  std::atomic<u32> ran{0};
  {
    nx::serial_queue queue(pool, {});
    for (int i = 0; i < 50; ++i)
      REQUIRE(queue.post([&] {
        nx::this_thread::sleep_for(std::chrono::microseconds(200));
        ran.fetch_add(1);
      }));
    queue.close();
    CHECK(queue.closed());
    CHECK_FALSE(queue.post([&] { ran.fetch_add(100); }));
    CHECK_FALSE(queue.post([&] { ran.fetch_add(100); }, true));
  }
  CHECK(ran.load() == 50u);
}
