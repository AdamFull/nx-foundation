#include "framework/nxtest.h"

#include "core/foundation/threading/completion_queue.h"
#include "core/foundation/threading/thread.h"
#include "core/foundation/threading/thread_pool.h"

#include <algorithm>
#include <chrono>

namespace {

[[nodiscard]] nx::vector<u32> drain(nx::completion_queue<u32> &queue,
                                    const usize budget = ~usize{0}) {
  nx::vector<u32> out;
  (void)queue.publish(budget, [&](u32 v) { out.push_back(v); });
  return out;
}

} // namespace

TEST_CASE("completion queue: results publish in the order reserved, "
          "whatever order they finish in") {
  nx::completion_queue<u32> queue(8);
  const auto first = queue.reserve();
  const auto second = queue.reserve();
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  queue.complete(*second, 2);
  CHECK(drain(queue).empty());
  queue.complete(*first, 1);
  CHECK(drain(queue) == nx::vector<u32>{1, 2});
  CHECK(queue.pending() == 0u);

  // Finished by workers in whatever order they run.
  nx::thread_pool pool;
  nx::completion_queue<u32> many(512);
  nx::vector<nx::completion_ticket> tickets;
  for (u32 i = 0; i < 500; ++i)
    tickets.push_back(*many.reserve());
  nx::task_group group;
  for (u32 i = 0; i < 500; ++i)
    pool.submit(group, [&, i] {
      if (i % 11 == 0)
        many.abandon(tickets[i]);
      else
        many.complete(tickets[i], i);
    });
  // Published while the workers still finish, then the rest, a budget a
  // call taking them over several.
  nx::vector<u32> published;
  const auto take = [&] {
    CHECK(many.publish(7, [&](u32 v) { published.push_back(v); }) <= 7u);
  };
  for (u32 spin = 0; spin < 2000 && many.pending() > 250; ++spin) {
    take();
    nx::this_thread::yield();
  }
  group.wait();
  for (u32 call = 0; call < 100 && many.pending() != 0; ++call)
    take();
  nx::vector<u32> expected;
  for (u32 i = 0; i < 500; ++i)
    if (i % 11 != 0)
      expected.push_back(i);
  CHECK(published == expected);
  CHECK(many.pending() == 0u);
}

TEST_CASE("completion queue: a publish takes at most its budget and skips "
          "abandoned work") {
  nx::completion_queue<u32> queue(8);
  nx::vector<nx::completion_ticket> t;
  for (u32 i = 0; i < 5; ++i)
    t.push_back(*queue.reserve());
  queue.complete(t[0], 10);
  queue.abandon(t[1]);
  queue.complete(t[2], 12);
  queue.complete(t[3], 13);
  CHECK(drain(queue, 2) == nx::vector<u32>{10, 12});
  CHECK(drain(queue, 0).empty());
  CHECK(drain(queue) == nx::vector<u32>{13});
  CHECK(queue.pending() == 1u);
  queue.abandon(t[4]);
  CHECK(drain(queue, 0).empty());
  CHECK(queue.pending() == 0u);

  // Past the first, a publish stops once its time is up.
  for (u32 i = 0; i < 3; ++i)
    queue.complete(*queue.reserve(), i);
  usize calls = 0;
  CHECK(queue.publish(
            10,
            [&](u32) {
              ++calls;
              nx::this_thread::sleep_for(std::chrono::milliseconds(2));
            },
            nx::duration::from_nanos(1)) == 1u);
  CHECK(calls == 1u);
  CHECK(queue.pending() == 2u);
}

TEST_CASE("completion queue: no more than its capacity is reserved at once, "
          "and publishing frees places") {
  nx::completion_queue<u32> queue(3);
  CHECK(queue.capacity() == 3u);
  nx::vector<nx::completion_ticket> t;
  for (u32 i = 0; i < 3; ++i)
    t.push_back(*queue.reserve());
  CHECK_FALSE(queue.reserve().has_value());
  queue.complete(t[1], 1);
  CHECK_FALSE(queue.reserve().has_value());
  queue.complete(t[0], 0);
  CHECK(drain(queue) == nx::vector<u32>{0, 1});
  const auto again = queue.reserve();
  REQUIRE(again.has_value());

  // Round the ring many times.
  queue.complete(t[2], 2);
  queue.complete(*again, 3);
  CHECK(drain(queue) == nx::vector<u32>{2, 3});
  u32 next = 4;
  for (u32 round = 0; round < 1000; ++round) {
    const auto a = queue.reserve();
    const auto b = queue.reserve();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    queue.complete(*b, next + 1);
    queue.complete(*a, next);
    CHECK(drain(queue) == nx::vector<u32>{next, next + 1});
    next += 2;
  }
}
