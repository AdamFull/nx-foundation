#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/diagnostics/log.h"

#include <format>
#include <utility>

namespace nx {

// Debugger helpers. They act only in NX_DEBUG builds; elsewhere they do
// nothing, and dbg_get_pid returns 0.
void dbg_break() noexcept;
[[nodiscard]] long dbg_get_pid() noexcept;
[[nodiscard]] bool dbg_is_present() noexcept;
/// Waits up to @p timeout_ms for a debugger to attach, then breaks into it;
/// a negative timeout waits for as long as it takes, zero not at all.
void dbg_wait(i32 timeout_ms) noexcept;

namespace detail {

template <typename... Args>
  requires(sizeof...(Args) > 0)
[[noreturn]] void assert_fail(const char *file, const int line,
                              const char *cond, const string_view fmt,
                              Args &&...args) noexcept {
  char buf[512];
  assert_fail(file, line, cond,
              nx::vformat_to(buf, fmt, std::make_format_args(args...)));
}

}
}
