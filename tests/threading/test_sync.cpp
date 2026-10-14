#include "framework/nxtest.h"

#include "core/foundation/threading/sync.h"
#include "core/foundation/threading/thread.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

int worker_count() {
  const unsigned hc = std::thread::hardware_concurrency();
  const int n = hc == 0 ? 4 : static_cast<int>(hc);
  return n < 2 ? 2 : (n > 8 ? 8 : n);
}

template <typename Pred>
bool wait_until_true(Pred pred, std::chrono::milliseconds limit = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::yield();
  }
  return true;
}

}

TEST_CASE(
    "futex: wait returns when the word changes and wake reports a count") {
  std::atomic<u32> word{0};

  const auto r = nx::futex::wait(&word, 1, nx::detail::WAIT_FOREVER);
  CHECK(r != nx::futex::Result::Error);

  CHECK(nx::futex::wake(&word, 1) >= 0);
  CHECK(nx::futex::wake_all(&word) >= 0);

  std::atomic<bool> woke{false};
  std::thread waiter([&] {
    while (word.load(std::memory_order_acquire) == 0)
      nx::futex::wait(&word, 0, nx::detail::WAIT_FOREVER);
    woke.store(true, std::memory_order_release);
  });

  std::this_thread::sleep_for(20ms);
  word.store(1, std::memory_order_release);
  nx::futex::wake_all(&word);
  waiter.join();
  CHECK(woke.load());
}

TEST_CASE("futex: a timed wait reports TimedOut and actually waits") {
  std::atomic<u32> word{0};
  const auto start = std::chrono::steady_clock::now();
  const auto r = nx::futex::wait(&word, 0, 50ms);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  CHECK(r == nx::futex::Result::TimedOut);
  CHECK(elapsed >= 20ms);
}

TEST_CASE("spin_lock: lock, try_lock and unlock") {
  nx::spin_lock m;

  CHECK(m.try_lock());
  CHECK_FALSE(m.try_lock());
  m.unlock();
  CHECK(m.try_lock());
  m.unlock();

  m.lock();
  CHECK_FALSE(m.try_lock());
  m.unlock();

  for (int i = 0; i < 1000; ++i) {
    m.lock();
    m.unlock();
  }
  CHECK(m.try_lock());
  m.unlock();
}

TEST_CASE("spin_lock: provides mutual exclusion under contention") {
  nx::spin_lock m;
  i64 counter = 0;
  const int threads = worker_count();
  constexpr int per_thread = 20000;

  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
      for (int i = 0; i < per_thread; ++i) {
        nx::scoped_lock<nx::spin_lock> guard(m);
        ++counter;
      }
    });
  }
  for (std::thread &w : workers)
    w.join();

  CHECK(counter == static_cast<i64>(threads) * per_thread);
}

TEST_CASE("mutex: lock, try_lock and unlock") {
  nx::mutex m;

  CHECK(m.try_lock());
  CHECK_FALSE(m.try_lock());
  m.unlock();

  m.lock();
  CHECK_FALSE(m.try_lock());
  m.unlock();
  CHECK(m.try_lock());
  m.unlock();

  for (int i = 0; i < 1000; ++i) {
    m.lock();
    m.unlock();
  }
  CHECK(m.try_lock());
  m.unlock();
}

TEST_CASE("mutex: try_lock_for succeeds when free and times out when held") {
  nx::mutex m;

  CHECK(m.try_lock_for(10ms));
  m.unlock();

  CHECK(m.try_lock_for(0ms));
  m.unlock();

  m.lock();
  CHECK_FALSE(m.try_lock_for(0ms));

  const auto start = std::chrono::steady_clock::now();
  CHECK_FALSE(m.try_lock_for(50ms));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK(elapsed >= 20ms);
  m.unlock();

  // The holder takes and releases the lock itself: lock accounting is per
  // thread, so an unlock from another thread would leave this one counting a
  // lock it no longer holds.
  std::atomic<bool> held{false};
  std::thread releaser([&] {
    m.lock();
    held = true;
    std::this_thread::sleep_for(30ms);
    m.unlock();
  });
  while (!held)
    std::this_thread::yield();
  CHECK(m.try_lock_for(5s));
  m.unlock();
  releaser.join();
}

