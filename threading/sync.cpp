#include "foundation/threading/sync.h"

#if NX_HAS_WINDOWS_API
#include <windows.h>
// Do not reorder!
#include <synchapi.h>
#elif NX_PLATFORM_POSIX
#include <cerrno>
#include <climits>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace nx::futex {

#if NX_PLATFORM_POSIX
static long futex_syscall(std::atomic<u32> *addr, i32 op, u32 val,
                          const timespec *timeout = nullptr) {
  return syscall(SYS_futex, addr, op, val, timeout, nullptr, 0);
}
#endif

Result wait(std::atomic<u32> *addr, u32 expected_value,
            const std::chrono::nanoseconds timeout) {
  if (!addr)
    return Result::Error;

#if NX_HAS_WINDOWS_API
  DWORD timeout_ms = INFINITE;
  if (timeout != std::chrono::nanoseconds::zero()) {
    // Round up so sub-millisecond timeouts do not degrade to a busy poll.
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(timeout);
    timeout_ms = nx::cast<DWORD>(ms.count());
  }

  if (const BOOL result =
          WaitOnAddress(addr, &expected_value, sizeof(u32), timeout_ms);
      !result) {
    if (const DWORD error = GetLastError(); error == ERROR_TIMEOUT)
      return Result::TimedOut;
    return Result::Error;
  }
#elif NX_PLATFORM_POSIX
  timespec ts;
  timespec *ts_ptr = nullptr;

  if (timeout != std::chrono::nanoseconds::zero()) {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    auto nsecs =
        timeout - std::chrono::duration_cast<std::chrono::nanoseconds>(secs);

    ts.tv_sec = secs.count();
    ts.tv_nsec = nsecs.count();
    ts_ptr = &ts;
  }

  long result = futex_syscall(addr, FUTEX_WAIT_PRIVATE, expected_value, ts_ptr);

  if (result == 0)
    return Result::Success;

  if (errno == ETIMEDOUT)
    return Result::TimedOut;

  if (errno == EAGAIN)
    return Result::Success;
#endif

  return Result::Success;
}

i32 wake(std::atomic<u32> *addr, const i32 count) {
  if (!addr)
    return 0;

#if NX_HAS_WINDOWS_API
  for (i32 i = 0; i < count; ++i)
    WakeByAddressSingle(addr);
#elif NX_PLATFORM_POSIX
  long result = futex_syscall(addr, FUTEX_WAKE_PRIVATE, count);
  return result >= 0 ? nx::cast<i32>(result) : 0;
#endif

  return count;
}

i32 wake_all(std::atomic<u32> *addr) {
  if (!addr)
    return 0;

#if NX_HAS_WINDOWS_API
  WakeByAddressAll(addr);
#elif NX_PLATFORM_POSIX
  long result = futex_syscall(addr, FUTEX_WAKE_PRIVATE, INT_MAX);
  return result >= 0 ? nx::cast<i32>(result) : 0;
#endif
  return 1;
}

} // namespace nx::futex
