#include "framework/nxtest.h"

#include "core/foundation/threading/sync.h"
#include "core/foundation/threading/thread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <string>
#include <vector>

using namespace std::chrono_literals;

TEST_CASE("thread: a default-constructed thread is not joinable") {
  nx::thread t;
  CHECK_FALSE(t.joinable());
  CHECK(t.id() == 0);
  CHECK(t.native_handle() == nullptr);
}

TEST_CASE("thread: runs the entry function and join waits for it") {
  std::atomic<bool> ran{false};
  nx::thread t([&] { ran.store(true, std::memory_order_release); });

  CHECK(t.joinable());
  CHECK(t.id() != 0);
  CHECK(t.native_handle() != nullptr);

  t.join();
  CHECK_FALSE(t.joinable());
  CHECK(ran.load(std::memory_order_acquire));
}

TEST_CASE("thread: join observes everything the thread wrote") {
  constexpr int n = 10000;
  std::vector<int> data;
  nx::thread t([&] {
    for (int i = 0; i < n; ++i)
      data.push_back(i);
  });
  t.join();

  REQUIRE(data.size() == n);
  CHECK(data.front() == 0);
  CHECK(data.back() == n - 1);
}

TEST_CASE("thread: accepts a plain lambda, an nx::function and captures") {
  std::atomic<int> sum{0};

  nx::thread a([&] { sum.fetch_add(1, std::memory_order_relaxed); });
  a.join();

  nx::thread::entry_fn fn{[&] { sum.fetch_add(2, std::memory_order_relaxed); }};
  nx::thread b(std::move(fn));
  b.join();

  int captured = 4;
  nx::thread c(
      [&sum, captured] { sum.fetch_add(captured, std::memory_order_relaxed); });
  c.join();

  CHECK(sum.load() == 7);
}

TEST_CASE("thread: the destructor joins a still-joinable thread") {
  std::atomic<bool> finished{false};
  {
    nx::thread t([&] {
      nx::this_thread::sleep_for(30ms);
      finished.store(true, std::memory_order_release);
    });
    CHECK(t.joinable());
  }
  CHECK(finished.load(std::memory_order_acquire));
}

TEST_CASE("thread: move construction and assignment transfer ownership") {
  std::atomic<int> ran{0};

  nx::thread a([&] { ran.fetch_add(1, std::memory_order_relaxed); });
  const u64 id = a.id();
  void *handle = a.native_handle();

  nx::thread b = std::move(a);
  CHECK_FALSE(a.joinable());
  CHECK(a.id() == 0);
  CHECK(b.joinable());
  CHECK(b.id() == id);
  CHECK(b.native_handle() == handle);
  b.join();
  CHECK(ran.load() == 1);

  std::atomic<bool> first_done{false};
  nx::thread c([&] {
    nx::this_thread::sleep_for(20ms);
    first_done.store(true, std::memory_order_release);
  });
  nx::thread d([&] { ran.fetch_add(1, std::memory_order_relaxed); });
  c = std::move(d);
  CHECK(
      first_done.load(std::memory_order_acquire));
  CHECK_FALSE(d.joinable());
  c.join();
  CHECK(ran.load() == 2);

  nx::thread e([&] { ran.fetch_add(1, std::memory_order_relaxed); });
  nx::thread *alias = &e;
  e = std::move(*alias);
  CHECK(e.joinable());
  e.join();
  CHECK(ran.load() == 3);
}

TEST_CASE("thread: detach releases ownership and the thread keeps running") {
  nx::semaphore done(0);
  {
    nx::thread t([&] {
      nx::this_thread::sleep_for(10ms);
      done.release();
    });
    t.detach();
    CHECK_FALSE(t.joinable());
    CHECK(t.native_handle() == nullptr);
  }
  CHECK(done.try_acquire_for(5s));
}

TEST_CASE("thread: is move-only") {
  static_assert(!std::is_copy_constructible_v<nx::thread>);
  static_assert(!std::is_copy_assignable_v<nx::thread>);
  static_assert(std::is_move_constructible_v<nx::thread>);
  static_assert(std::is_move_assignable_v<nx::thread>);
  CHECK(true);
}