TEST_CASE("mutex: provides mutual exclusion under heavy contention") {
  nx::mutex m;
  i64 counter = 0;
  const int threads = worker_count();
  constexpr int per_thread = 20000;

  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
      for (int i = 0; i < per_thread; ++i) {
        nx::scoped_lock<nx::mutex> guard(m);
        ++counter;
      }
    });
  }
  for (std::thread &w : workers)
    w.join();

  CHECK(counter == static_cast<i64>(threads) * per_thread);
  CHECK(m.try_lock());
  m.unlock();
}

TEST_CASE("mutex: is only four bytes and neither copyable nor movable") {
  CHECK(sizeof(nx::mutex) == 4);
  static_assert(!std::is_copy_constructible_v<nx::mutex>);
  static_assert(!std::is_copy_assignable_v<nx::mutex>);
}

TEST_CASE("recursive_mutex: the owner may re-lock and must unlock as often") {
  nx::recursive_mutex m;

  m.lock();
  m.lock();
  m.lock();
  CHECK(m.try_lock());
  m.unlock();
  m.unlock();
  m.unlock();
  m.unlock();

  std::atomic<bool> taken{false};
  std::thread other([&] {
    if (m.try_lock()) {
      taken.store(true);
      m.unlock();
    }
  });
  other.join();
  CHECK(taken.load());
}

TEST_CASE("recursive_mutex: another thread cannot take a held lock") {
  nx::recursive_mutex m;
  m.lock();
  m.lock();

  std::atomic<bool> acquired{false};
  std::thread other([&] { acquired.store(m.try_lock()); });
  other.join();
  CHECK_FALSE(acquired.load());

  m.unlock();
  std::atomic<bool> acquired2{false};
  std::thread other2([&] { acquired2.store(m.try_lock()); });
  other2.join();
  CHECK_FALSE(acquired2.load());

  m.unlock();
  std::atomic<bool> acquired3{false};
  std::thread other3([&] {
    if (m.try_lock()) {
      acquired3.store(true);
      m.unlock();
    }
  });
  other3.join();
  CHECK(acquired3.load());
}

TEST_CASE("recursive_mutex: serialises recursive critical sections") {
  nx::recursive_mutex m;
  i64 counter = 0;
  const int threads = worker_count();
  constexpr int per_thread = 5000;

  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
      for (int i = 0; i < per_thread; ++i) {
        m.lock();
        m.lock();
        ++counter;
        m.unlock();
        m.unlock();
      }
    });
  }
  for (std::thread &w : workers)
    w.join();
  CHECK(counter == static_cast<i64>(threads) * per_thread);
}

TEST_CASE("cond_var: notify_one releases a waiter") {
  nx::mutex m;
  nx::cond_var cv;
  bool ready = false;
  std::atomic<bool> done{false};

  std::thread waiter([&] {
    nx::scoped_lock<nx::mutex> guard(m);
    cv.wait(m, [&] { return ready; });
    done.store(true, std::memory_order_release);
  });

  std::this_thread::sleep_for(20ms);
  CHECK_FALSE(done.load());
  {
    nx::scoped_lock<nx::mutex> guard(m);
    ready = true;
  }
  cv.notify_one();
  waiter.join();
  CHECK(done.load());
}

