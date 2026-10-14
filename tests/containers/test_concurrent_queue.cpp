#include "framework/nxtest.h"

#include "core/foundation/containers/concurrent_queue.h"

#include <algorithm>
#include <atomic>
#include <numeric>
#include <set>
#include <thread>
#include <vector>

namespace {

std::atomic<int> g_live{0};

struct Payload {
  int value = 0;

  Payload() noexcept { g_live.fetch_add(1, std::memory_order_relaxed); }
  explicit Payload(int v) noexcept : value(v) {
    g_live.fetch_add(1, std::memory_order_relaxed);
  }
  Payload(const Payload &o) noexcept : value(o.value) {
    g_live.fetch_add(1, std::memory_order_relaxed);
  }
  Payload(Payload &&o) noexcept : value(o.value) {
    o.value = -1;
    g_live.fetch_add(1, std::memory_order_relaxed);
  }
  Payload &operator=(const Payload &) noexcept = default;
  Payload &operator=(Payload &&o) noexcept {
    value = o.value;
    o.value = -1;
    return *this;
  }
  ~Payload() { g_live.fetch_sub(1, std::memory_order_relaxed); }
};

}

TEST_CASE("concurrent_queue: capacity is rounded up to a power of two") {
  CHECK((nx::mpmc::Queue<int, 4>::CAPACITY) == 4);
  CHECK((nx::mpmc::Queue<int, 5>::CAPACITY) == 8);
  CHECK((nx::mpmc::Queue<int, 100>::CAPACITY) == 128);
  CHECK((nx::mpmc::Queue<int, 1024>::CAPACITY) == 1024);

  CHECK((nx::mpmc::Queue<int, 8>::MASK) == 7);
  CHECK((nx::mpsc::Queue<int, 5>::CAPACITY) == 8);
  CHECK((nx::mpsc::Queue<int, 8>::MASK) == 7);

  static_assert(nx::is_pow2(nx::mpmc::Queue<int, 3>::CAPACITY));
  static_assert(nx::is_pow2(nx::mpsc::Queue<int, 7>::CAPACITY));
}

TEST_CASE("concurrent_queue: queues are neither copyable nor movable") {
  using Q = nx::mpmc::Queue<int, 4>;
  static_assert(!std::is_copy_constructible_v<Q>);
  static_assert(!std::is_move_constructible_v<Q>);
  static_assert(!std::is_copy_assignable_v<Q>);
  static_assert(!std::is_move_assignable_v<Q>);

  using P = nx::mpsc::Queue<int, 4>;
  static_assert(!std::is_copy_constructible_v<P>);
  static_assert(!std::is_move_constructible_v<P>);
  CHECK(true);
}

TEST_CASE("mpmc: a fresh queue is empty and pops fail") {
  nx::mpmc::Queue<int, 8> q;
  CHECK(q.empty_approx());
  CHECK_FALSE(q.full_approx());
  CHECK(q.size_approx() == 0);

  int out = -1;
  CHECK_FALSE(q.try_pop(out));
  CHECK(out == -1);
}

TEST_CASE("mpmc: try_push and try_pop preserve FIFO order") {
  nx::mpmc::Queue<int, 8> q;

  for (int i = 0; i < 8; ++i)
    CHECK(q.try_push(i));
  CHECK(q.size_approx() == 8);
  CHECK(q.full_approx());

  for (int i = 0; i < 8; ++i) {
    int out = -1;
    REQUIRE(q.try_pop(out));
    CHECK(out == i);
  }
  CHECK(q.empty_approx());
}

TEST_CASE("mpmc: try_push reports failure when the queue is full") {
  nx::mpmc::Queue<int, 4> q;

  for (int i = 0; i < 4; ++i)
    REQUIRE(q.try_push(i));

  CHECK(q.full_approx());
  CHECK_FALSE(q.try_push(99));
  CHECK_FALSE(q.try_push(99));
  CHECK(q.size_approx() == 4);

  int out = -1;
  for (int i = 0; i < 4; ++i) {
    REQUIRE(q.try_pop(out));
    CHECK(out == i);
  }
  CHECK_FALSE(q.try_pop(out));
}