TEST_CASE("thread: owner and self ids agree and are unique while live") {
  constexpr int count = 8;
  std::vector<nx::thread> threads;
  std::vector<u64> owner_ids(count, 0);
  std::vector<u64> self_ids(count, 0);
  nx::barrier gate(count);

  for (int i = 0; i < count; ++i) {
    threads.emplace_back([&, i] {
      self_ids[static_cast<usize>(i)] = nx::this_thread::id();
      gate.arrive_and_wait();
    });
    owner_ids[static_cast<usize>(i)] = threads.back().id();
  }
  for (nx::thread &t : threads)
    t.join();

  const std::set<u64> unique_owner(owner_ids.begin(), owner_ids.end());
  const std::set<u64> unique_self(self_ids.begin(), self_ids.end());
  CHECK(unique_owner.size() == count);
  CHECK(unique_self.size() == count);
  for (int i = 0; i < count; ++i) {
    CHECK(self_ids[static_cast<usize>(i)] != 0);
    CHECK(owner_ids[static_cast<usize>(i)] ==
          self_ids[static_cast<usize>(i)]);
  }
  CHECK(unique_self.count(nx::this_thread::id()) == 0);
}

TEST_CASE("thread: options are accepted and the thread still runs") {
  std::atomic<bool> ran{false};
  nx::thread_options opts;
  opts.name = "nx-test-worker";
  opts.stack_size = 256 * 1024;
  opts.priority = nx::thread_priority::BelowNormal;

  nx::thread t([&] { ran.store(true, std::memory_order_release); }, opts);
  t.join();
  CHECK(ran.load(std::memory_order_acquire));

  std::atomic<bool> pinned_ran{false};
  nx::thread_options pinned;
  pinned.name = "nx-test-pinned";
  pinned.affinity_mask = 1ull;
  nx::thread p([&] { pinned_ran.store(true, std::memory_order_release); },
               pinned);
  p.join();
  CHECK(pinned_ran.load(std::memory_order_acquire));

  for (const nx::thread_priority prio :
       {nx::thread_priority::Low, nx::thread_priority::BelowNormal,
        nx::thread_priority::Normal, nx::thread_priority::AboveNormal,
        nx::thread_priority::High, nx::thread_priority::Realtime}) {
    std::atomic<bool> ok{false};
    nx::thread_options o;
    o.priority = prio;
    nx::thread x([&] { ok.store(true, std::memory_order_release); }, o);
    x.join();
    CHECK(ok.load(std::memory_order_acquire));
  }
}

TEST_CASE("thread: many short-lived threads all run to completion") {
  constexpr int count = 64;
  std::atomic<int> completed{0};
  for (int i = 0; i < count; ++i) {
    nx::thread t([&] { completed.fetch_add(1, std::memory_order_relaxed); });
    t.join();
  }
  CHECK(completed.load() == count);
}

TEST_CASE("this_thread: id is stable, non-zero and per-thread") {
  const u64 mine = nx::this_thread::id();
  CHECK(mine != 0);
  CHECK(nx::this_thread::id() == mine);

  std::atomic<u64> other{0};
  nx::thread t([&] { other.store(nx::this_thread::id()); });
  t.join();

  CHECK(other.load() != 0);
  CHECK(other.load() != mine);
  CHECK(nx::this_thread::id() == mine);
}

TEST_CASE("this_thread: yield is callable and harmless") {
  for (int i = 0; i < 1000; ++i)
    nx::this_thread::yield();
  CHECK(true);
}

TEST_CASE("this_thread: sleep_for waits at least the requested time") {
  const auto start = std::chrono::steady_clock::now();
  nx::this_thread::sleep_for(60ms);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK(elapsed >= 40ms);
  CHECK(elapsed < 5s);

  const auto s2 = std::chrono::steady_clock::now();
  nx::this_thread::sleep_for(0ms);
  nx::this_thread::sleep_for(std::chrono::nanoseconds(-1));
  CHECK(std::chrono::steady_clock::now() - s2 < 1s);

  nx::this_thread::sleep_for(std::chrono::microseconds(500));
  CHECK(true);
}

