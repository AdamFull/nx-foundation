
#include "framework/nxtest.h"

#include "core/foundation/core/callable.h"
#include "core/foundation/strings/utf8_string_view.h"

#include <type_traits>

namespace {

[[nodiscard]] int doubled(const int x) { return x * 2; }

[[nodiscard]] int apply(const nx::function_ref<int(int)> fn, const int x) {
  return fn(x);
}

int calls = 0;

}

TEST_CASE("function_ref: a lambda needs no type at the call site") {
  CHECK(apply([](const int x) { return x + 1; }, 41) == 42);
  CHECK(apply(doubled, 21) == 42);
}

TEST_CASE("function_ref: it sees the caller's own state, not a copy") {
  int total = 0;
  const auto adder = [&](const int x) { total += x; };
  const nx::function_ref<void(int)> add = adder;
  add(2);
  add(3);
  CHECK(total == 5);

  int seen = 0;
  auto counter = [&seen](const int x) mutable { seen += x; };
  const nx::function_ref<void(int)> ref = counter;
  ref(1);
  ref(1);
  CHECK(seen == 2);
}

TEST_CASE("function_ref: a temporary argument outlives the call it is made for") {
  int captured = 7;
  CHECK(apply([&](const int x) { return x + captured; }, 35) == 42);
}

TEST_CASE("function_ref: two pointers and no allocation") {
  CHECK(sizeof(nx::function_ref<int(int)>) == 2 * sizeof(void *));
  CHECK(std::is_trivially_copyable_v<nx::function_ref<int(int)>>);
}

TEST_CASE("function_ref: a default one is empty and converts to false") {
  const nx::function_ref<int(int)> empty;
  CHECK_FALSE(static_cast<bool>(empty));

  const nx::function_ref<int(int)> filled = doubled;
  CHECK(static_cast<bool>(filled));
}

TEST_CASE("function_ref: arguments and results pass through unchanged") {
  const auto picker = [](const nx::string_view s, const int n) {
    return n > 0 ? s : nx::string_view{};
  };
  const nx::function_ref<nx::string_view(nx::string_view, int)> pick = picker;
  CHECK(pick("kept", 1) == "kept");
  CHECK(pick("dropped", 0).empty());

  calls = 0;
  const auto bumper = [] { ++calls; };
  const nx::function_ref<void()> bump = bumper;
  bump();
  bump();
  CHECK(calls == 2);
}