TEST_CASE("cond_var: notify_all releases every waiter") {
  nx::mutex m;
  nx::cond_var cv;
  bool ready = false;
  std::atomic<int> woken{0};
  const int threads = worker_count();

  std::vector<std::thread> waiters;
  for (int t = 0; t < threads; ++t) {
    waiters.emplace_back([&] {
      nx::scoped_lock<nx::mutex> guard(m);
      cv.wait(m, [&] { return ready; });
      woken.fetch_add(1, std::memory_order_relaxed);
    });
  }

  std::this_thread::sleep_for(30ms);
  CHECK(woken.load() == 0);
  {
    nx::scoped_lock<nx::mutex> guard(m);
    ready = true;
  }
  cv.notify_all();
  for (std::thread &w : waiters)
    w.join();
  CHECK(woken.load() == threads);
}

TEST_CASE("cond_var: wait_for times out and reports the predicate") {
  nx::mutex m;
  nx::cond_var cv;

  {
    nx::scoped_lock<nx::mutex> guard(m);

    CHECK_FALSE(cv.wait_for(m, 0ms));

    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(cv.wait_for(m, 50ms, [] { return false; }));
    CHECK(std::chrono::steady_clock::now() - start >= 20ms);

    const auto start2 = std::chrono::steady_clock::now();
    CHECK(cv.wait_for(m, 5s, [] { return true; }));
    CHECK(std::chrono::steady_clock::now() - start2 < 1s);
  }
  CHECK(m.try_lock());
  m.unlock();
}

TEST_CASE("cond_var: wait_for with a predicate wakes on notification") {
  nx::mutex m;
  nx::cond_var cv;
  bool ready = false;
  std::atomic<bool> result{false};

  std::thread waiter([&] {
    nx::scoped_lock<nx::mutex> guard(m);
    result.store(cv.wait_for(m, 5s, [&] { return ready; }));
  });

  std::this_thread::sleep_for(20ms);
  {
    nx::scoped_lock<nx::mutex> guard(m);
    ready = true;
  }
  cv.notify_all();
  waiter.join();
  CHECK(result.load());
}

TEST_CASE("cond_var: a producer/consumer handoff loses no items") {
  nx::mutex m;
  nx::cond_var cv;
  std::vector<int> queue;
  std::vector<int> consumed;
  bool finished = false;
  constexpr int count = 2000;

  std::thread consumer([&] {
    for (;;) {
      nx::scoped_lock<nx::mutex> guard(m);
      cv.wait(m, [&] { return !queue.empty() || finished; });
      while (!queue.empty()) {
        consumed.push_back(queue.back());
        queue.pop_back();
      }
      if (finished)
        return;
    }
  });

  for (int i = 0; i < count; ++i) {
    {
      nx::scoped_lock<nx::mutex> guard(m);
      queue.push_back(i);
    }
    cv.notify_one();
  }
  {
    nx::scoped_lock<nx::mutex> guard(m);
    finished = true;
  }
  cv.notify_all();
  consumer.join();

  CHECK(consumed.size() == count);
}

TEST_CASE("rw_lock: readers share, writers are exclusive") {
  nx::rw_lock rw;

  CHECK(rw.try_lock_shared());
  CHECK(rw.try_lock_shared());
  CHECK(rw.try_lock_shared());
  CHECK_FALSE(rw.try_lock());
  rw.unlock_shared();
  rw.unlock_shared();
  CHECK_FALSE(rw.try_lock());
  rw.unlock_shared();

  CHECK(rw.try_lock());
  CHECK_FALSE(rw.try_lock());
  CHECK_FALSE(rw.try_lock_shared());
  rw.unlock();

  CHECK(rw.try_lock_shared());
  rw.unlock_shared();
}

TEST_CASE("rw_lock: blocking lock and lock_shared round-trip") {
  nx::rw_lock rw;

  rw.lock();
  rw.unlock();
  rw.lock_shared();
  rw.lock_shared();
  rw.unlock_shared();
  rw.unlock_shared();

  for (int i = 0; i < 500; ++i) {
    rw.lock();
    rw.unlock();
    rw.lock_shared();
    rw.unlock_shared();
  }
  CHECK(rw.try_lock());
  rw.unlock();
}

