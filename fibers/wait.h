#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/threading/sync.h"

#include <atomic>
#include <chrono>

namespace nx::fiber {
struct Fiber;
}

// The blocking layer under nx::fiber's synchronization primitives. A waiter
// is a fiber when the caller runs on a scheduler, otherwise a blocked thread.
namespace nx::fiber::internal {

using clock = std::chrono::steady_clock;
inline constexpr clock::time_point NO_DEADLINE = clock::time_point::max();

struct WaitList;

struct Waiter {
  static constexpr u32 IDLE = 0;
  static constexpr u32 WAITING = 1;
  static constexpr u32 WOKEN = 2;
  static constexpr u32 TIMED_OUT = 3;
  static constexpr u32 NOT_IN_HEAP = ~u32(0);

  Waiter *next = nullptr;
  Fiber *fiber = nullptr;
  WaitList *list = nullptr;
  spin_lock *lock = nullptr;
  clock::time_point deadline = NO_DEADLINE;
  u32 heap_index = NOT_IN_HEAP;
  std::atomic<u32> state{IDLE};
};

struct WaitList {
  Waiter *head = nullptr;
  Waiter *tail = nullptr;

  [[nodiscard]] bool empty() const noexcept { return head == nullptr; }
  void push(Waiter *waiter) noexcept;
  bool remove(Waiter *waiter) noexcept;
};

/// A waiter taken off a list. Only these fields may be used after the lock is
/// released: a thread waiter's record dies as soon as it sees WOKEN.
struct Claimed {
  Fiber *fiber = nullptr;
  std::atomic<u32> *word = nullptr;

  [[nodiscard]] explicit operator bool() const noexcept {
    return word != nullptr;
  }
};

enum class WaitResult : u8 { Woken, TimedOut };

/// The caller holds `lock`; it is released once the caller is on `list`, and
/// not held on return. `lock` and `list` may both be null to only sleep.
WaitResult block(spin_lock *lock, WaitList *list,
                 clock::time_point deadline) noexcept;

/// The caller holds the list's lock. Takes the first waiter that has not timed
/// out.
Claimed claim(WaitList &list) noexcept;

void resume(const Claimed &claimed) noexcept;

} // namespace nx::fiber::internal
