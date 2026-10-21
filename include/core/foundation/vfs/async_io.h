#pragma once

#include "core/foundation/containers/string_map.h"
#include "core/foundation/fibers/scheduler.h"
#include "core/foundation/threading/cancellation.h"
#include "core/foundation/threading/thread_pool.h"
#include "core/foundation/threading/workload.h"
#include "core/foundation/vfs/vfs.h"

#include <atomic>
#include <functional>
#include <type_traits>
#include <utility>

namespace nx::vfs {

namespace detail {
class NativeAsyncIo;
}

enum class AsyncIoBackend : u8 {
  Blocking,
  Iocp,
  IoUring,
  DispatchIo,
};

[[nodiscard]] constexpr nx::string_view
to_string(const AsyncIoBackend backend) noexcept {
  switch (backend) {
  case AsyncIoBackend::Iocp:
    return "iocp";
  case AsyncIoBackend::IoUring:
    return "io_uring";
  case AsyncIoBackend::DispatchIo:
    return "dispatch_io";
  default:
    return "blocking";
  }
}

struct AsyncIoConfig {
  /// Requests which may block at once. Fallback requests block on the
  /// workers' blocking-service threads, so this is also kept below their
  /// number: one always stays free for other blocking calls, such as present.
  u32 max_concurrent_requests = 1;
  /// Aggregate payload reservation for executing requests. Zero disables the
  /// byte limit. A single oversized request is admitted exclusively.
  u64 byte_limit = 64u * 1024u * 1024u;
  /// Names the native backend's completion thread.
  const char *thread_name = "nx-io";
  /// Prefer the platform kernel backend for host-backed files. Archive,
  /// memory, and custom devices run on blocking-service threads instead.
  bool enable_native_backend = true;
};

struct AsyncIoRequest {
  task_priority priority = task_priority::Normal;
  cancellation_token cancellation;
  /// Expected peak payload retained by a custom operation. Built-in whole and
  /// range reads reserve their exact payload automatically.
  u64 reservation_bytes = 0;
};

struct AsyncIoStatistics {
  workload_statistics workload;
  u32 concurrency_limit = 0;
  u64 byte_limit = 0;
  u64 bytes_in_flight = 0;
  u64 peak_bytes_in_flight = 0;
  u64 bytes_read = 0;
  u64 whole_reads = 0;
  u64 range_reads = 0;
  u64 native_reads = 0;
  u64 fallback_reads = 0;
  u64 native_cancellations = 0;
  u64 writes = 0;
  u64 bytes_written = 0;
  u64 removals = 0;
  AsyncIoBackend backend = AsyncIoBackend::Blocking;
};

class AsyncIoService;

/// Movable ownership of bytes charged to an AsyncIoService. Asset pipelines
/// transfer a request's reservation into this lease while its payload waits for
/// CPU preparation, preventing completed reads from escaping the shared budget.
class AsyncIoReservation {
public:
  AsyncIoReservation() = default;
  ~AsyncIoReservation() { reset(); }

  AsyncIoReservation(const AsyncIoReservation &) = delete;
  AsyncIoReservation &operator=(const AsyncIoReservation &) = delete;

  AsyncIoReservation(AsyncIoReservation &&other) noexcept
      : m_service(other.m_service), m_bytes(other.m_bytes) {
    other.m_service = nullptr;
    other.m_bytes = 0;
  }

  AsyncIoReservation &operator=(AsyncIoReservation &&other) noexcept {
    if (this != &other) {
      reset();
      m_service = other.m_service;
      m_bytes = other.m_bytes;
      other.m_service = nullptr;
      other.m_bytes = 0;
    }
    return *this;
  }

  [[nodiscard]] u64 bytes() const noexcept { return m_bytes; }
  [[nodiscard]] explicit operator bool() const noexcept { return m_bytes != 0; }
  void reset() noexcept;

private:
  friend class AsyncIoContext;
  AsyncIoReservation(AsyncIoService &service, u64 bytes) noexcept
      : m_service(&service), m_bytes(bytes) {}

