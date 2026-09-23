#pragma once

#include "core/foundation/core/foundation.h"

namespace nx::fiber {

struct Stack;
class Context;

struct Transfer {
  Context *from = nullptr;
  void *data = nullptr;
};

/// Must never return: a fiber ends by switching away for the last time.
using EntryFn = void (*)(Transfer);

/// Suspends `from` and resumes `to`. Returns when something switches back to
/// `from`, with the context that did so and the data it passed. The caller may
/// then be on another OS thread, so it must not reuse thread_local addresses
/// computed before the call.
Transfer switch_to(Context &from, Context &to, void *data = nullptr) noexcept;

/// Like switch_to, for a `from` that will never be resumed.
[[noreturn]] void exit_to(Context &from, Context &to,
                          void *data = nullptr) noexcept;

/// A default-constructed context adopts whatever runs on the thread when it is
/// first switched away from; one built from a stack starts `entry` when first
/// switched to.
class Context {
public:
  Context() noexcept = default;
  Context(const Stack &stack, EntryFn entry) noexcept;
  ~Context() noexcept;

  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;

  void reset(const Stack &stack, EntryFn entry) noexcept;

  [[nodiscard]] bool suspended() const noexcept { return m_sp != nullptr; }

private:
  friend Transfer switch_to(Context &, Context &, void *) noexcept;
  friend void exit_to(Context &, Context &, void *) noexcept;
  friend struct ContextAccess;

  void *m_sp = nullptr;
  EntryFn m_entry = nullptr;
  const void *m_stack_bottom = nullptr;
  usize m_stack_size = 0;
#if NX_ASAN_ENABLED
  void *m_asan_fake_stack = nullptr;
#endif
#if NX_TSAN_ENABLED
  void *m_tsan_fiber = nullptr;
  bool m_owns_tsan_fiber = false;
#endif
};

} // namespace nx::fiber
