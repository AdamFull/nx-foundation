#pragma once

#include "framework/nxtest.h"

#include "core/foundation/fibers/context.h"
#include "core/foundation/fibers/stack.h"

#include <type_traits>

namespace fiber_test {

struct Call {
  void (*invoke)(void *);
  void *fn;
  nx::fiber::context *self;
};

// Runs fn to completion on a fiber from pool and returns to the caller. REQUIRE
// throws, and an exception must not escape a fiber, so fn may only CHECK.
template <typename F>
void run_on_fiber(nx::fiber::stack_pool &pool, F &&fn,
                  nx::fiber::stack *used = nullptr) {
  using Fn = std::remove_reference_t<F>;
  auto stack = pool.acquire();
  REQUIRE(stack.has_value());
  nx::fiber::context caller;
  nx::fiber::context fiber(*stack, [](nx::fiber::transfer t) {
    auto *call = static_cast<Call *>(t.data);
    call->invoke(call->fn);
    nx::fiber::exit_to(*call->self, *t.from);
  });
  Call call{[](void *p) { (*static_cast<Fn *>(p))(); }, &fn, &fiber};
  nx::fiber::switch_to(caller, fiber, &call);
  if (used)
    *used = *stack;
  pool.release(*stack);
}

} // namespace fiber_test