TEST_CASE("mpmc: a requested capacity of one is raised to two usable slots") {
  static_assert(nx::mpmc::Queue<int, 1>::CAPACITY == 2);
  nx::mpmc::Queue<int, 1> q;

  REQUIRE(q.try_push(1));
  CHECK(q.size_approx() == 1);
  CHECK_FALSE(q.full_approx());

  REQUIRE(q.try_push(2));
  CHECK(q.full_approx());

  CHECK_FALSE(q.try_push(3));
  CHECK(q.size_approx() == 2);

  int out = -1;
  REQUIRE(q.try_pop(out));
  CHECK(out == 1);
  REQUIRE(q.try_pop(out));
  CHECK(out == 2);
  CHECK(q.empty_approx());
  CHECK_FALSE(q.try_pop(out));

  for (int i = 0; i < 100; ++i) {
    REQUIRE(q.try_push(i));
    REQUIRE(q.try_push(i + 1000));
    CHECK_FALSE(q.try_push(i + 2000));
    int a = -1, b = -1;
    REQUIRE(q.try_pop(a));
    REQUIRE(q.try_pop(b));
    CHECK(a == i);
    CHECK(b == i + 1000);
  }
}

TEST_CASE("mpmc: the ring wraps around many times") {
  nx::mpmc::Queue<int, 4> q;

  for (int round = 0; round < 1000; ++round) {
    REQUIRE(q.try_push(round));
    int out = -1;
    REQUIRE(q.try_pop(out));
    CHECK(out == round);
  }
  CHECK(q.empty_approx());

  for (int round = 0; round < 500; ++round) {
    REQUIRE(q.try_push(round * 2));
    REQUIRE(q.try_push(round * 2 + 1));
    int a = -1, b = -1;
    REQUIRE(q.try_pop(a));
    REQUIRE(q.try_pop(b));
    CHECK(a == round * 2);
    CHECK(b == round * 2 + 1);
  }
}

TEST_CASE("mpmc: try_emplace constructs in place") {
  nx::mpmc::Queue<Payload, 8> q;
  g_live = 0;

  CHECK(q.try_emplace(42));
  CHECK(g_live.load() == 1);

  Payload out;
  CHECK(g_live.load() == 2);
  REQUIRE(q.try_pop(out));
  CHECK(out.value == 42);
  CHECK(g_live.load() == 1);
}

TEST_CASE("mpmc: push copies an lvalue and moves an rvalue") {
  nx::mpmc::Queue<Payload, 8> q;
  g_live = 0;
  {
    Payload src(7);
    CHECK(q.try_push(src));
    CHECK(src.value == 7);

    Payload moved(9);
    CHECK(q.try_push(std::move(moved)));
    CHECK(moved.value == -1);

    Payload out;
    REQUIRE(q.try_pop(out));
    CHECK(out.value == 7);
    REQUIRE(q.try_pop(out));
    CHECK(out.value == 9);
  }
  CHECK(g_live.load() == 0);
}

TEST_CASE("mpmc: the destructor releases elements still in the queue") {
  g_live = 0;
  {
    nx::mpmc::Queue<Payload, 8> q;
    for (int i = 0; i < 5; ++i)
      REQUIRE(q.try_emplace(i));
    CHECK(g_live.load() == 5);
  }
  CHECK(g_live.load() == 0);

  g_live = 0;
  {
    nx::mpmc::Queue<Payload, 4> q;
    for (int round = 0; round < 10; ++round) {
      REQUIRE(q.try_emplace(round));
      Payload out;
      REQUIRE(q.try_pop(out));
    }
    REQUIRE(q.try_emplace(1));
    REQUIRE(q.try_emplace(2));
    CHECK(g_live.load() == 2);
  }
  CHECK(g_live.load() == 0);
}

TEST_CASE("mpmc: blocking push and pop round-trip on one thread") {
  nx::mpmc::Queue<int, 8> q;

  for (int i = 0; i < 8; ++i)
    q.push(i);
  CHECK(q.size_approx() == 8);

  for (int i = 0; i < 8; ++i) {
    int out = -1;
    q.pop(out);
    CHECK(out == i);
  }
  CHECK(q.size_approx() == 0);

  q.emplace(99);
  int out = -1;
  q.pop(out);
  CHECK(out == 99);

  int lvalue = 5;
  q.push(lvalue);
  q.pop(out);
  CHECK(out == 5);
}

TEST_CASE("mpmc: size_approx tracks pushes and pops") {
  nx::mpmc::Queue<int, 16> q;
  CHECK(q.size_approx() == 0);
  CHECK(q.empty_approx());

  for (int i = 0; i < 10; ++i) {
    REQUIRE(q.try_push(i));
    CHECK(q.size_approx() == i + 1);
    CHECK_FALSE(q.empty_approx());
  }
  CHECK_FALSE(q.full_approx());

  for (int i = 0; i < 10; ++i) {
    int out = 0;
    REQUIRE(q.try_pop(out));
    CHECK(q.size_approx() == 9 - i);
  }
  CHECK(q.empty_approx());
}

