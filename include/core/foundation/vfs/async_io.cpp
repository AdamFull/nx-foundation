#include "core/foundation/vfs/async_io.h"
#include "core/foundation/vfs/native_async_io.h"
#include "core/foundation/vfs/vfs_internal.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <optional>

namespace nx::vfs {
namespace {

constexpr usize READ_CHUNK = 256u * 1024u;

[[nodiscard]] io_result<nx::blob<u8>> read_exact(AsyncIoContext &context,
                                                 stream_ptr stream,
                                                 const u64 offset,
                                                 const u64 size) {
  if (!stream || size > std::numeric_limits<usize>::max() ||
      offset > nx::cast<u64>(std::numeric_limits<i64>::max()) ||
      !stream->seek(nx::cast<i64>(offset), nx::fs::seek_origin::begin))
    return err(file_error{io_error::InvalidArgument, 0});

  context.reserve(size);
  nx::blob<u8> bytes(nx::cast<usize>(size));
  usize read = 0;
  while (read < bytes.size()) {
    context.throw_if_cancelled();
    const usize requested = nx::min(READ_CHUNK, bytes.size() - read);
    const usize received = stream->read(bytes.data() + read, requested);
    if (received == 0)
      return err(file_error{io_error::ReadFailed, 0});
    read += received;
  }
  context.throw_if_cancelled();
  return bytes;
}

} // namespace

struct AsyncIoService::BudgetWaiter {
  fiber::counter pending{1};
  std::atomic<bool> woken{false};

  // Released bytes and cancellation may both wake the same waiter.
  void wake() noexcept {
    if (!woken.exchange(true, std::memory_order_acq_rel))
      pending.decrement();
  }
};

struct AsyncIoService::NativeRequest {
  NativeRequest(thread_pool *pool, nx::string native_path,
                const u64 read_offset, const u64 read_size,
                AsyncIoRequest io_request,
                const bool should_charge_budget) noexcept
      : completion(pool), path(std::move(native_path)), offset(read_offset),
        size(read_size), cancellation(std::move(io_request.cancellation)),
        priority(io_request.priority), charge_budget(should_charge_budget) {}