TEST_CASE("this_thread: set_priority reports the level it was granted") {
  std::atomic<bool> ran{false};
  std::atomic<bool> never_exceeded{true};
  std::atomic<bool> realtime_plausible{false};
  std::atomic<bool> restored{false};

  nx::thread t([&] {
    for (const nx::thread_priority prio :
         {nx::thread_priority::Low, nx::thread_priority::BelowNormal,
          nx::thread_priority::Normal, nx::thread_priority::AboveNormal,
          nx::thread_priority::High, nx::thread_priority::Realtime}) {
      const nx::thread_priority got = nx::this_thread::set_priority(prio);
      if (prio >= nx::thread_priority::Normal &&
          nx::cast<int>(got) > nx::cast<int>(prio))
        never_exceeded.store(false, std::memory_order_relaxed);
    }

    const nx::thread_priority rt =
        nx::this_thread::set_priority(nx::thread_priority::Realtime);
    realtime_plausible.store(rt == nx::thread_priority::Realtime ||
                                 rt == nx::thread_priority::High ||
                                 rt == nx::thread_priority::Normal,
                             std::memory_order_relaxed);

    restored.store(nx::this_thread::set_priority(nx::thread_priority::Normal) ==
                       nx::thread_priority::Normal,
                   std::memory_order_relaxed);
    ran.store(true, std::memory_order_release);
  });
  t.join();

  CHECK(ran.load(std::memory_order_acquire));
  CHECK(never_exceeded.load(std::memory_order_relaxed));
  CHECK(realtime_plausible.load(std::memory_order_relaxed));
  CHECK(restored.load(std::memory_order_relaxed));
}

TEST_CASE("this_thread: set_name, set_affinity and set_priority are callable") {
  std::atomic<bool> ok{false};
  nx::thread t([&] {
    nx::this_thread::set_name("nx-renamed");
    (void)nx::this_thread::set_affinity(1ull);
    (void)nx::this_thread::set_priority(nx::thread_priority::AboveNormal);
    (void)nx::this_thread::set_priority(nx::thread_priority::Normal);
    ok.store(true, std::memory_order_release);
  });
  t.join();
  CHECK(ok.load(std::memory_order_acquire));

  std::atomic<bool> ok2{false};
  nx::thread t2([&] {
    nx::this_thread::set_name(
        "an-extremely-long-thread-name-well-past-any-platform-limit");
    ok2.store(true, std::memory_order_release);
  });
  t2.join();
  CHECK(ok2.load(std::memory_order_acquire));
}

TEST_CASE("cpu: the core counts are plausible and mutually consistent") {
  const u32 logical = nx::cpu::logical_core_count();
  const u32 physical = nx::cpu::physical_core_count();

  CHECK(logical >= 1);
  CHECK(physical >= 1);
  CHECK(logical >= physical);
  CHECK(logical <= 4096);

  CHECK(nx::cpu::logical_core_count() == logical);
  CHECK(nx::cpu::physical_core_count() == physical);
}

TEST_CASE("cpu: cache_line_size is a sane power of two") {
  const u32 line = nx::cpu::cache_line_size();
  CHECK(line >= 16);
  CHECK(line <= 512);
  CHECK(nx::is_pow2(line));
  CHECK(nx::cpu::cache_line_size() == line);
}

TEST_CASE(
    "cpu: performance and efficiency cores partition the physical cores") {
  const u32 perf = nx::cpu::performance_core_count();
  const u32 eff = nx::cpu::efficiency_core_count();
  const u32 physical = nx::cpu::physical_core_count();

  CHECK(perf >= 1);
  CHECK(perf + eff == physical);
  CHECK(perf <= physical);
  CHECK(eff <= physical);
}

