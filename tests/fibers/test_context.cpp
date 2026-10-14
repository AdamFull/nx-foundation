#include "fiber_test_support.h"

#include "core/foundation/threading/thread.h"

#include <cfenv>
#include <stdexcept>

#if NX_HAS_WINDOWS_API
#include <windows.h>
#endif

using nx::fiber::Context;
using nx::fiber::StackPool;
using nx::fiber::Transfer;

namespace {

struct PingPong {
  Context *fiber = nullptr;
};

NX_NO_INLINE bool stack_is_aligned() {
  alignas(16) volatile u8 probe[16] = {};
  probe[0] = 1;
  return (reinterpret_cast<uintptr_t>(&probe[0]) & 15) == 0;
}

NX_NO_INLINE void throw_from_depth(i32 depth) {
  volatile i32 guard = depth;
  if (guard == 0)
    throw std::runtime_error("from a fiber");
  throw_from_depth(depth - 1);
}

} // namespace

TEST_CASE("fiber context: a new fiber gets the data and the context that "
          "started it") {
  StackPool pool;
  auto stack = pool.acquire();
  REQUIRE(stack.has_value());

  struct Seen {
    Context *from = nullptr;
    void *data = nullptr;
    Context *self = nullptr;
  } seen;

  Context caller;
  Context fiber(*stack, [](Transfer t) {
    auto *s = static_cast<Seen *>(t.data);
    s->from = t.from;
    s->data = t.data;
    nx::fiber::exit_to(*s->self, *t.from, s);
  });
  seen.self = &fiber;

  CHECK(fiber.suspended());
  const Transfer back = nx::fiber::switch_to(caller, fiber, &seen);
  CHECK(seen.from == &caller);
  CHECK(seen.data == &seen);
  CHECK(back.from == &fiber);
  CHECK(back.data == &seen);
  CHECK_FALSE(caller.suspended());
  pool.release(*stack);
}

TEST_CASE("fiber context: ping-pong passes data both ways and keeps each "
          "side's locals") {
  StackPool pool;
  auto stack = pool.acquire();
  REQUIRE(stack.has_value());

  Context caller;
  Context fiber(*stack, [](Transfer t) {
    auto *state = static_cast<PingPong *>(t.data);
    u64 local_sum = 0;
    for (u32 i = 1;; ++i) {
      local_sum += i;
      t = nx::fiber::switch_to(*state->fiber, *t.from,
                               reinterpret_cast<void *>(uintptr_t(local_sum)));
    }
  });

  PingPong state{&fiber};
  u64 expected = 0;
  bool matched = true;
  for (u32 i = 1; i <= 10000; ++i) {
    const Transfer t =
        nx::fiber::switch_to(caller, fiber, i == 1 ? &state : nullptr);
    expected += i;
    matched = matched && reinterpret_cast<uintptr_t>(t.data) == expected;
  }
  CHECK(matched);
  CHECK(fiber.suspended());
  pool.release(*stack);
}

TEST_CASE("fiber context: code on a new fiber sees an ABI-aligned stack") {
  StackPool pool;
  bool aligned = false;
  fiber_test::run_on_fiber(pool, [&] { aligned = stack_is_aligned(); });
  CHECK(aligned);
}

TEST_CASE("fiber context: each fiber keeps its own floating-point rounding "
          "mode") {
  StackPool pool;
  auto stack = pool.acquire();
  REQUIRE(stack.has_value());

  struct Modes {
    Context *self = nullptr;
    int first = -1;
    int after_resume = -1;
  } modes;

  Context caller;
  Context fiber(*stack, [](Transfer t) {
    auto *m = static_cast<Modes *>(t.data);
    m->first = std::fegetround();
    std::fesetround(FE_UPWARD);
    t = nx::fiber::switch_to(*m->self, *t.from);
    m->after_resume = std::fegetround();
    nx::fiber::exit_to(*m->self, *t.from);
  });
  modes.self = &fiber;

  REQUIRE(std::fesetround(FE_TOWARDZERO) == 0);
  nx::fiber::switch_to(caller, fiber, &modes);
  const int caller_mode = std::fegetround();
  nx::fiber::switch_to(caller, fiber);
  std::fesetround(FE_TONEAREST);

  CHECK(modes.first == FE_TONEAREST);
  CHECK(caller_mode == FE_TOWARDZERO);
  CHECK(modes.after_resume == FE_UPWARD);
  pool.release(*stack);
}

TEST_CASE("fiber context: a suspended fiber resumes on another OS thread") {
  StackPool pool;
  auto stack = pool.acquire();
  REQUIRE(stack.has_value());

  struct Threads {
    Context *self = nullptr;
    u64 ids[2] = {};
    u32 runs = 0;
  } seen;

  Context caller;
  Context fiber(*stack, [](Transfer t) {
    auto *s = static_cast<Threads *>(t.data);
    for (;;) {
      s->ids[s->runs++] = nx::this_thread::id();
      t = nx::fiber::switch_to(*s->self, *t.from, s);
    }
  });
  seen.self = &fiber;

  nx::fiber::switch_to(caller, fiber, &seen);
  nx::thread other([&] {
    Context thread_context;
    nx::fiber::switch_to(thread_context, fiber, &seen);
  });
  other.join();

  CHECK(seen.runs == 2);
  CHECK(seen.ids[0] == nx::this_thread::id());
  CHECK(seen.ids[1] != seen.ids[0]);
  CHECK(fiber.suspended());
  pool.release(*stack);
}

TEST_CASE("fiber context: exceptions unwind within a fiber's stack") {
  StackPool pool;
  bool caught = false;
  fiber_test::run_on_fiber(pool, [&] {
    try {
      throw_from_depth(8);
    } catch (const std::runtime_error &) {
      caught = true;
    }
  });
  CHECK(caught);
}

TEST_CASE("fiber context: a reset context runs its entry again") {
  StackPool pool;
  auto stack = pool.acquire();
  REQUIRE(stack.has_value());

  struct Count {
    Context *self = nullptr;
    u32 runs = 0;
  } count;

  const nx::fiber::EntryFn entry = [](Transfer t) {
    auto *c = static_cast<Count *>(t.data);
    ++c->runs;
    nx::fiber::exit_to(*c->self, *t.from);
  };

  Context caller;
  Context fiber(*stack, entry);
  count.self = &fiber;
  nx::fiber::switch_to(caller, fiber, &count);
  fiber.reset(*stack, entry);
  nx::fiber::switch_to(caller, fiber, &count);
  CHECK(count.runs == 2);
  pool.release(*stack);
}

#if NX_HAS_WINDOWS_API
TEST_CASE("fiber context: the thread's stack bounds follow the running "
          "fiber") {
  StackPool pool;
  ULONG_PTR low = 0;
  ULONG_PTR high = 0;
  nx::fiber::Stack used;
  fiber_test::run_on_fiber(
      pool, [&] { GetCurrentThreadStackLimits(&low, &high); }, &used);

  CHECK(low == reinterpret_cast<ULONG_PTR>(used.reservation));
  CHECK(high == reinterpret_cast<ULONG_PTR>(used.base));

  ULONG_PTR thread_low = 0;
  ULONG_PTR thread_high = 0;
  GetCurrentThreadStackLimits(&thread_low, &thread_high);
  const auto here = reinterpret_cast<ULONG_PTR>(&thread_low);
  CHECK((here >= thread_low && here < thread_high));
}
#endif
