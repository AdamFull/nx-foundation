#pragma once


#include "core/foundation/containers/fixed_vector.h"
#include "core/foundation/containers/small_vector.h"
#include "core/foundation/core/callable.h"
#include "core/foundation/core/foundation.h"

#include <atomic>
#include <chrono>

namespace nx {

enum class thread_priority {
  Low,
  BelowNormal,
  Normal,
  AboveNormal,
  High,
  Realtime
};

struct thread_options {
  const char *name = nullptr;
  /// Stack size in bytes; 0 uses the platform default.
  usize stack_size = 0;
  u64 affinity_mask = 0;
  thread_priority priority = thread_priority::Normal;
};

class thread {
public:
  using entry_fn = function<void()>;

  thread() = default;
  explicit thread(entry_fn fn, const thread_options &opts = {});

  template <typename F>
    requires(!std::is_same_v<std::decay_t<F>, thread> &&
             !std::is_same_v<std::decay_t<F>, entry_fn>)
  explicit thread(F &&fn, const thread_options &opts = {})
      : thread(entry_fn(std::forward<F>(fn)), opts) {}

  ~thread() {
    if (joinable())
      join();
  }

  thread(const thread &) = delete;
  thread &operator=(const thread &) = delete;

  thread(thread &&o) noexcept : m_handle(o.m_handle), m_id(o.m_id) {
    o.m_handle = 0;
    o.m_id = 0;
  }

  thread &operator=(thread &&o) noexcept {
    if (this != &o) {
      if (joinable())
        join();
      m_handle = o.m_handle;
      m_id = o.m_id;
      o.m_handle = 0;
      o.m_id = 0;
    }
    return *this;
  }

  [[nodiscard]] bool joinable() const noexcept { return m_handle != 0; }

  /// Blocks until the thread finishes. Must be joinable.
  void join();

  /// Releases ownership; the thread keeps running on its own.
  void detach();

  [[nodiscard]] u64 id() const noexcept { return m_id; }

  [[nodiscard]] void *native_handle() const noexcept {
    return reinterpret_cast<void *>(m_handle);
  }

private:
  uintptr_t m_handle = 0;
  u64 m_id = 0;
};

namespace this_thread {

u64 id() noexcept;

void yield() noexcept;

/// Sleeps for at least @p d (granularity is ~1ms on Windows).
void sleep_for(std::chrono::nanoseconds d) noexcept;

template <class Rep, class Period>
void sleep_for(std::chrono::duration<Rep, Period> d) noexcept {
  sleep_for(std::chrono::ceil<std::chrono::nanoseconds>(d));
}

inline constexpr usize NAME_CAPACITY = 32;

void set_name(const char *name) noexcept;

[[nodiscard]] const char *name() noexcept;

bool set_affinity(u64 mask) noexcept;

/// Returns the level actually granted, which is at most @p prio and is
/// thread_priority::Normal if the OS refused outright.
[[nodiscard]] thread_priority set_priority(thread_priority prio) noexcept;

}

/// Parks one thread at a time until unpark() or a deadline. Deadlines are kept
/// to well under a millisecond on every platform, which a Windows futex
/// timeout is not. An unpark() before the park is not lost; the parked thread
/// may also return spuriously, so callers re-check their condition.
class thread_parker {
public:
  thread_parker() noexcept;
  ~thread_parker() noexcept;
  thread_parker(const thread_parker &) = delete;
  thread_parker &operator=(const thread_parker &) = delete;

  void park_until(std::chrono::steady_clock::time_point deadline) noexcept;
  void park() noexcept {
    park_until(std::chrono::steady_clock::time_point::max());
  }
  void unpark() noexcept;

private:
#if defined(_WIN32)
  void *m_event = nullptr;
  void *m_timer = nullptr;
#else
  std::atomic<u32> m_state{0};
#endif
};

namespace cpu {

enum class cache_type : u8 { Unified, Data, Instruction };

struct cache_info {
  u8 level = 0;
  cache_type type = cache_type::Unified;
  u32 line_size = 0; ///< bytes
  u64 size = 0;      ///< bytes per instance
  u32 instance_count = 0;
};

struct core_info {
  u32 index = 0;
  u32 package = 0;
  u32 efficiency_class = 0;
  u32 logical_count = 0;
  u64 logical_mask = 0;
};

struct topology_info {
  u32 logical_count = 0;
  u32 physical_count = 0;
  u32 package_count = 0;
  u32 numa_node_count = 0;
  u32 cache_line_size = 0;
  u32 efficiency_class_count = 0;
  small_vector<core_info, 64> cores;
  fixed_vector<cache_info, 16> caches;
};

/// Full machine description; queried once, cached for the process lifetime.
const topology_info &topology() noexcept;

u32 logical_core_count() noexcept;

u32 physical_core_count() noexcept;

/// Data cache line size in bytes (64 if it cannot be determined).
u32 cache_line_size() noexcept;

u32 performance_core_count() noexcept;

u32 efficiency_core_count() noexcept;

}

}
