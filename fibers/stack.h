#pragma once

#include "core/foundation/containers/small_vector.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/core/result.h"
#include "core/foundation/threading/sync.h"

namespace nx::fiber {

/// A fiber stack. It grows down from `base` towards `limit`; the pages below
/// `limit` are an inaccessible guard. On Windows only `[committed, base)` is
/// committed up front and the rest is committed by the OS as the stack grows.
struct Stack {
  void *reservation = nullptr;
  usize reservation_size = 0;
  void *limit = nullptr;
  void *committed = nullptr;
  void *base = nullptr;

  [[nodiscard]] usize size() const noexcept {
    return nx::cast<usize>(nx::cast<u8 *>(base) - nx::cast<u8 *>(limit));
  }
  [[nodiscard]] explicit operator bool() const noexcept {
    return base != nullptr;
  }
};

enum class StackError : u8 { OutOfMemory };

/// Bytes of the stack currently backed by physical memory (POSIX) or
/// committed (Windows).
[[nodiscard]] usize resident_bytes(const Stack &stack) noexcept;

/// Reserves guarded, lazily backed fiber stacks and recycles them.
class StackPool {
public:
  struct Config {
    usize stack_size = 256 * 1024;
    usize initial_commit = 16 * 1024;
    u32 max_cached = 64;
  };

  struct Stats {
    u32 live = 0;
    u32 cached = 0;
    u32 peak_live = 0;
    usize reserved_bytes = 0;
  };

  StackPool() noexcept : StackPool(Config{}) {}
  explicit StackPool(const Config &config) noexcept;
  ~StackPool() noexcept;

  StackPool(const StackPool &) = delete;
  StackPool &operator=(const StackPool &) = delete;

  [[nodiscard]] result<Stack, StackError> acquire() noexcept;
  void release(const Stack &stack) noexcept;

  /// Returns the physical memory of cached stacks that grew past the initial
  /// commit to the OS.
  void trim() noexcept;

  [[nodiscard]] Stats stats() const noexcept;
  [[nodiscard]] usize stack_size() const noexcept { return m_stack_size; }

private:
  usize m_page_size = 0;
  usize m_guard_size = 0;
  usize m_reservation_size = 0;
  usize m_stack_size = 0;
  usize m_initial_commit = 0;
  u32 m_max_cached = 0;

  mutable mutex m_lock;
  small_vector<Stack, 16> m_cached;
  Stats m_stats;
};

} // namespace nx::fiber
