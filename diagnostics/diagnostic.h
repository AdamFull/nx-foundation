#pragma once

#include "foundation/core/foundation.h"
#include "foundation/diagnostics/log.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#elif defined(NX_PLATFORM_POSIX)
#include <unistd.h>
#endif

namespace nx {
#if !defined(NDEBUG)
static void dbg_break() {
#if defined(_WIN32)
  DebugBreak();
#elif defined(__clang__) || defined(__GNUC__)
  __builtin_trap();
#endif
}

static long dbg_get_pid() {
#if defined(_WIN32)
  return nx::cast<long>(GetCurrentProcessId());
#elif defined(NX_PLATFORM_POSIX)
  return nx::cast<long>(getpid());
#else
  return 0;
#endif
}

static bool dbg_is_present() {
#if defined(_WIN32)
  return IsDebuggerPresent();
#else
  return false;
#endif
}

static void dbg_wait(const i32 timeout_ms) {
  if (dbg_is_present() || timeout_ms == 0)
    return;

  printf("Waiting for debugger to attach (PID: %lu)...\n", dbg_get_pid());

  u32 elapsed_ms = 0;
  while (!dbg_is_present()) {
#if defined(_WIN32)
    Sleep(500);
#elif defined(NX_PLATFORM_POSIX)
    usleep(500000);
#endif
    elapsed_ms += 500;
    if (timeout_ms > 0 && elapsed_ms >= timeout_ms) {
      printf("Timeout reached, continuing without debugger.\n");
      return;
    }
  }

  printf("Debugger attached.\n");
  dbg_break();
}
#else
static void dbg_break() {}
static long dbg_get_pid() { return 0; }
static bool dbg_is_present() { return false; }
static void dbg_wait(i32 timeout_ms) { (void)timeout_ms; }
#endif

namespace detail {

// Logs a critical assertion message, flushes the (async) logger so it is not
// lost, breaks into an attached debugger, then aborts. Not reached in release
// builds (NX_ASSERT compiles the check out there).
[[noreturn]] [[maybe_unused]] static void
assert_fail(const char *file, const int line, const char *cond,
            const char *msg = nullptr) noexcept {
  if (msg)
    nx::logc(nx::log::app, "assertion failed: ({}) at {}:{} — {}", cond, file,
             line, msg);
  else
    nx::logc(nx::log::app, "assertion failed: ({}) at {}:{}", cond, file, line);
  nx::log::flush();
  nx::dbg_break();
  ::abort();
}

template <typename... Args>
[[noreturn]] static void assert_fail(const char *file, const int line,
                                     const char *cond, string_view fmt,
                                     Args &&...args) {
  char buf[512];
  assert_fail(file, line, cond,
              nx::format_to(buf, fmt, std::forward<Args>(args)...));
}

} // namespace detail
} // namespace nx

// Runtime assertion. Active only in debug builds; compiled out (but still
// type-checked) in release. Optional formatted message:
//   NX_ASSERT(ptr != nullptr);
//   NX_ASSERT(i < n, "index {} out of range {}", i, n);
#if NX_DEBUG
#define NX_ASSERT(cond, ...)                                                   \
  do {                                                                         \
    if (!(cond)) [[unlikely]]                                                  \
      ::nx::detail::assert_fail(__FILE__, __LINE__, #cond __VA_OPT__(, )       \
                                    __VA_ARGS__);                              \
  } while (0)
#else
#define NX_ASSERT(cond, ...) ((void)sizeof(!(cond)))
#endif