namespace {

// Holds its push open: the slot is claimed until the constructor returns.
struct HeldOpen {
  int value = 0;

  HeldOpen() noexcept = default;
  HeldOpen(std::atomic<bool> &entered,
           const std::atomic<bool> &release) noexcept
      : value(1) {
    entered.store(true);
    while (!release.load())
      std::this_thread::yield();
  }
  HeldOpen(HeldOpen &&) noexcept = default;
  HeldOpen &operator=(HeldOpen &&) noexcept = default;
};

} // namespace

TEST_CASE("mpmc: an element still being pushed is counted but cannot be "
          "popped") {
  nx::mpmc::Queue<HeldOpen, 16> q;
  CHECK_FALSE(q.can_pop_approx());

  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  std::atomic<bool> pushed{false};
  std::thread pusher([&] { pushed.store(q.try_emplace(entered, release)); });
  while (!entered.load())
    std::this_thread::yield();

  CHECK(q.size_approx() == 1);
  CHECK_FALSE(q.empty_approx());
  CHECK_FALSE(q.can_pop_approx());
  HeldOpen out;
  CHECK_FALSE(q.try_pop(out));

  release.store(true);
  pusher.join();
  REQUIRE(pushed.load());
  CHECK(q.can_pop_approx());
  REQUIRE(q.try_pop(out));
  CHECK(out.value == 1);
  CHECK_FALSE(q.can_pop_approx());
}

TEST_CASE("mpsc: a fresh queue is empty and pops fail") {
  nx::mpsc::Queue<int, 8> q;
  CHECK(q.empty_approx());
  CHECK(q.size_approx() == 0);

  int out = -1;
  CHECK_FALSE(q.try_pop(out));
  CHECK(out == -1);
}

TEST_CASE("mpsc: try_push and try_pop preserve FIFO order") {
  nx::mpsc::Queue<int, 8> q;

  for (int i = 0; i < 8; ++i)
    CHECK(q.try_push(i));
  CHECK(q.full_approx());
  CHECK_FALSE(q.try_push(99));

  int out = -1;
  for (int i = 0; i < 8; ++i) {
    REQUIRE(q.try_pop(out));
    CHECK(out == i);
  }
  CHECK(q.empty_approx());
  CHECK_FALSE(q.try_pop(out));
}

TEST_CASE("mpsc: a requested capacity of one is raised to two usable slots") {
  static_assert(nx::mpsc::Queue<int, 1>::CAPACITY == 2);
  nx::mpsc::Queue<int, 1> q;

  REQUIRE(q.try_push(1));
  CHECK_FALSE(q.full_approx());
  REQUIRE(q.try_push(2));
  CHECK(q.full_approx());
  CHECK_FALSE(q.try_push(3));
  CHECK(q.size_approx() == 2);

  int out = -1;
  REQUIRE(q.try_pop(out));
  CHECK(out == 1);
  REQUIRE(q.try_pop(out));
  CHECK(out == 2);
  CHECK_FALSE(q.try_pop(out));

  for (int i = 0; i < 100; ++i) {
    REQUIRE(q.try_push(i));
    REQUIRE(q.try_push(i + 1000));
    CHECK_FALSE(q.try_push(i + 2000));
    int a = -1, b = -1;
    REQUIRE(q.try_pop(a));
    REQUIRE(q.try_pop(b));
    CHECK(a == i);
    CHECK(b == i + 1000);
  }
}

TEST_CASE("mpsc: the ring wraps around many times") {
  nx::mpsc::Queue<int, 4> q;
  for (int round = 0; round < 1000; ++round) {
    REQUIRE(q.try_push(round));
    int out = -1;
    REQUIRE(q.try_pop(out));
    CHECK(out == round);
  }
  CHECK(q.empty_approx());
}

TEST_CASE("mpsc: blocking push and pop round-trip on one thread") {
  nx::mpsc::Queue<int, 8> q;
  for (int i = 0; i < 8; ++i)
    q.push(i);
  for (int i = 0; i < 8; ++i) {
    int out = -1;
    q.pop(out);
    CHECK(out == i);
  }

  q.emplace(11);
  int out = -1;
  q.pop(out);
  CHECK(out == 11);

  int lvalue = 3;
  q.push(lvalue);
  q.pop(out);
  CHECK(out == 3);
}