  completion_source<io_result<nx::blob<u8>>> completion;
  nx::string path;
  u64 offset = 0;
  u64 size = 0;
  cancellation_token cancellation;
  task_priority priority = task_priority::Normal;
  workload_ticket ticket;
  u64 started_at_ns = 0;
  u64 charged = 0;
  bool charge_budget = true;
};

void AsyncIoReservation::reset() noexcept {
  if (m_service != nullptr && m_bytes != 0)
    m_service->release_bytes(m_bytes);
  m_service = nullptr;
  m_bytes = 0;
}

AsyncIoContext::AsyncIoContext(AsyncIoService &service,
                               const task_priority priority,
                               cancellation_token cancellation) noexcept
    : m_service(service), m_priority(priority),
      m_cancellation(std::move(cancellation)) {}

AsyncIoContext::~AsyncIoContext() {
  if (m_charged != 0)
    m_service.release_bytes(m_charged);
}

void AsyncIoContext::throw_if_cancelled() const {
  m_cancellation.throw_if_requested();
  m_service.throw_if_stopping();
}

FileInfo AsyncIoContext::stat(const path_view path) const {
  throw_if_cancelled();
  const FileInfo info = vfs::stat(path);
  throw_if_cancelled();
  return info;
}

io_result<stream_ptr> AsyncIoContext::open(const path_view path) const {
  throw_if_cancelled();
  auto result = vfs::open(path);
  throw_if_cancelled();
  return result;
}

void AsyncIoContext::reserve(const u64 bytes) {
  throw_if_cancelled();
  if (bytes <= m_reservation)
    return;
  // Reacquire the larger reservation as one unit. Incremental acquisition can
  // deadlock when several operations hold partial budgets and all try to grow.
  if (m_charged != 0) {
    m_service.release_bytes(m_charged);
    m_charged = 0;
  }
  m_reservation = 0;
  m_charged = m_service.acquire_bytes(bytes, m_cancellation);
  m_reservation = bytes;
}

AsyncIoReservation AsyncIoContext::take_reservation() noexcept {
  AsyncIoReservation held(m_service, m_charged);
  m_charged = 0;
  m_reservation = 0;
  return held;
}

void AsyncIoContext::record_range_read(const u64 bytes) noexcept {
  m_service.m_range_reads.fetch_add(1, std::memory_order_relaxed);
  m_service.m_fallback_reads.fetch_add(1, std::memory_order_relaxed);
  m_service.m_bytes_read.fetch_add(bytes, std::memory_order_relaxed);
}

// A native read needs no thread while the kernel runs it, so the request gives
// its slot to the next one meanwhile. It takes a slot back before going on,
// outside any catch block: waiting inside one could resume this fiber on
// another thread in the middle of the exception.
io_result<nx::blob<u8>>
AsyncIoContext::wait_native(future<io_result<nx::blob<u8>>> &pending) {
  m_service.release_admission();
  std::optional<io_result<nx::blob<u8>>> result;
  std::exception_ptr error;
  try {
    result.emplace(pending.get());
  } catch (...) {
    error = std::current_exception();
  }
  m_service.reacquire_admission();
  if (error)
    std::rethrow_exception(error);
  return std::move(*result);
}

io_result<nx::blob<u8>> AsyncIoContext::read(const path_view path,
                                             const u64 max_bytes) {
  detail::NativeReadFile native;
  if (m_service.m_native && detail::resolve_native_read(path, native) &&
      native.info.size <= std::numeric_limits<usize>::max() &&
      (max_bytes == 0 || native.info.size <= max_bytes)) {
    reserve(native.info.size);
    auto pending = m_service.enqueue_native(
        std::move(native.path), 0, native.info.size,
        {.priority = m_priority, .cancellation = m_cancellation}, true, false,
        nullptr);
    if (!pending.valid())
      return err(file_error{io_error::ReadFailed, 0});
    return wait_native(pending);
  }

  m_service.m_whole_reads.fetch_add(1, std::memory_order_relaxed);
  m_service.m_fallback_reads.fetch_add(1, std::memory_order_relaxed);
  auto opened = open(path);
  if (!opened)
    return err(opened.error());
  stream_ptr stream = std::move(opened.value());
  const i64 length = stream->size();
  throw_if_cancelled();
  if (length >= 0) {
    const u64 size = nx::cast<u64>(length);
    if (size > std::numeric_limits<usize>::max() ||
        (max_bytes != 0 && size > max_bytes))
      return err(file_error{io_error::FileTooLarge, 0});
    auto result = read_exact(*this, std::move(stream), 0, size);
    if (result)
      m_service.m_bytes_read.fetch_add(size, std::memory_order_relaxed);
    return result;
  }

  // Unknown-length streams receive the entire service budget exclusively.
  // The max_bytes option remains the hard allocation bound when supplied.
  reserve(m_service.m_byte_limit);
  nx::vector<u8> bytes;
  nx::blob<u8> chunk(READ_CHUNK);
  for (;;) {
    throw_if_cancelled();
    const usize received = stream->read(chunk.data(), chunk.size());
    if (received == 0)
      break;
    if (max_bytes != 0 &&
        (received > max_bytes || bytes.size() > max_bytes - received))
      return err(file_error{io_error::FileTooLarge, 0});
    bytes.insert(bytes.end(), chunk.data(), chunk.data() + received);
  }
  throw_if_cancelled();
  m_service.m_bytes_read.fetch_add(bytes.size(), std::memory_order_relaxed);
  return nx::blob<u8>(bytes.begin(), bytes.end());
}

io_result<nx::blob<u8>> AsyncIoContext::read_range(const path_view path,
                                                   const u64 offset,
                                                   const u64 size) {
  detail::NativeReadFile native;
  if (m_service.m_native && detail::resolve_native_read(path, native) &&
      offset <= native.info.size && size <= native.info.size - offset &&
      size <= std::numeric_limits<usize>::max()) {
    reserve(size);
    auto pending = m_service.enqueue_native(
        std::move(native.path), offset, size,
        {.priority = m_priority, .cancellation = m_cancellation}, false, false,
        nullptr);
    if (!pending.valid())
      return err(file_error{io_error::ReadFailed, 0});
    return wait_native(pending);
  }

  m_service.m_range_reads.fetch_add(1, std::memory_order_relaxed);
  m_service.m_fallback_reads.fetch_add(1, std::memory_order_relaxed);
  auto opened = open(path);
  if (!opened)
    return err(opened.error());
  stream_ptr stream = std::move(opened.value());
  const i64 length = stream->size();
  throw_if_cancelled();
  if (length >= 0 &&
      (offset > nx::cast<u64>(length) || size > nx::cast<u64>(length) - offset))
    return err(file_error{io_error::InvalidArgument, 0});
  auto result = read_exact(*this, std::move(stream), offset, size);
  if (result)
    m_service.m_bytes_read.fetch_add(size, std::memory_order_relaxed);
  return result;
}

AsyncIoService::AsyncIoService(thread_pool &workers,
                               const AsyncIoConfig &config)
    : m_workers(&workers), m_byte_limit(config.byte_limit),
      m_native_limit(nx::max(1u, config.max_concurrent_requests)) {
  const u32 blocking = workers.blocking_thread_count();
  m_fallback_limit = nx::max(1u, config.max_concurrent_requests);
  if (blocking > 1)
    m_fallback_limit = nx::min(m_fallback_limit, blocking - 1);
  if (config.enable_native_backend)
    m_native = detail::make_native_async_io(config.thread_name);
}

AsyncIoService::~AsyncIoService() { shutdown(); }

future<io_result<nx::blob<u8>>> AsyncIoService::read(const path_view path,
                                                     AsyncIoRequest request,
                                                     const u64 max_bytes) {
  detail::NativeReadFile native;
  if (m_native && detail::resolve_native_read(path, native) &&
      native.info.size <= std::numeric_limits<usize>::max() &&
      (max_bytes == 0 || native.info.size <= max_bytes)) {
    return enqueue_native(std::move(native.path), 0, native.info.size,
                          std::move(request), true, true, m_workers);
  }
  return async(std::move(request), [path = nx::string(path.native()),
                                    max_bytes](AsyncIoContext &context) {
    return context.read(path.view(), max_bytes);
  });
}

future<io_result<nx::blob<u8>>>
AsyncIoService::read_range(const path_view path, const u64 offset,
                           const u64 size, AsyncIoRequest request) {
  detail::NativeReadFile native;
  if (m_native && detail::resolve_native_read(path, native) &&
      offset <= native.info.size && size <= native.info.size - offset &&
      size <= std::numeric_limits<usize>::max()) {
    return enqueue_native(std::move(native.path), offset, size,
                          std::move(request), false, true, m_workers);
  }
  request.reservation_bytes = size;
  return async(std::move(request), [path = nx::string(path.native()), offset,
                                    size](AsyncIoContext &context) {
    return context.read_range(path.view(), offset, size);
  });
}

future<io_result<nx::blob<u8>>>
AsyncIoService::enqueue_native(nx::string path, const u64 offset,
                               const u64 size, AsyncIoRequest request,
                               const bool whole_read, const bool charge_budget,
                               thread_pool *const continuation_pool) {
  if (!m_workers || !m_native || m_stopping.load(std::memory_order_acquire))
    return {};

  NativeRequest *pending =
      nx::allocate<NativeRequest>(continuation_pool, std::move(path), offset,
                                  size, std::move(request), charge_budget);
  if (pending == nullptr || !pending->completion.valid()) {
    if (pending != nullptr)
      nx::release(pending);
    return {};
  }
  future<io_result<nx::blob<u8>>> result = pending->completion.get_future();
  pending->ticket = m_workload.submit();
  if (whole_read)
    m_whole_reads.fetch_add(1, std::memory_order_relaxed);
  else
    m_range_reads.fetch_add(1, std::memory_order_relaxed);
  m_native_reads.fetch_add(1, std::memory_order_relaxed);

  {
    try {
      nx::scoped_lock lock(m_budget_lock);
      if (m_stopping.load(std::memory_order_relaxed)) {
        pending->started_at_ns = m_workload.start(pending->ticket);
        ++m_native_active;
        // Completion outside the lock keeps user continuations re-entrant.
      } else {
        m_native_pending[task_priority_index(pending->priority)].push_back(
            pending);
        pending = nullptr;
      }
    } catch (...) {
      m_workload.reject(pending->ticket);
      pending->completion.set_exception(std::current_exception());
      nx::release(pending);
      return result;
    }
  }
  if (pending != nullptr) {
    complete_native(pending, err(file_error{io_error::Interrupted, 0}), true);
  } else {
    pump_native();
  }
  return result;
}

AsyncIoService::NativeRequest *
AsyncIoService::take_ready_native_locked() noexcept {
  if (m_native_active >= m_native_limit)
    return nullptr;

  for (usize priority = task_priority_index(task_priority::Count);
       priority-- > 0;) {
    nx::vector<NativeRequest *> &queue = m_native_pending[priority];
    for (usize i = 0; i < queue.size(); ++i) {
      NativeRequest *request = queue[i];
      const u64 charge = !request->charge_budget || m_byte_limit == 0
                             ? 0
                             : nx::min(request->size, m_byte_limit);
      const bool cancelled = request->cancellation.stop_requested();
      const bool fits =
          charge == 0 ||
          charge <= m_byte_limit - nx::min(m_byte_limit, m_bytes_in_flight);
      if (!cancelled && !fits)
        continue;

      queue.erase(queue.begin() + nx::cast<isize>(i));
      request->charged = cancelled ? 0 : charge;
      m_bytes_in_flight += request->charged;
      ++m_native_active;
      update_visible_budget_locked();
      return request;
    }
  }
  return nullptr;
}

void AsyncIoService::pump_native() noexcept {
  for (;;) {
    NativeRequest *request = nullptr;
    {
      nx::scoped_lock lock(m_budget_lock);
      if (m_stopping.load(std::memory_order_relaxed))
        return;
      request = take_ready_native_locked();
      if (request == nullptr)
        return;
      request->started_at_ns = m_workload.start(request->ticket);
    }

    if (request->cancellation.stop_requested()) {
      complete_native(request, err(file_error{io_error::Interrupted, 0}), true);
      continue;
    }

    detail::NativeAsyncReadRequest native_request{
        .path = std::move(request->path),
        .offset = request->offset,
        .size = request->size,
        .cancellation = request->cancellation,
    };
    detail::NativeAsyncReadCompletion completion(
        [this, request](io_result<nx::blob<u8>> bytes,
                        const bool cancelled) mutable {
          complete_native(request, std::move(bytes), cancelled);
        });
    if (!m_native->submit(std::move(native_request), std::move(completion))) {
      complete_native(request, err(file_error{io_error::OpenFailed, 0}),
                      m_stopping.load(std::memory_order_acquire));
    }
  }
}

void AsyncIoService::complete_native(NativeRequest *request,
                                     io_result<nx::blob<u8>> result,
                                     bool cancelled) noexcept {
  cancelled = cancelled || request->cancellation.stop_requested() ||
              m_stopping.load(std::memory_order_acquire);
  if (!cancelled && result)
    m_bytes_read.fetch_add(result->size(), std::memory_order_relaxed);
  if (cancelled)
    m_native_cancellations.fetch_add(1, std::memory_order_relaxed);
  m_workload.complete(request->started_at_ns, cancelled, false);

  {
    nx::scoped_lock lock(m_budget_lock);
    NX_ASSERT(m_native_active != 0);
    --m_native_active;
    NX_ASSERT(request->charged <= m_bytes_in_flight);
    m_bytes_in_flight -= request->charged;
    update_visible_budget_locked();
    wake_budget_waiters_locked();
  }

  if (cancelled)
    request->completion.set_cancelled();
  else
    request->completion.set_value(std::move(result));
  nx::release(request);
  pump_native();
}

void AsyncIoService::enqueue_fallback(const task_priority priority,
                                      nx::detail::task_fn job) {
  m_outstanding.add(1);
  {
    nx::scoped_lock lock(m_budget_lock);
    m_fallback_pending[task_priority_index(priority)].push_back(std::move(job));
  }
  pump_fallback();
}

// Strict priority, except that background requests leave one slot free when
// there is more than one, so speculative streaming cannot hold them all.
bool AsyncIoService::take_fallback_locked(nx::detail::task_fn &job,
                                          task_priority &priority) noexcept {
  for (usize lane = task_priority_index(task_priority::Count); lane-- > 0;) {
    nx::vector<nx::detail::task_fn> &queue = m_fallback_pending[lane];
    if (queue.empty())
      continue;
    const bool background =
        lane == task_priority_index(task_priority::Background);
    const u32 limit = background && m_fallback_limit > 1 ? m_fallback_limit - 1
                                                         : m_fallback_limit;
    if (m_fallback_active >= limit)
      return false;
    job = std::move(queue.front());
    queue.erase(queue.begin());
    priority = static_cast<task_priority>(lane);
    ++m_fallback_active;
    return true;
  }
  return false;
}

void AsyncIoService::release_admission() noexcept {
  {
    nx::scoped_lock lock(m_budget_lock);
    --m_fallback_active;
  }
  pump_fallback();
}

void AsyncIoService::reacquire_admission() {
  fiber::counter admitted(1);
  {
    nx::scoped_lock lock(m_budget_lock);
    if (m_readmit.empty() && m_fallback_active < m_fallback_limit) {
      ++m_fallback_active;
      return;
    }
    m_readmit.push_back(&admitted);
  }
  fiber::wait(admitted);
}

void AsyncIoService::pump_fallback() noexcept {
  for (;;) {
    nx::detail::task_fn job;
    task_priority priority = task_priority::Normal;
    fiber::counter *readmitted = nullptr;
    {
      nx::scoped_lock lock(m_budget_lock);
      if (!m_readmit.empty()) {
        if (m_fallback_active >= m_fallback_limit)
          return;
        readmitted = m_readmit.front();
        m_readmit.erase(m_readmit.begin());
        ++m_fallback_active;
      } else if (!take_fallback_locked(job, priority)) {
        return;
      }
    }
    if (readmitted != nullptr) {
      readmitted->decrement();
      continue;
    }
    m_workers->submit(priority, [this, job = std::move(job)]() mutable {
      job();
      {
        nx::scoped_lock lock(m_budget_lock);
        --m_fallback_active;
      }
      pump_fallback();
      m_outstanding.decrement();
    });
  }
}

void AsyncIoService::update_visible_budget_locked() noexcept {
  m_visible_bytes_in_flight.store(m_bytes_in_flight, std::memory_order_relaxed);
  u64 peak = m_peak_bytes_in_flight.load(std::memory_order_relaxed);
  while (peak < m_bytes_in_flight &&
         !m_peak_bytes_in_flight.compare_exchange_weak(
             peak, m_bytes_in_flight, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

void AsyncIoService::wake_budget_waiters_locked() noexcept {
  for (BudgetWaiter *waiter : m_budget_waiters)
    waiter->wake();
  m_budget_waiters.clear();
}

// Only a request holding a slot waits here. It gives the slot up while it
// waits: the request holding the bytes it wants may need one to finish.
u64 AsyncIoService::acquire_bytes(const u64 bytes,
                                  const cancellation_token &cancellation) {
  if (bytes == 0 || m_byte_limit == 0)
    return 0;
  const u64 charged = nx::min(bytes, m_byte_limit);
  const auto charge_if_room = [&]() {
    if (charged > m_byte_limit - nx::min(m_byte_limit, m_bytes_in_flight))
      return false;
    m_bytes_in_flight += charged;
    update_visible_budget_locked();
    return true;
  };
  {
    nx::scoped_lock lock(m_budget_lock);
    if (charge_if_room())
      return charged;
  }

  release_admission();
  bool granted = false;
  for (;;) {
    BudgetWaiter waiter;
    {
      nx::scoped_lock lock(m_budget_lock);
      if (cancellation.stop_requested() ||
          m_stopping.load(std::memory_order_acquire))
        break;
      granted = charge_if_room();
      if (granted)
        break;
      m_budget_waiters.push_back(&waiter);
    }
    {
      const cancellation_callback on_cancel(
          cancellation,
          cancellation_callback::Fn([&waiter] { waiter.wake(); }));
      fiber::wait(waiter.pending);
    }
    nx::scoped_lock lock(m_budget_lock);
    const auto found =
        std::find(m_budget_waiters.begin(), m_budget_waiters.end(), &waiter);
    if (found != m_budget_waiters.end())
      m_budget_waiters.erase(found);
  }
  reacquire_admission();
  if (!granted) {
    cancellation.throw_if_requested();
    throw_if_stopping();
  }
  return charged;
}

void AsyncIoService::release_bytes(const u64 bytes) noexcept {
  if (bytes == 0)
    return;
  {
    nx::scoped_lock lock(m_budget_lock);
    NX_ASSERT(bytes <= m_bytes_in_flight);
    m_bytes_in_flight -= bytes;
    update_visible_budget_locked();
    wake_budget_waiters_locked();
  }
  if (!m_stopping.load(std::memory_order_acquire))
    pump_native();
}

void AsyncIoService::throw_if_stopping() const {
  if (m_stopping.load(std::memory_order_acquire))
    throw task_cancelled{};
}

AsyncIoStatistics AsyncIoService::statistics() const noexcept {
  return {
      .workload = m_workload.statistics(),
      .concurrency_limit = m_fallback_limit,
      .byte_limit = m_byte_limit,
      .bytes_in_flight =
          m_visible_bytes_in_flight.load(std::memory_order_relaxed),
      .peak_bytes_in_flight =
          m_peak_bytes_in_flight.load(std::memory_order_relaxed),
      .bytes_read = m_bytes_read.load(std::memory_order_relaxed),
      .whole_reads = m_whole_reads.load(std::memory_order_relaxed),
      .range_reads = m_range_reads.load(std::memory_order_relaxed),
      .native_reads = m_native_reads.load(std::memory_order_relaxed),
      .fallback_reads = m_fallback_reads.load(std::memory_order_relaxed),
      .native_cancellations =
          m_native_cancellations.load(std::memory_order_relaxed),
      .backend = m_native ? m_native->backend() : AsyncIoBackend::Blocking,
  };
}

void AsyncIoService::shutdown() noexcept {
  if (!m_workers)
    return;
  m_stopping.store(true, std::memory_order_release);
  {
    nx::scoped_lock lock(m_budget_lock);
    wake_budget_waiters_locked();
  }

  for (;;) {
    NativeRequest *pending = nullptr;
    {
      nx::scoped_lock lock(m_budget_lock);
      for (nx::vector<NativeRequest *> &queue : m_native_pending) {
        if (queue.empty())
          continue;
        pending = queue.back();
        queue.pop_back();
        pending->started_at_ns = m_workload.start(pending->ticket);
        ++m_native_active;
        break;
      }
    }
    if (pending == nullptr)
      break;
    complete_native(pending, err(file_error{io_error::Interrupted, 0}), true);
  }
  if (m_native)
    m_native->shutdown();
  m_native.reset();
  // Requests still queued run too, but see the service stopping and cancel.
  fiber::wait(m_outstanding);
  m_workers = nullptr;
}

} // namespace nx::vfs