TEST_CASE("rw_lock: the guards acquire and release the right mode") {
  nx::rw_lock rw;
  {
    nx::scoped_shared_lock<nx::rw_lock> r1(rw);
    nx::scoped_shared_lock<nx::rw_lock> r2(rw);
    CHECK_FALSE(rw.try_lock());
  }
  CHECK(rw.try_lock());
  rw.unlock();

  {
    nx::scoped_lock<nx::rw_lock> w(rw);
    CHECK_FALSE(rw.try_lock_shared());
  }
  CHECK(rw.try_lock_shared());
  rw.unlock_shared();
}

TEST_CASE("rw_lock: no writer ever overlaps a reader or another writer") {
  nx::rw_lock rw;
  std::atomic<int> readers{0};
  std::atomic<int> writers{0};
  std::atomic<bool> violation{false};
  std::atomic<i64> value{0};

  const int threads = worker_count();
  constexpr int iterations = 4000;

  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      for (int i = 0; i < iterations; ++i) {
        if ((i + t) % 4 == 0) {
          nx::scoped_lock<nx::rw_lock> guard(rw);
          const int w = writers.fetch_add(1, std::memory_order_acq_rel) + 1;
          if (w != 1 || readers.load(std::memory_order_acquire) != 0)
            violation.store(true, std::memory_order_relaxed);
          value.fetch_add(1, std::memory_order_relaxed);
          writers.fetch_sub(1, std::memory_order_acq_rel);
        } else {
          nx::scoped_shared_lock<nx::rw_lock> guard(rw);
          readers.fetch_add(1, std::memory_order_acq_rel);
          if (writers.load(std::memory_order_acquire) != 0)
            violation.store(true, std::memory_order_relaxed);
          readers.fetch_sub(1, std::memory_order_acq_rel);
        }
      }
    });
  }
  for (std::thread &w : workers)
    w.join();

  CHECK_FALSE(violation.load());
  CHECK(value.load() > 0);
  CHECK(rw.try_lock());
  rw.unlock();
}

TEST_CASE("semaphore: acquire consumes the count and release restores it") {
  nx::semaphore s(2);

  CHECK(s.try_acquire());
  CHECK(s.try_acquire());
  CHECK_FALSE(s.try_acquire());

  s.release();
  CHECK(s.try_acquire());
  s.release(2);
  CHECK(s.try_acquire());
  CHECK(s.try_acquire());
  CHECK_FALSE(s.try_acquire());

  nx::semaphore empty;
  CHECK_FALSE(empty.try_acquire());
  empty.release();
  CHECK(empty.try_acquire());
}

TEST_CASE(
    "semaphore: try_acquire_for times out then succeeds after a release") {
  nx::semaphore s(0);

  CHECK_FALSE(s.try_acquire_for(0ms));

  const auto start = std::chrono::steady_clock::now();
  CHECK_FALSE(s.try_acquire_for(50ms));
  CHECK(std::chrono::steady_clock::now() - start >= 20ms);

  std::thread releaser([&] {
    std::this_thread::sleep_for(30ms);
    s.release();
  });
  CHECK(s.try_acquire_for(5s));
  releaser.join();

  s.release();
  CHECK(s.try_acquire_for(5s));
}

TEST_CASE("semaphore: blocking acquire is woken by release") {
  nx::semaphore s(0);
  std::atomic<int> acquired{0};
  const int threads = worker_count();

  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
      s.acquire();
      acquired.fetch_add(1, std::memory_order_relaxed);
    });
  }

  std::this_thread::sleep_for(30ms);
  CHECK(acquired.load() == 0);

  s.release(static_cast<u32>(threads));
  for (std::thread &w : workers)
    w.join();
  CHECK(acquired.load() == threads);
  CHECK_FALSE(s.try_acquire());
}

