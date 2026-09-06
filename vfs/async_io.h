#pragma once

#include "core/foundation/threading/cancellation.h"
#include "core/foundation/threading/thread_pool.h"
#include "core/foundation/threading/workload.h"
#include "core/foundation/vfs/vfs.h"

#include <atomic>
#include <functional>
#include <type_traits>
#include <utility>

namespace nx::vfs {

struct AsyncIoConfig {
  /// Blocking requests which may execute concurrently. These workers are kept
  /// separate from CPU jobs so a slow device cannot occupy simulation workers.
  u32 max_concurrent_requests = 1;
  /// Aggregate payload reservation for executing requests. Zero disables the
  /// byte limit. A single oversized request is admitted exclusively.
  u64 byte_limit = 64u * 1024u * 1024u;
  const char *thread_name = "nx-io";
  bool collect_statistics = true;
};

struct AsyncIoRequest {
  task_priority priority = task_priority::Normal;
  cancellation_token cancellation;
  /// Expected peak payload retained by a custom operation. Built-in whole and
  /// range reads reserve their exact payload automatically.
  u64 reservation_bytes = 0;
};

struct AsyncIoStatistics {
  thread_pool_statistics threads;
  workload_statistics workload;
  u64 byte_limit = 0;
  u64 bytes_in_flight = 0;
  u64 peak_bytes_in_flight = 0;
  u64 bytes_read = 0;
  u64 whole_reads = 0;
  u64 range_reads = 0;
};

class AsyncIoService;

/// Per-request view of the VFS. It propagates cancellation around every
/// potentially blocking call and releases the operation's byte reservation
/// when the request completes.
class AsyncIoContext {
public:
  ~AsyncIoContext();

  AsyncIoContext(const AsyncIoContext &) = delete;
  AsyncIoContext &operator=(const AsyncIoContext &) = delete;

  [[nodiscard]] FileInfo stat(path_view path) const;
  [[nodiscard]] io_result<stream_ptr> open(path_view path) const;
  [[nodiscard]] io_result<nx::blob<u8>> read(path_view path, u64 max_bytes = 0);
  [[nodiscard]] io_result<nx::blob<u8>> read_range(path_view path, u64 offset,
                                                   u64 size);

  /// Ensures this request reserves at least @p bytes until completion. Call
  /// before allocating or reading a custom payload. Waiting is cooperative and
  /// cancellation-aware; reservations never shrink during an operation.
  void reserve(u64 bytes);
  /// Accounts one range read performed by a stateful custom operation on a
  /// stream obtained through open(). Built-in read helpers account themselves.
  void record_range_read(u64 bytes) noexcept;
  void throw_if_cancelled() const;

  [[nodiscard]] const cancellation_token &cancellation() const noexcept {
    return m_cancellation;
  }

private:
  friend class AsyncIoService;
  AsyncIoContext(AsyncIoService &service,
                 cancellation_token cancellation) noexcept;

  AsyncIoService &m_service;
  cancellation_token m_cancellation;
  u64 m_reservation = 0;
  u64 m_charged = 0;
};

/// Bounded executor for blocking VFS operations. Callers may use the built-in
/// reads or invoke a stateful operation (for example a progressive archive
/// stream) without depending on the underlying thread-pool implementation.
class AsyncIoService {
public:
  explicit AsyncIoService(const AsyncIoConfig &config = {});
  ~AsyncIoService();

  AsyncIoService(const AsyncIoService &) = delete;
  AsyncIoService &operator=(const AsyncIoService &) = delete;

  [[nodiscard]] bool valid() const noexcept {
    return m_pool != nullptr && m_pool->worker_count() != 0;
  }
  [[nodiscard]] u32 concurrency_limit() const noexcept {
    return m_pool ? m_pool->worker_count() : 0;
  }

  template <typename F, typename R = std::invoke_result_t<std::decay_t<F> &,
                                                          AsyncIoContext &>>
  [[nodiscard]] future<R> async(AsyncIoRequest request, F &&operation) {
    if (!m_pool || m_stopping.load(std::memory_order_acquire))
      return {};

    const workload_ticket ticket = m_workload.submit();
    const cancellation_token cancellation = request.cancellation;
    const u64 reservation = request.reservation_bytes;
    future<R> result = m_pool->async(
        request.priority,
        [this, cancellation, reservation, ticket,
         operation =
             std::decay_t<F>(std::forward<F>(operation))]() mutable -> R {
          return measure_workload(m_workload, ticket, cancellation, [&]() -> R {
            AsyncIoContext context(*this, cancellation);
            if (reservation != 0)
              context.reserve(reservation);
            context.throw_if_cancelled();
            if constexpr (std::is_void_v<R>) {
              std::invoke(operation, context);
              context.throw_if_cancelled();
              return;
            } else {
              R result = std::invoke(operation, context);
              context.throw_if_cancelled();
              return result;
            }
          });
        });
    if (!result.valid())
      m_workload.reject(ticket);
    return result;
  }

  [[nodiscard]] future<io_result<nx::blob<u8>>>
  read(path_view path, AsyncIoRequest request = {}, u64 max_bytes = 0);
  [[nodiscard]] future<io_result<nx::blob<u8>>>
  read_range(path_view path, u64 offset, u64 size, AsyncIoRequest request = {});

  [[nodiscard]] AsyncIoStatistics statistics() const noexcept;

  /// Cancels service-owned waits and drains all submitted operations. Clients
  /// must still request cancellation for operations blocked inside a device.
  /// No request submission may race with shutdown or destruction.
  void shutdown() noexcept;

private:
  friend class AsyncIoContext;

  [[nodiscard]] u64 acquire_bytes(u64 bytes,
                                  const cancellation_token &cancellation);
  void release_bytes(u64 bytes) noexcept;
  void throw_if_stopping() const;

  nx::unique_ptr<thread_pool> m_pool;
  workload_tracker m_workload;
  mutable nx::mutex m_budget_lock;
  nx::cond_var m_budget_changed;
  std::atomic<bool> m_stopping{false};
  const u64 m_byte_limit = 0;
  u64 m_bytes_in_flight = 0;
  std::atomic<u64> m_visible_bytes_in_flight{0};
  std::atomic<u64> m_peak_bytes_in_flight{0};
  std::atomic<u64> m_bytes_read{0};
  std::atomic<u64> m_whole_reads{0};
  std::atomic<u64> m_range_reads{0};
};

} // namespace nx::vfs
