#include "core/foundation/diagnostics/diagnostic.h"

#include <cstdio>
#include <cstdlib>

#if NX_HAS_WINDOWS_API
#include <windows.h>
#elif NX_PLATFORM_POSIX
#include <unistd.h>
#endif

namespace nx {

#if NX_DEBUG
void dbg_break() noexcept {
#if NX_HAS_WINDOWS_API
  DebugBreak();
#elif defined(__clang__) || defined(__GNUC__)
  __builtin_trap();
#endif
}

long dbg_get_pid() noexcept {
#if NX_HAS_WINDOWS_API
  return nx::cast<long>(GetCurrentProcessId());
#elif NX_PLATFORM_POSIX
  return nx::cast<long>(getpid());
#else
  return 0;
#endif
}

bool dbg_is_present() noexcept {
#if NX_HAS_WINDOWS_API
  return IsDebuggerPresent() != FALSE;
#else
  return false;
#endif
}

void dbg_wait(const i32 timeout_ms) noexcept {
  if (dbg_is_present() || timeout_ms == 0)
    return;

  std::printf("Waiting for debugger to attach (PID: %ld)...\n", dbg_get_pid());

  i32 elapsed_ms = 0;
  while (!dbg_is_present()) {
#if NX_HAS_WINDOWS_API
    Sleep(500);
#elif NX_PLATFORM_POSIX
    usleep(500000);
#endif
    elapsed_ms += 500;
    if (timeout_ms > 0 && elapsed_ms >= timeout_ms) {
      std::printf("Timeout reached, continuing without debugger.\n");
      return;
    }
  }

  std::printf("Debugger attached.\n");
  dbg_break();
}
#else
void dbg_break() noexcept {}
long dbg_get_pid() noexcept { return 0; }
bool dbg_is_present() noexcept { return false; }
void dbg_wait(const i32 timeout_ms) noexcept { (void)timeout_ms; }
#endif

namespace detail {

void assert_fail(const char *file, const int line, const char *cond,
                 const char *msg) noexcept {
  // An assertion that fails inside the logger would come back here forever.
  static thread_local bool failing = false;
  if (!failing) {
    failing = true;
    if (msg)
      nx::logc(nx::log::app, "assertion failed: ({}) at {}:{} — {}", cond, file,
               line, msg);
    else
      nx::logc(nx::log::app, "assertion failed: ({}) at {}:{}", cond, file,
               line);
    nx::log::flush();
  }
  nx::dbg_break();
  std::abort();
}

}
}
