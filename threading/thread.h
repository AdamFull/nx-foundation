#pragma once

/**
 * @file thread.h
 * @brief Cross-platform threads (nx::thread), current-thread utilities
 * (nx::this_thread) and CPU topology queries (nx::cpu).
 *
 * nx::thread wraps a native OS thread (Win32 / pthreads) with engine
 * conveniences: naming, stack size, affinity and priority are set at
 * creation through nx::thread_options. Unlike std::thread, destroying a
 * joinable nx::thread joins it instead of terminating the process.
 *
 * nx::cpu::topology() describes the machine once and caches the result:
 * logical/physical core counts, SMT layout, hybrid (P/E core) efficiency
 * classes, cache hierarchy and NUMA node count.
 */

#include "core/foundation/containers/fixed_vector.h"
#include "core/foundation/containers/small_vector.h"
#include "core/foundation/core/callable.h"
#include "core/foundation/core/foundation.h"

#include <chrono>

namespace nx {

/// Scheduling priority of a thread, mapped to the closest native level.
///
/// Realtime is for a thread with a hard deadline - an audio mixer feeding a
/// device callback - and is the one level the OS may refuse: it needs
/// CAP_SYS_NICE on Linux, and Apple's per-thread knob does not reach that far
/// at all. Ask for it through set_priority and read what you were given.
enum class thread_priority {
  Low,
  BelowNormal,
  Normal,
  AboveNormal,
  High,
  Realtime
};

/// Creation parameters for nx::thread. All fields are optional.
struct thread_options {
  /// Thread name for debuggers/profilers (truncated to 15 chars on Linux).
  const char *name = nullptr;
  /// Stack size in bytes; 0 uses the platform default.
  usize stack_size = 0;
  /// Affinity bitmask over logical processors; 0 leaves the thread unpinned.
  u64 affinity_mask = 0;
  thread_priority priority = thread_priority::Normal;
};

/**
 * @brief An OS thread.
 *
 * Move-only. The destructor joins a still-joinable thread, so a thread
 * never outlives its owner by accident; call detach() to opt out. Name,
 * affinity and priority from thread_options are applied by the new thread
 * itself before the entry function runs.
 */
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

  /// Opaque unique thread id. On Windows this is the OS thread id; on
  /// POSIX it is derived from the pthread handle (not the kernel tid).
  [[nodiscard]] u64 id() const noexcept { return m_id; }

  /// Native handle: HANDLE on Windows, pthread_t on POSIX.
  [[nodiscard]] void *native_handle() const noexcept {
    return reinterpret_cast<void *>(m_handle);
  }

private:
  uintptr_t m_handle = 0;
  u64 m_id = 0;
};

/// Operations on the calling thread.
namespace this_thread {

/// OS thread id of the calling thread (kernel tid; cached in TLS).
u64 id() noexcept;

/// Offers the remainder of the time slice to another ready thread.
void yield() noexcept;

/// Sleeps for at least @p d (granularity is ~1ms on Windows).
void sleep_for(std::chrono::nanoseconds d) noexcept;

template <class Rep, class Period>
void sleep_for(std::chrono::duration<Rep, Period> d) noexcept {
  sleep_for(std::chrono::ceil<std::chrono::nanoseconds>(d));
}

/// Names the calling thread for debuggers/profilers.
void set_name(const char *name) noexcept;

/// Pins the calling thread to the logical processors set in @p mask.
bool set_affinity(u64 mask) noexcept;

/// Returns the level actually granted, which is at most @p prio and is
/// thread_priority::Normal if the OS refused outright.
[[nodiscard]] thread_priority set_priority(thread_priority prio) noexcept;

} // namespace this_thread

/// CPU topology queries. All results are gathered once and cached.
namespace cpu {

enum class cache_type : u8 { Unified, Data, Instruction };

/// One level of the cache hierarchy, aggregated over identical instances.
struct cache_info {
  u8 level = 0; ///< 1, 2 or 3
  cache_type type = cache_type::Unified;
  u32 line_size = 0; ///< bytes
  u64 size = 0;      ///< bytes per instance
  u32 instance_count = 0;
};

/// One physical core and the logical processors it hosts.
struct core_info {
  u32 index = 0;            ///< dense physical core index
  u32 package = 0;          ///< physical package (socket) index
  u32 efficiency_class = 0; ///< 0 = most efficient; higher = faster (P-core)
  u32 logical_count = 0;    ///< SMT width (1 = no hyper-threading)
  u64 logical_mask = 0;     ///< affinity bitmask (first 64 logical CPUs)
};

struct topology_info {
  u32 logical_count = 0;  ///< logical processors (physical + SMT)
  u32 physical_count = 0; ///< physical cores
  u32 package_count = 0;
  u32 numa_node_count = 0;
  u32 cache_line_size = 0;
  u32 efficiency_class_count = 0; ///< 1 unless the CPU is hybrid (P+E cores)
  small_vector<core_info, 64> cores;
  fixed_vector<cache_info, 16> caches;
};

/// Full machine description; queried once, cached for the process lifetime.
const topology_info &topology() noexcept;

/// Number of logical processors (physical cores + SMT siblings).
u32 logical_core_count() noexcept;

/// Number of physical cores.
u32 physical_core_count() noexcept;

/// Data cache line size in bytes (64 if it cannot be determined).
u32 cache_line_size() noexcept;

/// Physical cores in the highest efficiency class (all cores when uniform).
u32 performance_core_count() noexcept;

/// Physical cores below the highest efficiency class (0 when uniform).
u32 efficiency_core_count() noexcept;

} // namespace cpu

} // namespace nx