TEST_CASE("mpsc: consume_all drains everything and returns the count") {
  nx::mpsc::Queue<int, 16> q;
  for (int i = 0; i < 10; ++i)
    REQUIRE(q.try_push(i));

  std::vector<int> seen;
  const usize n = q.consume_all([&seen](int v) { seen.push_back(v); });
  CHECK(n == 10);
  CHECK(seen == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
  CHECK(q.empty_approx());

  int calls = 0;
  CHECK(q.consume_all([&calls](int) { ++calls; }) == 0);
  CHECK(calls == 0);
}

TEST_CASE("mpsc: consume_batch stops at the requested limit") {
  nx::mpsc::Queue<int, 16> q;
  for (int i = 0; i < 10; ++i)
    REQUIRE(q.try_push(i));

  std::vector<int> seen;
  CHECK(q.consume_batch([&seen](int v) { seen.push_back(v); }, 4) == 4);
  CHECK(seen == std::vector<int>{0, 1, 2, 3});
  CHECK(q.size_approx() == 6);

  seen.clear();
  CHECK(q.consume_batch([&seen](int v) { seen.push_back(v); }, 100) == 6);
  CHECK(seen == std::vector<int>{4, 5, 6, 7, 8, 9});
  CHECK(q.empty_approx());

  REQUIRE(q.try_push(1));
  CHECK(q.consume_batch([](int) {}, 0) == 0);
  CHECK(q.size_approx() == 1);
}

TEST_CASE("mpsc: the destructor releases elements still in the queue") {
  g_live = 0;
  {
    nx::mpsc::Queue<Payload, 8> q;
    for (int i = 0; i < 5; ++i)
      REQUIRE(q.try_emplace(i));
    CHECK(g_live.load() == 5);
  }
  CHECK(g_live.load() == 0);
}

TEST_CASE(
    "mpmc: every element survives multi-producer multi-consumer traffic") {
  constexpr int producers = 4;
  constexpr int consumers = 4;
  constexpr int per_producer = 4000;
  constexpr int total = producers * per_producer;

  nx::mpmc::Queue<int, 1024> q;
  std::atomic<int> produced{0};
  std::vector<std::vector<int>> consumed(consumers);
  std::atomic<int> consumed_count{0};

  std::vector<std::thread> threads;
  for (int p = 0; p < producers; ++p) {
    threads.emplace_back([&q, &produced, p] {
      for (int i = 0; i < per_producer; ++i) {
        const int value = p * per_producer + i;
        while (!q.try_push(value))
          std::this_thread::yield();
        produced.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (int c = 0; c < consumers; ++c) {
    threads.emplace_back([&q, &consumed, &consumed_count, c] {
      int value = 0;
      while (consumed_count.load(std::memory_order_relaxed) < total) {
        if (q.try_pop(value)) {
          consumed[static_cast<usize>(c)].push_back(value);
          consumed_count.fetch_add(1, std::memory_order_relaxed);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }
  for (std::thread &t : threads)
    t.join();

  CHECK(produced.load() == total);

  std::vector<int> all;
  for (const auto &c : consumed)
    all.insert(all.end(), c.begin(), c.end());
  REQUIRE(all.size() == static_cast<usize>(total));

  std::sort(all.begin(), all.end());
  for (int i = 0; i < total; ++i)
    REQUIRE(all[static_cast<usize>(i)] == i);

  CHECK(q.empty_approx());
}

TEST_CASE("mpmc: FIFO order is preserved per producer") {
  constexpr int producers = 3;
  constexpr int per_producer = 3000;

  nx::mpmc::Queue<u64, 256> q;
  std::vector<std::thread> threads;
  for (int p = 0; p < producers; ++p) {
    threads.emplace_back([&q, p] {
      for (int i = 0; i < per_producer; ++i) {
        const u64 value = (static_cast<u64>(p) << 32) | static_cast<u32>(i);
        while (!q.try_push(value))
          std::this_thread::yield();
      }
    });
  }

  std::vector<int> last(producers, -1);
  int received = 0;
  while (received < producers * per_producer) {
    u64 value = 0;
    if (q.try_pop(value)) {
      const int p = static_cast<int>(value >> 32);
      const int seq = static_cast<int>(value & 0xffffffffu);
      REQUIRE(seq == last[static_cast<usize>(p)] + 1);
      last[static_cast<usize>(p)] = seq;
      ++received;
    } else {
      std::this_thread::yield();
    }
  }
  for (std::thread &t : threads)
    t.join();

  for (const int l : last)
    CHECK(l == per_producer - 1);
}

TEST_CASE("mpmc: blocking push and pop deliver every element") {
  constexpr int producers = 3;
  constexpr int per_producer = 2000;
  constexpr int total = producers * per_producer;

  nx::mpmc::Queue<int, 256> q;
  std::vector<std::thread> threads;
  for (int p = 0; p < producers; ++p) {
    threads.emplace_back([&q, p] {
      for (int i = 0; i < per_producer; ++i)
        q.push(p * per_producer + i);
    });
  }

  std::vector<int> got;
  got.reserve(total);
  std::thread consumer([&q, &got] {
    for (int i = 0; i < total; ++i) {
      int value = 0;
      q.pop(value);
      got.push_back(value);
    }
  });

  for (std::thread &t : threads)
    t.join();
  consumer.join();

  REQUIRE(got.size() == static_cast<usize>(total));
  std::sort(got.begin(), got.end());
  for (int i = 0; i < total; ++i)
    REQUIRE(got[static_cast<usize>(i)] == i);
}

TEST_CASE("mpsc: many producers and one consumer lose nothing") {
  constexpr int producers = 4;
  constexpr int per_producer = 4000;
  constexpr int total = producers * per_producer;

  nx::mpsc::Queue<int, 1024> q;
  std::vector<std::thread> threads;
  for (int p = 0; p < producers; ++p) {
    threads.emplace_back([&q, p] {
      for (int i = 0; i < per_producer; ++i) {
        while (!q.try_push(p * per_producer + i))
          std::this_thread::yield();
      }
    });
  }

  std::vector<int> got;
  got.reserve(total);
  while (got.size() < static_cast<usize>(total)) {
    const usize n = q.consume_batch([&got](int v) { got.push_back(v); }, 64);
    if (n == 0)
      std::this_thread::yield();
  }
  for (std::thread &t : threads)
    t.join();

  REQUIRE(got.size() == static_cast<usize>(total));
  std::sort(got.begin(), got.end());
  for (int i = 0; i < total; ++i)
    REQUIRE(got[static_cast<usize>(i)] == i);
  CHECK(q.empty_approx());
}

TEST_CASE("mpsc: FIFO order is preserved per producer") {
  constexpr int producers = 3;
  constexpr int per_producer = 3000;

  nx::mpsc::Queue<u64, 256> q;
  std::vector<std::thread> threads;
  for (int p = 0; p < producers; ++p) {
    threads.emplace_back([&q, p] {
      for (int i = 0; i < per_producer; ++i) {
        const u64 value = (static_cast<u64>(p) << 32) | static_cast<u32>(i);
        while (!q.try_push(value))
          std::this_thread::yield();
      }
    });
  }

  std::vector<int> last(producers, -1);
  int received = 0;
  while (received < producers * per_producer) {
    u64 value = 0;
    if (q.try_pop(value)) {
      const int p = static_cast<int>(value >> 32);
      const int seq = static_cast<int>(value & 0xffffffffu);
      REQUIRE(seq == last[static_cast<usize>(p)] + 1);
      last[static_cast<usize>(p)] = seq;
      ++received;
    } else {
      std::this_thread::yield();
    }
  }
  for (std::thread &t : threads)
    t.join();

  for (const int l : last)
    CHECK(l == per_producer - 1);
}

TEST_CASE(
    "mpmc: non-trivial payloads are neither leaked nor duplicated under load") {
  constexpr int producers = 3;
  constexpr int per_producer = 2000;
  constexpr int total = producers * per_producer;

  g_live = 0;
  {
    nx::mpmc::Queue<Payload, 512> q;
    std::vector<std::thread> threads;
    for (int p = 0; p < producers; ++p) {
      threads.emplace_back([&q, p] {
        for (int i = 0; i < per_producer; ++i) {
          while (!q.try_emplace(p * per_producer + i))
            std::this_thread::yield();
        }
      });
    }

    std::vector<int> got;
    got.reserve(total);
    while (got.size() < static_cast<usize>(total)) {
      Payload out;
      if (q.try_pop(out))
        got.push_back(out.value);
      else
        std::this_thread::yield();
    }
    for (std::thread &t : threads)
      t.join();

    REQUIRE(got.size() == static_cast<usize>(total));
    std::sort(got.begin(), got.end());
    for (int i = 0; i < total; ++i)
      REQUIRE(got[static_cast<usize>(i)] == i);
    CHECK(q.empty_approx());
  }
  CHECK(g_live.load() == 0);
}
