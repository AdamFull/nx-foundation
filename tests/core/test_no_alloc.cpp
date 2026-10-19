#include "framework/nxtest.h"

#include "core/foundation/core/foundation.h"

#include <atomic>
#include <thread>

TEST_CASE("scoped_no_alloc: forbids allocation for its scope") {
#if NX_ALLOC_GUARD
  const bool before = nx::allocation_allowed();
  bool inside = true;
  {
    const nx::scoped_no_alloc guard;
    inside = nx::allocation_allowed();
  }
  const bool after = nx::allocation_allowed();

  CHECK(before);
  CHECK_FALSE(inside);
  CHECK(after);
#else
  SKIP("the allocation guard is compiled out in this configuration");
#endif
}

TEST_CASE("scoped_no_alloc: nests, and only the outermost exit lifts it") {
#if NX_ALLOC_GUARD
  bool outer = true;
  bool inner = true;
  bool after_inner = true;
  {
    const nx::scoped_no_alloc a;
    outer = nx::allocation_allowed();
    {
      const nx::scoped_no_alloc b;
      inner = nx::allocation_allowed();
    }
    after_inner = nx::allocation_allowed();
  }

  CHECK_FALSE(outer);
  CHECK_FALSE(inner);
  CHECK_FALSE(after_inner);
  CHECK(nx::allocation_allowed());
#else
  SKIP("the allocation guard is compiled out in this configuration");
#endif
}

TEST_CASE("scoped_no_alloc: binds to one thread and leaves the others alone") {
#if NX_ALLOC_GUARD
  std::atomic<bool> guarded{false};
  std::atomic<bool> other_allowed{false};
  std::atomic<bool> done{false};

  std::thread observer([&] {
    while (!guarded.load(std::memory_order_acquire))
      std::this_thread::yield();
    other_allowed.store(nx::allocation_allowed(), std::memory_order_relaxed);
    done.store(true, std::memory_order_release);
  });

  bool here = true;
  {
    const nx::scoped_no_alloc guard;
    here = nx::allocation_allowed();
    guarded.store(true, std::memory_order_release);
    while (!done.load(std::memory_order_acquire))
      std::this_thread::yield();
  }
  observer.join();

  CHECK_FALSE(here);
  CHECK(other_allowed.load(std::memory_order_relaxed));
#else
  SKIP("the allocation guard is compiled out in this configuration");
#endif
}

TEST_CASE("scoped_no_alloc: compiles away when the guard is off") {
#if NX_ALLOC_GUARD
  SKIP("the allocation guard is compiled in for this configuration");
#else
  static_assert(nx::allocation_allowed(),
                "with the guard off, allocation_allowed must fold to true");
  const nx::scoped_no_alloc guard;
  CHECK(nx::allocation_allowed());
#endif
}

TEST_CASE("scoped_no_alloc: an empty nx::vector may be made and dropped") {
  usize size = 1;
  {
    const nx::scoped_no_alloc guard;
    const nx::vector<int> empty;
    size = empty.size();
  }
  CHECK(size == 0);
}
