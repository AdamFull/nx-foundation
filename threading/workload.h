#pragma once

#include "core/foundation/platform/os.h"
#include "core/foundation/threading/cancellation.h"

#include <atomic>
#include <functional>
#include <type_traits>
#include <utility>

namespace nx {

/// Cumulative measurements for one named asynchronous workload. Queue and
/// execution timings are kept outside thread_pool task metadata so ordinary
/// tasks remain compact and subsystems only pay for telemetry they request.
struct workload_statistics {
  u64 submitted = 0;
  u64 started = 0;
  u64 completed = 0;
  u64 cancelled = 0;
  u64 failed = 0;
  u64 rejected = 0;
  u64 queue_ns = 0;
  u64 execution_ns = 0;
  u64 max_queue_ns = 0;
  u64 max_execution_ns = 0;
  u32 queued = 0;
  u32 peak_queued = 0;
  u32 active = 0;
  u32 peak_active = 0;
};

struct workload_ticket {
  u64 queued_at_ns = 0;
};

/// Lock-free counters for a subsystem-defined class of work. A ticket is
/// created immediately before submission, started inside the task, and either
/// completed by measure_workload() or rejected when submission cannot allocate
/// a future.
class workload_tracker {
public:
  [[nodiscard]] workload_ticket submit() noexcept {
    m_submitted.fetch_add(1, std::memory_order_relaxed);
    const u32 queued = m_queued.fetch_add(1, std::memory_order_relaxed) + 1u;
    record_max(m_peak_queued, queued);
    return {.queued_at_ns = os::ticks_ns()};
  }

  void reject(workload_ticket) noexcept {
    m_queued.fetch_sub(1, std::memory_order_relaxed);
    m_rejected.fetch_add(1, std::memory_order_relaxed);
  }

  [[nodiscard]] u64 start(const workload_ticket ticket) noexcept {
    const u64 now = os::ticks_ns();
    const u64 queued_ns = now - ticket.queued_at_ns;
    m_queued.fetch_sub(1, std::memory_order_relaxed);
    m_started.fetch_add(1, std::memory_order_relaxed);
    m_queue_ns.fetch_add(queued_ns, std::memory_order_relaxed);
    record_max(m_max_queue_ns, queued_ns);
    const u32 active = m_active.fetch_add(1, std::memory_order_relaxed) + 1u;
    record_max(m_peak_active, active);
    return now;
  }

  void complete(const u64 started_at_ns, const bool cancelled = false,
                const bool failed = false) noexcept {
    const u64 elapsed = os::ticks_ns() - started_at_ns;
    m_active.fetch_sub(1, std::memory_order_relaxed);
    m_completed.fetch_add(1, std::memory_order_relaxed);
    m_execution_ns.fetch_add(elapsed, std::memory_order_relaxed);
    record_max(m_max_execution_ns, elapsed);
    if (cancelled)
      m_cancelled.fetch_add(1, std::memory_order_relaxed);
    else if (failed)
      m_failed.fetch_add(1, std::memory_order_relaxed);
  }

  [[nodiscard]] workload_statistics statistics() const noexcept {
    return {
        .submitted = m_submitted.load(std::memory_order_relaxed),
        .started = m_started.load(std::memory_order_relaxed),
        .completed = m_completed.load(std::memory_order_relaxed),
        .cancelled = m_cancelled.load(std::memory_order_relaxed),
        .failed = m_failed.load(std::memory_order_relaxed),
        .rejected = m_rejected.load(std::memory_order_relaxed),
        .queue_ns = m_queue_ns.load(std::memory_order_relaxed),
        .execution_ns = m_execution_ns.load(std::memory_order_relaxed),
        .max_queue_ns = m_max_queue_ns.load(std::memory_order_relaxed),
        .max_execution_ns = m_max_execution_ns.load(std::memory_order_relaxed),
        .queued = m_queued.load(std::memory_order_relaxed),
        .peak_queued = m_peak_queued.load(std::memory_order_relaxed),
        .active = m_active.load(std::memory_order_relaxed),
        .peak_active = m_peak_active.load(std::memory_order_relaxed),
    };
  }

private:
  template <typename T>
  static void record_max(std::atomic<T> &target, const T value) noexcept {
    T known = target.load(std::memory_order_relaxed);
    while (known < value && !target.compare_exchange_weak(
                                known, value, std::memory_order_relaxed,
                                std::memory_order_relaxed)) {
    }
  }

  std::atomic<u64> m_submitted{0};
  std::atomic<u64> m_started{0};
  std::atomic<u64> m_completed{0};
  std::atomic<u64> m_cancelled{0};
  std::atomic<u64> m_failed{0};
  std::atomic<u64> m_rejected{0};
  std::atomic<u64> m_queue_ns{0};
  std::atomic<u64> m_execution_ns{0};
  std::atomic<u64> m_max_queue_ns{0};
  std::atomic<u64> m_max_execution_ns{0};
  std::atomic<u32> m_queued{0};
  std::atomic<u32> m_peak_queued{0};
  std::atomic<u32> m_active{0};
  std::atomic<u32> m_peak_active{0};
};

/// Measures queue latency, execution time and terminal outcome without hiding
/// cancellation from the future/task-group machinery.
template <typename F>
auto measure_workload(workload_tracker &tracker, const workload_ticket ticket,
                      const cancellation_token &cancellation, F &&callable)
    -> std::invoke_result_t<F &> {
  const u64 started = tracker.start(ticket);
  try {
    cancellation.throw_if_requested();
    using R = std::invoke_result_t<F &>;
    if constexpr (std::is_void_v<R>) {
      std::invoke(callable);
      tracker.complete(started);
      return;
    } else {
      R result = std::invoke(callable);
      tracker.complete(started);
      return result;
    }
  } catch (const task_cancelled &) {
    tracker.complete(started, true, false);
    throw;
  } catch (...) {
    tracker.complete(started, false, true);
    throw;
  }
}

} // namespace nx
