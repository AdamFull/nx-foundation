#pragma once

#include "core/foundation/core/foundation.h"

namespace nx::fiber {

struct stack;
class context;

struct transfer {
  context *from = nullptr;
  void *data = nullptr;
};

/// Must never return: a fiber ends by switching away for the last time.
using entry_fn = void (*)(transfer);

/// Suspends `from` and resumes `to`. Returns when something switches back to
/// `from`, with the context that did so and the data it passed. The caller may
/// then be on another OS thread, so it must not reuse thread_local addresses
/// computed before the call.
transfer switch_to(context &from, context &to, void *data = nullptr) noexcept;

/// Like switch_to, for a `from` that will never be resumed.
[[noreturn]] void exit_to(context &from, context &to,
                          void *data = nullptr) noexcept;

/// A default-constructed context adopts whatever runs on the thread when it is
/// first switched away from; one built from a stack starts `entry` when first
/// switched to.
class context {
public:
  context() noexcept = default;
  context(const fiber::stack &stack, entry_fn entry) noexcept;
  ~context() noexcept;

  context(const context &) = delete;
  context &operator=(const context &) = delete;

  void reset(const fiber::stack &stack, entry_fn entry) noexcept;

  [[nodiscard]] bool suspended() const noexcept { return m_sp != nullptr; }

private:
  friend transfer switch_to(context &, context &, void *) noexcept;
  friend void exit_to(context &, context &, void *) noexcept;
  friend struct ContextAccess;

  void *m_sp = nullptr;
  entry_fn m_entry = nullptr;
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

/// Deprecated: the names before foundation types were snake_case.
using Context = context;
using Transfer = transfer;
using EntryFn = entry_fn;

} // namespace nx::fiber