TEST_CASE("semaphore: bounds concurrency to the permit count") {
  constexpr u32 permits = 3;
  nx::semaphore s(permits);
  std::atomic<int> inside{0};
  std::atomic<int> peak{0};
  const int threads = worker_count() * 2;

  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
      for (int i = 0; i < 500; ++i) {
        s.acquire();
        const int now = inside.fetch_add(1, std::memory_order_acq_rel) + 1;
        int prev = peak.load(std::memory_order_relaxed);
        while (now > prev && !peak.compare_exchange_weak(
                                 prev, now, std::memory_order_relaxed))
          ;
        inside.fetch_sub(1, std::memory_order_acq_rel);
        s.release();
      }
    });
  }
  for (std::thread &w : workers)
    w.join();

  CHECK(peak.load() >= 1);
  CHECK(peak.load() <= static_cast<int>(permits));
}

TEST_CASE("event: manual-reset releases everyone until reset") {
  nx::event e(true);
  CHECK_FALSE(e.is_set());

  e.set();
  CHECK(e.is_set());
  e.wait();
  e.wait();
  CHECK(e.is_set());

  e.reset();
  CHECK_FALSE(e.is_set());

  std::atomic<int> woken{0};
  const int threads = worker_count();
  std::vector<std::thread> waiters;
  for (int t = 0; t < threads; ++t) {
    waiters.emplace_back([&] {
      e.wait();
      woken.fetch_add(1, std::memory_order_relaxed);
    });
  }
  std::this_thread::sleep_for(30ms);
  CHECK(woken.load() == 0);
  e.set();
  for (std::thread &w : waiters)
    w.join();
  CHECK(woken.load() == threads);
}

TEST_CASE("event: auto-reset releases one waiter and clears itself") {
  nx::event e(false);
  CHECK_FALSE(e.is_set());

  e.set();
  CHECK(e.is_set());
  e.wait();
  CHECK_FALSE(e.is_set());

  std::atomic<int> woken{0};
  std::thread a([&] {
    e.wait();
    woken.fetch_add(1, std::memory_order_relaxed);
  });
  std::thread b([&] {
    e.wait();
    woken.fetch_add(1, std::memory_order_relaxed);
  });

  std::this_thread::sleep_for(30ms);
  CHECK(woken.load() == 0);

  e.set();
  CHECK(wait_until_true([&] { return woken.load() >= 1; }));
  std::this_thread::sleep_for(30ms);
  CHECK(woken.load() == 1);

  e.set();
  a.join();
  b.join();
  CHECK(woken.load() == 2);
}

TEST_CASE("event: initially_set starts signalled") {
  nx::event manual(true, true);
  CHECK(manual.is_set());
  manual.wait();

  nx::event automatic(false, true);
  CHECK(automatic.is_set());
  automatic.wait();
  CHECK_FALSE(automatic.is_set());
}

TEST_CASE("event: wait_for times out and consumes the flag when set") {
  nx::event manual(true);
  const auto start = std::chrono::steady_clock::now();
  CHECK_FALSE(manual.wait_for(50ms));
  CHECK(std::chrono::steady_clock::now() - start >= 20ms);
  manual.set();
  CHECK(manual.wait_for(5s));
  CHECK(manual.is_set());

  nx::event automatic(false);
  CHECK_FALSE(automatic.wait_for(50ms));
  automatic.set();
  CHECK(automatic.wait_for(5s));
  CHECK_FALSE(automatic.is_set());
  CHECK_FALSE(automatic.wait_for(20ms));

  nx::event late(true);
  std::thread setter([&] {
    std::this_thread::sleep_for(30ms);
    late.set();
  });
  CHECK(late.wait_for(5s));
  setter.join();
}

