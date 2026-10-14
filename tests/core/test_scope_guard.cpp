#include "framework/nxtest.h"

#include "core/foundation/core/foundation.h"
#include "core/foundation/core/scope_guard.h"

#include <string>
#include <utility>
#include <vector>

TEST_CASE("scope_guard: runs the action on scope exit") {
  int calls = 0;
  {
    const nx::scope_guard g([&calls] { ++calls; });
    CHECK(calls == 0);
  }
  CHECK(calls == 1);
}

TEST_CASE("scope_guard: dismiss cancels the action") {
  int calls = 0;
  {
    nx::scope_guard g([&calls] { ++calls; });
    g.dismiss();
  }
  CHECK(calls == 0);

  {
    nx::scope_guard g([&calls] { ++calls; });
    g.dismiss();
    g.dismiss();
  }
  CHECK(calls == 0);
}

TEST_CASE("scope_guard: moving transfers ownership exactly once") {
  int calls = 0;
  {
    nx::scope_guard outer([&calls] { ++calls; });
    {
      const nx::scope_guard inner(std::move(outer));
      CHECK(calls == 0);
    }
    CHECK(calls == 1);
  }
  CHECK(calls == 1);
}

TEST_CASE("scope_guard: a moved-from guard is dismissed") {
  int calls = 0;
  {
    nx::scope_guard g([&calls] { ++calls; });
    g.dismiss();
    const nx::scope_guard moved(std::move(g));
  }
  CHECK(calls == 0);
}

TEST_CASE("scope_guard: fires while an exception unwinds the scope") {
  int calls = 0;
  try {
    const nx::scope_guard g([&calls] { ++calls; });
    throw std::string("boom");
  } catch (const std::string &e) {
    CHECK(e == "boom");
  }
  CHECK(calls == 1);
}

TEST_CASE("scope_guard: guards unwind in reverse construction order") {
  std::vector<int> order;
  {
    const nx::scope_guard a([&order] { order.push_back(1); });
    const nx::scope_guard b([&order] { order.push_back(2); });
    const nx::scope_guard c([&order] { order.push_back(3); });
  }
  REQUIRE(order.size() == 3);
  CHECK(order[0] == 3);
  CHECK(order[1] == 2);
  CHECK(order[2] == 1);
}

TEST_CASE("scope_guard: scoped_exit deduces the callable and defers it") {
  int calls = 0;
  {
    const auto g = nx::scoped_exit([&calls] { ++calls; });
    CHECK(calls == 0);
  }
  CHECK(calls == 1);

  {
    auto g = nx::scoped_exit([&calls] { ++calls; });
    g.dismiss();
  }
  CHECK(calls == 1);
}

TEST_CASE("scope_guard: scoped_exit accepts a function pointer") {
  static int fp_calls = 0;
  fp_calls = 0;
  {
    const auto g = nx::scoped_exit(+[] { ++fp_calls; });
    CHECK(fp_calls == 0);
  }
  CHECK(fp_calls == 1);
}

TEST_CASE("scope_guard: defer runs the block at scope exit") {
  int calls = 0;
  {
    defer { ++calls; };
    CHECK(calls == 0);
  }
  CHECK(calls == 1);
}

TEST_CASE("scope_guard: several defers in one scope unwind in reverse order") {
  std::vector<int> order;
  {
    defer { order.push_back(1); };
    defer { order.push_back(2); };
    defer { order.push_back(3); };
  }
  REQUIRE(order.size() == 3);
  CHECK(order[0] == 3);
  CHECK(order[1] == 2);
  CHECK(order[2] == 1);
}

TEST_CASE("scope_guard: defer captures by reference and sees the final value") {
  int value = 0;
  int captured = -1;
  {
    defer { captured = value; };
    value = 99;
  }
  CHECK(captured == 99);
}

TEST_CASE("scope_guard: defer fires during exception unwinding") {
  bool ran = false;
  try {
    defer { ran = true; };
    throw 42;
  } catch (int e) {
    CHECK(e == 42);
  }
  CHECK(ran);
}

TEST_CASE("scope_guard: guards inside a loop fire once per iteration") {
  int calls = 0;
  for (int i = 0; i < 5; ++i) {
    defer { ++calls; };
    CHECK(calls == i);
  }
  CHECK(calls == 5);
}

TEST_CASE("scope_guard: releases a resource on the early-return path") {
  bool freed = false;
  const auto body = [&freed](bool bail) {
    void *resource = nx::mem_alloc(64);
    defer {
      nx::mem_free(resource);
      freed = true;
    };
    if (bail)
      return -1;
    return 0;
  };

  CHECK(body(true) == -1);
  CHECK(freed);

  freed = false;
  CHECK(body(false) == 0);
  CHECK(freed);
}

TEST_CASE("scope_guard: is move-only and nodiscard") {
  static_assert(!std::is_copy_constructible_v<nx::scope_guard<void (*)()>>);
  static_assert(!std::is_copy_assignable_v<nx::scope_guard<void (*)()>>);
  static_assert(!std::is_move_assignable_v<nx::scope_guard<void (*)()>>);
  static_assert(std::is_move_constructible_v<nx::scope_guard<void (*)()>>);
  CHECK(true);
}

TEST_CASE("scope_guard: holds a stateful mutable functor by value") {
  int observed = 0;
  struct Counter {
    int n = 0;
    int *out;
    void operator()() { *out = ++n; }
  };
  {
    const nx::scope_guard g{Counter{0, &observed}};
  }
  CHECK(observed == 1);
}

TEST_CASE("scoped_value: restores nested temporary values") {
  int value = 1;
  {
    const nx::scoped_value outer(value, 2);
    CHECK(value == 2);
    {
      const nx::scoped_value inner(value, 3);
      CHECK(value == 3);
    }
    CHECK(value == 2);
  }
  CHECK(value == 1);
}

TEST_CASE("scoped_value: moving transfers restoration exactly once") {
  int value = 1;
  {
    nx::scoped_value original(value, 2);
    CHECK(value == 2);
    const nx::scoped_value moved(std::move(original));
    CHECK(value == 2);
  }
  CHECK(value == 1);

  static_assert(!std::is_copy_constructible_v<nx::scoped_value<int>>);
  static_assert(!std::is_copy_assignable_v<nx::scoped_value<int>>);
  static_assert(!std::is_move_assignable_v<nx::scoped_value<int>>);
  static_assert(std::is_move_constructible_v<nx::scoped_value<int>>);
}
