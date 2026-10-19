#include "core/foundation/vfs/native_async_io.h"

#if defined(NX_PLATFORM_APPLE)

#include <dispatch/dispatch.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>

namespace nx::vfs::detail {
namespace {

[[nodiscard]] file_error map_error(const i32 error,
                                   const io_error fallback) noexcept {
  io_error kind = fallback;
  switch (error) {
  case ENOENT:
    kind = io_error::NotFound;
    break;
  case EACCES:
  case EPERM:
    kind = io_error::PermissionDenied;
    break;
  case EISDIR:
    kind = io_error::IsADirectory;
    break;
  case ENOTDIR:
    kind = io_error::NotADirectory;
    break;
  case EINVAL:
  case ENAMETOOLONG:
    kind = io_error::InvalidArgument;
    break;
  case ECANCELED:
  case EINTR:
    kind = io_error::Interrupted;
    break;
  default:
    break;
  }
  return {kind, error};
}

class DispatchAsyncIo final : public NativeAsyncIo {
  struct Operation {
    Operation(NativeAsyncReadRequest io_request,
              NativeAsyncReadCompletion io_completion) noexcept
        : request(std::move(io_request)),
          completion(std::move(io_completion)) {}

    NativeAsyncReadRequest request;
    NativeAsyncReadCompletion completion;
    nx::blob<u8> bytes;
    dispatch_io_t channel = nullptr;
    u64 completed = 0;
    i32 copy_error = 0;
    bool cancellation_sent = false;
  };

public:
  ~DispatchAsyncIo() override { shutdown(); }

  [[nodiscard]] bool start(const char *thread_name) noexcept {
    m_queue = dispatch_queue_create(
        thread_name != nullptr ? thread_name : "nx.dispatch-io",
        DISPATCH_QUEUE_SERIAL);
    m_group = dispatch_group_create();
    if (m_queue == nullptr || m_group == nullptr)
      return false;

    m_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, m_queue);
    if (m_timer == nullptr)
      return false;
    dispatch_source_set_timer(m_timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                              2u * NSEC_PER_MSEC, 1u * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(m_timer, ^{
      cancel_requested();
    });
    dispatch_resume(m_timer);
    return true;
  }

  [[nodiscard]] bool
  submit(NativeAsyncReadRequest request,
         NativeAsyncReadCompletion completion) noexcept override {
    if (!completion || m_stopping.load(std::memory_order_acquire))
      return false;
    Operation *operation = nx::allocate<Operation>(
        std::move(request), std::move(completion));
    if (operation == nullptr)
      return false;
    dispatch_group_enter(m_group);
    dispatch_async(m_queue, ^{
      begin(operation);
    });
    return true;
  }

  [[nodiscard]] AsyncIoBackend backend() const noexcept override {
    return AsyncIoBackend::DispatchIo;
  }

  void shutdown() noexcept override {
    if (m_queue == nullptr)
      return;
    m_stopping.store(true, std::memory_order_release);
    dispatch_sync(m_queue, ^{
      cancel_requested();
    });
    if (m_group != nullptr)
      dispatch_group_wait(m_group, DISPATCH_TIME_FOREVER);
    if (m_timer != nullptr)
      dispatch_source_cancel(m_timer);
    dispatch_sync(m_queue, ^{});
    m_timer = nullptr;
    m_group = nullptr;
    m_queue = nullptr;
  }

private:
  void begin(Operation *operation) noexcept {
    if (m_stopping.load(std::memory_order_acquire) ||
        operation->request.cancellation.stop_requested()) {
      operation->completion(
          err(file_error{io_error::Interrupted, ECANCELED}), true);
      dispatch_group_leave(m_group);
      nx::release(operation);
      return;
    }
    try {
      operation->bytes =
          nx::blob<u8>(nx::cast<usize>(operation->request.size));
    } catch (...) {
      operation->completion(
          err(file_error{io_error::ReadFailed, ENOMEM}), false);
      dispatch_group_leave(m_group);
      nx::release(operation);
      return;
    }
    try {
      m_active.push_back(operation);
    } catch (...) {
      operation->completion(err(file_error{io_error::ReadFailed, ENOMEM}),
                            false);
      dispatch_group_leave(m_group);
      nx::release(operation);
      return;
    }
    if (operation->request.size == 0) {
      finish(operation, nx::blob<u8>{}, false);
      return;
    }

    operation->channel = dispatch_io_create_with_path(
        DISPATCH_IO_RANDOM, operation->request.path.c_str(), O_RDONLY, 0,
        m_queue, ^(int) {});
    if (operation->channel == nullptr) {
      finish(operation, err(file_error{io_error::OpenFailed, errno}), false);
      return;
    }
    dispatch_io_set_low_water(operation->channel, 1);
    dispatch_io_set_high_water(operation->channel, 4u * 1024u * 1024u);
    dispatch_io_read(
        operation->channel, operation->request.offset,
        nx::cast<size_t>(operation->request.size), m_queue,
        ^(bool done, dispatch_data_t data, int error) {
          if (data != nullptr) {
            dispatch_data_apply(
                data, ^bool(dispatch_data_t, size_t, const void *buffer,
                            size_t size) {
                  if (size > operation->bytes.size() - operation->completed) {
                    operation->copy_error = EIO;
                    return false;
                  }
                  std::memcpy(operation->bytes.data() + operation->completed,
                              buffer, size);
                  operation->completed += size;
                  return true;
                });
          }
          if (!done)
            return;
          if (error == 0)
            error = operation->copy_error;
          const bool cancelled =
              error == ECANCELED &&
              (operation->cancellation_sent ||
               operation->request.cancellation.stop_requested() ||
               m_stopping.load(std::memory_order_acquire));
          if (error != 0) {
            finish(operation, err(map_error(error, io_error::ReadFailed)),
                   cancelled);
          } else if (operation->completed != operation->request.size) {
            finish(operation, err(file_error{io_error::ReadFailed, 0}), false);
          } else {
            finish(operation, std::move(operation->bytes), false);
          }
        });
  }

  void cancel_requested() noexcept {
    const bool stopping = m_stopping.load(std::memory_order_acquire);
    for (Operation *operation : m_active) {
      if (!operation->cancellation_sent &&
          (stopping || operation->request.cancellation.stop_requested())) {
        operation->cancellation_sent = true;
        if (operation->channel != nullptr)
          dispatch_io_close(operation->channel, DISPATCH_IO_STOP);
      }
    }
  }

  void finish(Operation *operation, io_result<nx::blob<u8>> result,
              const bool cancelled) noexcept {
    const auto found = std::find(m_active.begin(), m_active.end(), operation);
    if (found != m_active.end()) {
      *found = m_active.back();
      m_active.pop_back();
    }
    if (operation->channel != nullptr)
      dispatch_io_close(operation->channel, 0);
    operation->channel = nullptr;
    operation->completion(std::move(result), cancelled);
    dispatch_group_leave(m_group);
    nx::release(operation);
  }

  dispatch_queue_t m_queue = nullptr;
  dispatch_group_t m_group = nullptr;
  dispatch_source_t m_timer = nullptr;
  nx::vector<Operation *> m_active;
  std::atomic<bool> m_stopping{false};
};

} // namespace

nx::unique_ptr<NativeAsyncIo> make_native_async_io(const char *thread_name) {
  DispatchAsyncIo *backend = nx::allocate<DispatchAsyncIo>();
  if (backend == nullptr)
    return {};
  if (!backend->start(thread_name)) {
    nx::release(backend);
    return {};
  }
  return nx::adopt_unique<NativeAsyncIo>(backend);
}

} // namespace nx::vfs::detail

#endif