TEST_CASE("barrier: releases only once every participant has arrived") {
  const int threads = worker_count();
  nx::barrier b(static_cast<u32>(threads));
  std::atomic<int> before{0};
  std::atomic<int> after{0};

  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      std::this_thread::sleep_for(std::chrono::milliseconds(t * 5));
      before.fetch_add(1, std::memory_order_acq_rel);
      b.arrive_and_wait();
      if (before.load(std::memory_order_acquire) == threads)
        after.fetch_add(1, std::memory_order_acq_rel);
    });
  }
  for (std::thread &w : workers)
    w.join();

  CHECK(before.load() == threads);
  CHECK(after.load() == threads);
}

TEST_CASE("barrier: is reusable across phases") {
  const int threads = worker_count();
  constexpr int phases = 20;
  nx::barrier b(static_cast<u32>(threads));
  std::atomic<int> phase_counter{0};
  std::atomic<bool> violation{false};

  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
      for (int p = 0; p < phases; ++p) {
        phase_counter.fetch_add(1, std::memory_order_acq_rel);
        b.arrive_and_wait();
        if (phase_counter.load(std::memory_order_acquire) < (p + 1) * threads)
          violation.store(true, std::memory_order_relaxed);
        b.arrive_and_wait();
      }
    });
  }
  for (std::thread &w : workers)
    w.join();

  CHECK_FALSE(violation.load());
  CHECK(phase_counter.load() == threads * phases);
}

TEST_CASE("barrier: a count of one never blocks") {
  nx::barrier b(1);
  for (int i = 0; i < 100; ++i)
    b.arrive_and_wait();
  CHECK(true);
}

TEST_CASE("call_once: runs the callable exactly once") {
  nx::once_flag flag;
  int calls = 0;

  nx::call_once(flag, [&] { ++calls; });
  nx::call_once(flag, [&] { ++calls; });
  nx::call_once(flag, [&] { ++calls; });
  CHECK(calls == 1);

  nx::once_flag flag2;
  int seen = 0;
  nx::call_once(flag2, [&](int a, int b) { seen = a + b; }, 2, 3);
  nx::call_once(flag2, [&](int a, int b) { seen = a + b; }, 100, 100);
  CHECK(seen == 5);
}

TEST_CASE("call_once: exactly one of many racing threads wins") {
  nx::once_flag flag;
  std::atomic<int> calls{0};
  std::atomic<int> observed_after{0};
  const int threads = worker_count() * 2;

  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
      nx::call_once(flag, [&] {
        std::this_thread::sleep_for(10ms);
        calls.fetch_add(1, std::memory_order_release);
      });
      observed_after.fetch_add(calls.load(std::memory_order_acquire),
                               std::memory_order_relaxed);
    });
  }
  for (std::thread &w : workers)
    w.join();

  CHECK(calls.load() == 1);
  CHECK(observed_after.load() == threads);
}

TEST_CASE("scoped_lock releases on the exception path") {
  nx::mutex m;
  try {
    nx::scoped_lock<nx::mutex> guard(m);
    throw 42;
  } catch (int e) {
    CHECK(e == 42);
  }
  CHECK(m.try_lock());
  m.unlock();

  nx::rw_lock rw;
  try {
    nx::scoped_shared_lock<nx::rw_lock> guard(rw);
    throw 7;
  } catch (int e) {
    CHECK(e == 7);
  }
  CHECK(rw.try_lock());
  rw.unlock();
}

TEST_CASE("sync: the primitives are neither copyable nor assignable") {
  static_assert(!std::is_copy_constructible_v<nx::spin_lock>);
  static_assert(!std::is_copy_constructible_v<nx::mutex>);
  static_assert(!std::is_copy_constructible_v<nx::recursive_mutex>);
  static_assert(!std::is_copy_constructible_v<nx::cond_var>);
  static_assert(!std::is_copy_constructible_v<nx::rw_lock>);
  static_assert(!std::is_copy_constructible_v<nx::semaphore>);
  static_assert(!std::is_copy_constructible_v<nx::event>);
  static_assert(!std::is_copy_constructible_v<nx::barrier>);
  static_assert(!std::is_copy_constructible_v<nx::once_flag>);
  CHECK(true);
}