TEST_CASE("cpu: topology is internally consistent and cached") {
  const nx::cpu::topology_info &t = nx::cpu::topology();

  CHECK(&t == &nx::cpu::topology());
  CHECK(t.logical_count == nx::cpu::logical_core_count());
  CHECK(t.physical_count == nx::cpu::physical_core_count());
  CHECK(t.cache_line_size == nx::cpu::cache_line_size());
  CHECK(t.package_count >= 1);
  CHECK(t.numa_node_count >= 1);
  CHECK(t.efficiency_class_count >= 1);

  if (!t.cores.empty()) {
    CHECK(t.cores.size() == t.physical_count);

    u32 logical_total = 0;
    u64 seen_mask = 0;
    std::set<u32> indices;
    for (const nx::cpu::core_info &c : t.cores) {
      CHECK(c.logical_count >= 1);
      CHECK(c.efficiency_class < t.efficiency_class_count);
      CHECK(c.package < t.package_count);
      indices.insert(c.index);
      logical_total += c.logical_count;

      CHECK((seen_mask & c.logical_mask) == 0);
      seen_mask |= c.logical_mask;
    }
    CHECK(indices.size() == t.cores.size());
    if (t.logical_count <= 64)
      CHECK(logical_total == t.logical_count);
  }

  for (const nx::cpu::cache_info &c : t.caches) {
    CHECK(c.level >= 1);
    CHECK(c.level <= 3);
    CHECK(c.line_size >= 16);
    CHECK(c.line_size <= 512);
    CHECK(c.size > 0);
    CHECK(c.instance_count >= 1);
  }
}

TEST_CASE("cpu: topology can be queried concurrently without tearing") {
  constexpr int threads = 8;
  std::vector<const nx::cpu::topology_info *> seen(threads, nullptr);
  std::vector<u32> counts(threads, 0);
  nx::barrier gate(threads);

  std::vector<nx::thread> workers;
  for (int i = 0; i < threads; ++i) {
    workers.emplace_back([&, i] {
      gate.arrive_and_wait();
      seen[static_cast<usize>(i)] = &nx::cpu::topology();
      counts[static_cast<usize>(i)] = nx::cpu::logical_core_count();
    });
  }
  for (nx::thread &w : workers)
    w.join();

  for (int i = 0; i < threads; ++i) {
    CHECK(seen[static_cast<usize>(i)] == seen[0]);
    CHECK(counts[static_cast<usize>(i)] == counts[0]);
  }
  CHECK(counts[0] == nx::cpu::logical_core_count());
}

namespace {

template <typename F> std::chrono::nanoseconds median_of(u32 runs, F &&measure) {
  std::vector<std::chrono::nanoseconds> samples;
  for (u32 i = 0; i < runs; ++i)
    samples.push_back(measure());
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

}

TEST_CASE("thread_parker: an unpark before the park is not lost") {
  nx::thread_parker parker;
  parker.unpark();
  const auto start = std::chrono::steady_clock::now();
  parker.park_until(start + std::chrono::seconds(5));
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
}

TEST_CASE("thread_parker: unpark wakes a thread parked without a deadline") {
  nx::thread_parker parker;
  std::atomic<bool> woke{false};
  nx::thread sleeper([&] {
    parker.park();
    woke.store(true);
  });
  nx::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK_FALSE(woke.load());
  parker.unpark();
  sleeper.join();
  CHECK(woke.load());
}

// A futex timeout on Windows rounds to the ~15.6 ms system tick; the parker
// must not.
TEST_CASE("thread_parker: a short deadline is kept to well under a tick") {
  nx::thread_parker parker;
  const auto overshoot = median_of(21, [&] {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::microseconds(500);
    parker.park_until(deadline);
    return std::chrono::steady_clock::now() - deadline;
  });
  CHECK(overshoot >= std::chrono::nanoseconds(0));
  CHECK(overshoot < std::chrono::milliseconds(3));
}

TEST_CASE("this_thread: os_id is stable on a thread and differs between "
          "threads") {
  const u64 mine = nx::this_thread::os_id();
  CHECK(mine != 0u);
  CHECK(nx::this_thread::os_id() == mine);

  std::atomic<u64> theirs{0};
  nx::thread t([&] { theirs = nx::this_thread::os_id(); });
  t.join();
  CHECK(theirs.load() != 0u);
  CHECK(theirs.load() != mine);
}