  AsyncIoService *m_service = nullptr;
  u64 m_bytes = 0;
};

/// Per-request view of the VFS used by stateful fallback operations. Host-file
/// read helpers still route through the native backend while preserving this
/// context's retained byte reservation; provider-specific open/stat work stays
/// on the bounded I/O worker. Cancellation covers both paths.
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
  /// Transfers the current byte charge to the caller. The returned lease must
  /// not outlive the service. Further context reads start a new reservation.
  [[nodiscard]] AsyncIoReservation take_reservation() noexcept;
  /// Accounts one range read performed by a stateful custom operation on a
  /// stream obtained through open(). Built-in read helpers account themselves.
  void record_range_read(u64 bytes) noexcept;
  void throw_if_cancelled() const;

  [[nodiscard]] const cancellation_token &cancellation() const noexcept {
    return m_cancellation;
  }

private:
  friend class AsyncIoService;
  AsyncIoContext(AsyncIoService &service, task_priority priority,
                 cancellation_token cancellation) noexcept;

  [[nodiscard]] io_result<nx::blob<u8>>
  wait_native(future<io_result<nx::blob<u8>>> &pending);

  AsyncIoService &m_service;
  task_priority m_priority = task_priority::Normal;
  cancellation_token m_cancellation;
  u64 m_reservation = 0;
  u64 m_charged = 0;
};

/// Asynchronous VFS service. Host-backed whole/range reads use the platform
/// kernel backend; devices which need user-space work (archives, memory,
/// remote providers) and custom stateful operations run as jobs on @p workers
/// that move to its blocking-service threads, admitted in priority order.
class AsyncIoService {
public:
  explicit AsyncIoService(thread_pool &workers,
                          const AsyncIoConfig &config = {});
  ~AsyncIoService();

  AsyncIoService(const AsyncIoService &) = delete;
  AsyncIoService &operator=(const AsyncIoService &) = delete;

  [[nodiscard]] bool valid() const noexcept { return m_workers != nullptr; }
  [[nodiscard]] u32 concurrency_limit() const noexcept {
    return m_fallback_limit;
  }

  template <typename F, typename R = std::invoke_result_t<std::decay_t<F> &,
                                                          AsyncIoContext &>>
  [[nodiscard]] future<R> async(AsyncIoRequest request, F &&operation) {
    if (m_workers == nullptr || m_stopping.load(std::memory_order_acquire))
      return {};
    completion_source<R> source(m_workers);
    future<R> result = source.get_future();
    if (!result.valid())
      return {};

    const workload_ticket ticket = m_workload.submit();
    const task_priority priority = request.priority;
    nx::detail::task_fn job{
        [this, source = std::move(source), ticket, priority,
         cancellation = std::move(request.cancellation),
         reservation = request.reservation_bytes,
         operation = std::decay_t<F>(std::forward<F>(operation))]() mutable {
          const auto run = [&]() -> R {
            AsyncIoContext context(*this, priority, cancellation);
            if (reservation != 0)
              context.reserve(reservation);
            context.throw_if_cancelled();
            if constexpr (std::is_void_v<R>) {
              std::invoke(operation, context);
              context.throw_if_cancelled();
              return;
            } else {
              R value = std::invoke(operation, context);
              context.throw_if_cancelled();
              return value;
            }
          };
          // Nothing may leave run_blocking by exception, so the outcome is
          // settled on the blocking-service thread.
          fiber::run_blocking([&] {
            try {
              if constexpr (std::is_void_v<R>) {
                measure_workload(m_workload, ticket, cancellation, run);
                source.set_value();
              } else {
                source.set_value(
                    measure_workload(m_workload, ticket, cancellation, run));
              }
            } catch (const task_cancelled &) {
              source.set_cancelled();
            } catch (...) {
              source.set_exception(std::current_exception());
            }
          });
        }};
    if (!job) {
      m_workload.reject(ticket);
      return {};
    }
    enqueue_fallback(priority, std::move(job));
    return result;
  }

  [[nodiscard]] future<io_result<nx::blob<u8>>>
  read(path_view path, AsyncIoRequest request = {}, u64 max_bytes = 0);
  [[nodiscard]] future<io_result<nx::blob<u8>>>
  read_range(path_view path, u64 offset, u64 size, AsyncIoRequest request = {});

