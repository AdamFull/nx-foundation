#include "fiber_test_support.h"

#include "core/foundation/platform/os.h"

#if NX_HAS_WINDOWS_API
#include <windows.h>
#endif

using nx::fiber::Stack;
using nx::fiber::StackPool;

namespace {

constexpr usize KIB = 1024;

NX_NO_INLINE u32 descend(u32 depth) {
  volatile u8 frame[1024];
  frame[0] = u8(depth);
  frame[sizeof(frame) - 1] = u8(depth >> 8);
  if (depth == 0)
    return frame[0];
  return descend(depth - 1) + frame[sizeof(frame) - 1];
}

// A frame over one page makes MSVC emit a __chkstk probe, which trusts the
// TIB's stack limit.
NX_NO_INLINE u32 one_large_frame() {
  volatile u8 frame[96 * KIB];
  for (usize i = 0; i < sizeof(frame); i += 4096)
    frame[i] = u8(i);
  return frame[0] + frame[sizeof(frame) - 4096];
}

} // namespace

TEST_CASE("fiber stack pool: stacks are page aligned, guarded and at least "
          "the configured size") {
  StackPool pool({.stack_size = 100 * KIB, .initial_commit = 8 * KIB});
  auto stack = pool.acquire();
  REQUIRE(stack.has_value());

  const usize page = nx::vmem::get_page_size();
  CHECK(stack->size() >= 100 * KIB);
  CHECK(stack->size() == pool.stack_size());
  CHECK(reinterpret_cast<uintptr_t>(stack->base) % page == 0);
  CHECK(nx::cast<u8 *>(stack->limit) - nx::cast<u8 *>(stack->reservation) ==
        isize(page));
  CHECK(nx::cast<u8 *>(stack->reservation) + stack->reservation_size ==
        stack->base);

#if NX_HAS_WINDOWS_API
  MEMORY_BASIC_INFORMATION guard = {};
  REQUIRE(VirtualQuery(stack->reservation, &guard, sizeof(guard)) != 0);
  CHECK(guard.State == MEM_RESERVE);
  CHECK(nx::cast<u8 *>(stack->base) - nx::cast<u8 *>(stack->committed) ==
        isize(8 * KIB));
#endif
  pool.release(*stack);
}

TEST_CASE("fiber stack pool: released stacks are reused most recent first") {
  StackPool pool;
  auto a = pool.acquire();
  auto b = pool.acquire();
  REQUIRE((a.has_value() && b.has_value()));
  CHECK(a->base != b->base);

  pool.release(*a);
  pool.release(*b);
  auto again = pool.acquire();
  REQUIRE(again.has_value());
  CHECK(again->base == b->base);
  pool.release(*again);
}

TEST_CASE("fiber stack pool: the cache is bounded and stats track stacks") {
  StackPool pool({.max_cached = 1});
  Stack stacks[3];
  for (Stack &stack : stacks) {
    auto acquired = pool.acquire();
    REQUIRE(acquired.has_value());
    stack = *acquired;
  }
  StackPool::Stats stats = pool.stats();
  CHECK(stats.live == 3);
  CHECK(stats.peak_live == 3);
  CHECK(stats.reserved_bytes == 3 * stacks[0].reservation_size);

  for (const Stack &stack : stacks)
    pool.release(stack);
  stats = pool.stats();
  CHECK(stats.live == 0);
  CHECK(stats.cached == 1);
  CHECK(stats.peak_live == 3);
  CHECK(stats.reserved_bytes == stacks[0].reservation_size);
}

TEST_CASE("fiber stack pool: a fiber can use its whole stack, growing it on "
          "demand") {
  StackPool pool({.stack_size = 512 * KIB, .initial_commit = 16 * KIB});
  u32 deep = 0;
  u32 large = 0;
  fiber_test::run_on_fiber(pool, [&] {
    deep = descend(300);
    large = one_large_frame();
  });
  CHECK(deep == descend(300));
  CHECK(large == one_large_frame());
}

TEST_CASE("fiber stack pool: only touched pages are resident and trim "
          "returns grown ones") {
  constexpr usize initial = 16 * KIB;
  StackPool pool({.stack_size = 512 * KIB, .initial_commit = initial});

  {
    auto fresh = pool.acquire();
    REQUIRE(fresh.has_value());
    CHECK(nx::fiber::resident_bytes(*fresh) <= initial);
    pool.release(*fresh);
  }

  Stack used;
  fiber_test::run_on_fiber(pool, [] { descend(200); }, &used);
  // ASan moves these frames to its heap-allocated fake stack.
#if !NX_ASAN_ENABLED
  CHECK(nx::fiber::resident_bytes(used) >= 200 * KIB);
#endif

  pool.trim();
  // MADV_FREE leaves pages resident until the system needs them.
#if !NX_ASAN_ENABLED && !NX_PLATFORM_APPLE
  CHECK(nx::fiber::resident_bytes(used) <= initial);
#endif

  u32 again = 0;
  fiber_test::run_on_fiber(pool, [&] { again = descend(200); });
  CHECK(again == descend(200));
}
