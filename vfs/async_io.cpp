#include "core/foundation/vfs/async_io.h"

#include <chrono>
#include <limits>

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

void AsyncIoReservation::reset() noexcept {
  if (m_service != nullptr && m_bytes != 0)
    m_service->release_bytes(m_bytes);
  m_service = nullptr;
  m_bytes = 0;
}

AsyncIoContext::AsyncIoContext(AsyncIoService &service,
                               cancellation_token cancellation) noexcept
    : m_service(service), m_cancellation(std::move(cancellation)) {}

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
  m_service.m_bytes_read.fetch_add(bytes, std::memory_order_relaxed);
}

io_result<nx::blob<u8>> AsyncIoContext::read(const path_view path,
                                             const u64 max_bytes) {
  m_service.m_whole_reads.fetch_add(1, std::memory_order_relaxed);
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
  m_service.m_range_reads.fetch_add(1, std::memory_order_relaxed);
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

AsyncIoService::AsyncIoService(const AsyncIoConfig &config)
    : m_byte_limit(config.byte_limit) {
  const u32 concurrency = nx::max(1u, config.max_concurrent_requests);
  m_pool = nx::make_unique<thread_pool>(thread_pool::config{
      .thread_count = concurrency,
      .name = config.thread_name != nullptr ? config.thread_name : "nx-io",
      // Keep one lane available to visible/critical work when possible. This
      // prevents speculative streaming from occupying every blocking worker.
      .max_background_concurrency = 0,
      .collect_statistics = config.collect_statistics,
  });
}

AsyncIoService::~AsyncIoService() { shutdown(); }

future<io_result<nx::blob<u8>>> AsyncIoService::read(const path_view path,
                                                     AsyncIoRequest request,
                                                     const u64 max_bytes) {
  return async(std::move(request), [path = nx::string(path.native()),
                                    max_bytes](AsyncIoContext &context) {
    return context.read(path.view(), max_bytes);
  });
}

future<io_result<nx::blob<u8>>>
AsyncIoService::read_range(const path_view path, const u64 offset,
                           const u64 size, AsyncIoRequest request) {
  request.reservation_bytes = size;
  return async(std::move(request), [path = nx::string(path.native()), offset,
                                    size](AsyncIoContext &context) {
    return context.read_range(path.view(), offset, size);
  });
}

u64 AsyncIoService::acquire_bytes(const u64 bytes,
                                  const cancellation_token &cancellation) {
  if (bytes == 0 || m_byte_limit == 0)
    return 0;
  const u64 charged = nx::min(bytes, m_byte_limit);
  nx::scoped_lock lock(m_budget_lock);
  while (charged > m_byte_limit - nx::min(m_byte_limit, m_bytes_in_flight)) {
    cancellation.throw_if_requested();
    throw_if_stopping();
    (void)m_budget_changed.wait_for(m_budget_lock,
                                    std::chrono::milliseconds(2));
  }
  m_bytes_in_flight += charged;
  m_visible_bytes_in_flight.store(m_bytes_in_flight, std::memory_order_relaxed);
  u64 peak = m_peak_bytes_in_flight.load(std::memory_order_relaxed);
  while (peak < m_bytes_in_flight &&
         !m_peak_bytes_in_flight.compare_exchange_weak(
             peak, m_bytes_in_flight, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
  return charged;
}

void AsyncIoService::release_bytes(const u64 bytes) noexcept {
  if (bytes == 0)
    return;
  {
    nx::scoped_lock lock(m_budget_lock);
    assert(bytes <= m_bytes_in_flight);
    m_bytes_in_flight -= bytes;
    m_visible_bytes_in_flight.store(m_bytes_in_flight,
                                    std::memory_order_relaxed);
  }
  m_budget_changed.notify_all();
}

void AsyncIoService::throw_if_stopping() const {
  if (m_stopping.load(std::memory_order_acquire))
    throw task_cancelled{};
}

AsyncIoStatistics AsyncIoService::statistics() const noexcept {
  return {
      .threads = m_pool ? m_pool->statistics() : thread_pool_statistics{},
      .workload = m_workload.statistics(),
      .byte_limit = m_byte_limit,
      .bytes_in_flight =
          m_visible_bytes_in_flight.load(std::memory_order_relaxed),
      .peak_bytes_in_flight =
          m_peak_bytes_in_flight.load(std::memory_order_relaxed),
      .bytes_read = m_bytes_read.load(std::memory_order_relaxed),
      .whole_reads = m_whole_reads.load(std::memory_order_relaxed),
      .range_reads = m_range_reads.load(std::memory_order_relaxed),
  };
}

void AsyncIoService::shutdown() noexcept {
  if (!m_pool)
    return;
  m_stopping.store(true, std::memory_order_release);
  m_budget_changed.notify_all();
  m_pool->wait_idle();
  m_pool.reset();
}

} // namespace nx::vfs