  /// Writes @p bytes to @p path as vfs::write does: through the first
  /// writable mount, which for a host one replaces the file atomically and
  /// on storage. Writes and removals of one path run one at a time, in the
  /// order they were asked for. Cancellation stops one that has not
  /// started; one that has finishes, and shutdown runs every one asked for
  /// before it.
  [[nodiscard]] future<io_result<void>>
  write(path_view path, nx::blob<u8> bytes, AsyncIoRequest request = {});
  /// Removes @p path as vfs::remove does, in order with its writes.
  [[nodiscard]] future<io_result<void>> remove(path_view path,
                                               AsyncIoRequest request = {});
  /// The entries of directory @p path, as vfs::list gives them.
  [[nodiscard]] future<io_result<nx::vector<DirEntry>>>
  list(path_view path, AsyncIoRequest request = {});

  [[nodiscard]] AsyncIoStatistics statistics() const noexcept;

  /// Cancels native requests and service-owned waits, then drains all submitted
  /// operations. Custom fallback operations remain cooperatively cancellable.
  /// No request submission may race with shutdown or destruction.
  void shutdown() noexcept;

private:
  friend class AsyncIoContext;
  friend class AsyncIoReservation;

  struct NativeRequest;
  struct BudgetWaiter;
  struct PendingChange {
    task_priority priority = task_priority::Normal;
    nx::detail::task_fn job;
  };
  using ChangeFn = nx::unique_function<io_result<void>(path_view)>;

  [[nodiscard]] future<io_result<void>>
  submit_change(path_view path, AsyncIoRequest request, ChangeFn change);
  void finish_change(nx::string_view path) noexcept;

  [[nodiscard]] u64 acquire_bytes(u64 bytes,
                                  const cancellation_token &cancellation);
  void release_bytes(u64 bytes) noexcept;
  void throw_if_stopping() const;
  void enqueue_fallback(task_priority priority, nx::detail::task_fn job);
  void release_admission() noexcept;
  void reacquire_admission();
  void pump_fallback() noexcept;
  [[nodiscard]] bool take_fallback_locked(nx::detail::task_fn &job,
                                          task_priority &priority) noexcept;
  [[nodiscard]] future<io_result<nx::blob<u8>>>
  enqueue_native(nx::string path, u64 offset, u64 size, AsyncIoRequest request,
                 bool whole_read, bool charge_budget,
                 thread_pool *continuation_pool);
  void pump_native() noexcept;
  void complete_native(NativeRequest *request, io_result<nx::blob<u8>> result,
                       bool cancelled) noexcept;
  [[nodiscard]] NativeRequest *take_ready_native_locked() noexcept;
  void update_visible_budget_locked() noexcept;
  void wake_budget_waiters_locked() noexcept;

  thread_pool *m_workers = nullptr;
  nx::unique_ptr<detail::NativeAsyncIo> m_native;
  workload_tracker m_workload{"io::request"};
  mutable nx::mutex m_budget_lock;
  /// Requests waiting for bytes; woken whenever bytes are released.
  nx::vector<BudgetWaiter *> m_budget_waiters;
  std::atomic<bool> m_stopping{false};
  const u64 m_byte_limit = 0;
  u64 m_bytes_in_flight = 0;
  std::atomic<u64> m_visible_bytes_in_flight{0};
  std::atomic<u64> m_peak_bytes_in_flight{0};
  std::atomic<u64> m_bytes_read{0};
  std::atomic<u64> m_whole_reads{0};
  std::atomic<u64> m_range_reads{0};
  std::atomic<u64> m_native_reads{0};
  std::atomic<u64> m_fallback_reads{0};
  std::atomic<u64> m_native_cancellations{0};
  nx::vector<NativeRequest *>
      m_native_pending[static_cast<usize>(task_priority::Count)];
  u32 m_native_active = 0;
  const u32 m_native_limit = 1;
  nx::vector<nx::detail::task_fn>
      m_fallback_pending[static_cast<usize>(task_priority::Count)];
  u32 m_fallback_active = 0;
  u32 m_fallback_limit = 1;
  /// Requests whose native read has finished, waiting to take a slot back.
  /// They go before any request not yet started.
  nx::vector<fiber::counter *> m_readmit;
  /// Fallback requests submitted and not yet finished; shutdown waits on it.
  fiber::counter m_outstanding;
  /// For each path a write or removal is running or queued for, under
  /// m_budget_lock, those asked for after it.
  nx::string_map<nx::vector<PendingChange>> m_changes;
  std::atomic<u64> m_writes{0};
  std::atomic<u64> m_bytes_written{0};
  std::atomic<u64> m_removals{0};
};

} // namespace nx::vfs
