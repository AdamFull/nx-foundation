#include "core/foundation/vfs/native_async_io.h"

#if defined(NX_PLATFORM_WINDOWS)

#include "core/foundation/platform/windows_path.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <limits>

namespace nx::vfs::detail {
namespace {

constexpr DWORD COMPLETION_POLL_MS = 2;
constexpr usize READ_CHUNK = 4u * 1024u * 1024u;
constexpr ULONG_PTR WAKE_KEY = 1;

[[nodiscard]] file_error map_error(const DWORD error,
                                   const io_error fallback) noexcept {
  io_error kind = fallback;
  switch (error) {
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
  case ERROR_INVALID_DRIVE:
    kind = io_error::NotFound;
    break;
  case ERROR_ACCESS_DENIED:
  case ERROR_SHARING_VIOLATION:
    kind = io_error::PermissionDenied;
    break;
  case ERROR_INVALID_NAME:
  case ERROR_BAD_PATHNAME:
    kind = io_error::InvalidArgument;
    break;
  case ERROR_HANDLE_EOF:
    kind = io_error::ReadFailed;
    break;
  default:
    break;
  }
  return {kind, nx::cast<i32>(error)};
}

[[nodiscard]] nx::vector<wchar_t> widen(const nx::string_view path) {
  return nx::fs::detail::win32_path(path);
}

class IocpAsyncIo final : public NativeAsyncIo {
  struct Operation;
  struct IoOverlapped final : OVERLAPPED {
    Operation *owner = nullptr;
  };

  struct Operation {
    Operation(NativeAsyncReadRequest io_request,
              NativeAsyncReadCompletion io_completion) noexcept
        : request(std::move(io_request)), completion(std::move(io_completion)) {
    }

    NativeAsyncReadRequest request;
    NativeAsyncReadCompletion completion;
    nx::blob<u8> bytes;
    HANDLE file = INVALID_HANDLE_VALUE;
    IoOverlapped io{};
    u64 completed = 0;
    bool cancellation_sent = false;
  };

public:
  IocpAsyncIo() = default;
  ~IocpAsyncIo() override { shutdown(); }

  [[nodiscard]] bool start(const char *thread_name) {
    m_port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (m_port == nullptr)
      return false;
    m_thread =
        nx::thread([this] { run(); },
                   {.name = thread_name != nullptr ? thread_name : "nx-iocp"});
    return m_thread.joinable();
  }

  [[nodiscard]] bool
  submit(NativeAsyncReadRequest request,
         NativeAsyncReadCompletion completion) noexcept override {
    if (!completion || m_stopping.load(std::memory_order_acquire))
      return false;
    Operation *operation =
        nx::allocate<Operation>(std::move(request), std::move(completion));
    if (operation == nullptr)
      return false;
    operation->io.owner = operation;
    {
      nx::scoped_lock lock(m_pending_lock);
      if (m_stopping.load(std::memory_order_relaxed)) {
        nx::release(operation);
        return false;
      }
      try {
        m_pending.push_back(operation);
      } catch (...) {
        nx::release(operation);
        return false;
      }
    }
    (void)::PostQueuedCompletionStatus(m_port, 0, WAKE_KEY, nullptr);
    return true;
  }

  [[nodiscard]] AsyncIoBackend backend() const noexcept override {
    return AsyncIoBackend::Iocp;
  }

  void shutdown() noexcept override {
    if (m_port == nullptr)
      return;
    m_stopping.store(true, std::memory_order_release);
    (void)::PostQueuedCompletionStatus(m_port, 0, WAKE_KEY, nullptr);
    if (m_thread.joinable())
      m_thread.join();
    ::CloseHandle(m_port);
    m_port = nullptr;
  }

private:
  void run() noexcept {
    for (;;) {
      drain_submissions();
      cancel_requested();
      if (m_stopping.load(std::memory_order_acquire) && m_active.empty() &&
          pending_empty())
        return;

      DWORD transferred = 0;
      ULONG_PTR key = 0;
      OVERLAPPED *overlapped = nullptr;
      const BOOL ok = ::GetQueuedCompletionStatus(
          m_port, &transferred, &key, &overlapped, COMPLETION_POLL_MS);
      if (overlapped == nullptr)
        continue;

      auto *io = static_cast<IoOverlapped *>(overlapped);
      Operation *operation = io->owner;
      if (!ok) {
        const DWORD error = ::GetLastError();
        finish(operation, err(map_error(error, io_error::ReadFailed)),
               error == ERROR_OPERATION_ABORTED &&
                   (operation->cancellation_sent ||
                    operation->request.cancellation.stop_requested()));
        continue;
      }
      if (transferred == 0 && operation->completed != operation->request.size) {
        finish(operation,
               err(file_error{io_error::ReadFailed, ERROR_HANDLE_EOF}), false);
        continue;
      }
      operation->completed += transferred;
      if (operation->completed == operation->request.size) {
        finish(operation, std::move(operation->bytes), false);
      } else {
        issue_read(operation);
      }
    }
  }

  void drain_submissions() noexcept {
    nx::vector<Operation *> pending;
    {
      nx::scoped_lock lock(m_pending_lock);
      pending.swap(m_pending);
    }
    for (Operation *operation : pending) {
      if (m_stopping.load(std::memory_order_acquire) ||
          operation->request.cancellation.stop_requested()) {
        operation->completion(
            err(file_error{io_error::Interrupted, ERROR_OPERATION_ABORTED}),
            true);
        nx::release(operation);
        continue;
      }
      open_and_start(operation);
    }
  }

  void open_and_start(Operation *operation) noexcept {
    nx::vector<wchar_t> path;
    try {
      path = widen(operation->request.path.view());
    } catch (...) {
      operation->completion(
          err(file_error{io_error::OpenFailed, ERROR_NOT_ENOUGH_MEMORY}),
          false);
      nx::release(operation);
      return;
    }
    if (path.empty()) {
      operation->completion(
          err(file_error{io_error::InvalidArgument, ERROR_INVALID_NAME}),
          false);
      nx::release(operation);
      return;
    }
    operation->file = ::CreateFileW(
        path.data(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (operation->file == INVALID_HANDLE_VALUE) {
      operation->completion(
          err(map_error(::GetLastError(), io_error::OpenFailed)), false);
      nx::release(operation);
      return;
    }
    if (::CreateIoCompletionPort(operation->file, m_port, 0, 0) == nullptr) {
      const DWORD error = ::GetLastError();
      ::CloseHandle(operation->file);
      operation->file = INVALID_HANDLE_VALUE;
      operation->completion(err(map_error(error, io_error::OpenFailed)), false);
      nx::release(operation);
      return;
    }

    try {
      m_active.push_back(operation);
    } catch (...) {
      finish(operation,
             err(file_error{io_error::ReadFailed, ERROR_NOT_ENOUGH_MEMORY}),
             false);
      return;
    }
    if (operation->request.size == 0) {
      finish(operation, nx::blob<u8>{}, false);
      return;
    }
    if (operation->request.size > std::numeric_limits<usize>::max()) {
      finish(operation, err(file_error{io_error::FileTooLarge, 0}), false);
      return;
    }
    try {
      operation->bytes = nx::blob<u8>(nx::cast<usize>(operation->request.size));
    } catch (...) {
      finish(operation,
             err(file_error{io_error::ReadFailed, ERROR_NOT_ENOUGH_MEMORY}),
             false);
      return;
    }
    issue_read(operation);
  }

  void issue_read(Operation *operation) noexcept {
    if (operation->request.cancellation.stop_requested() ||
        m_stopping.load(std::memory_order_acquire)) {
      finish(operation,
             err(file_error{io_error::Interrupted, ERROR_OPERATION_ABORTED}),
             true);
      return;
    }
    const u64 absolute = operation->request.offset + operation->completed;
    static_cast<OVERLAPPED &>(operation->io) = OVERLAPPED{};
    operation->io.owner = operation;
    operation->io.Offset = nx::cast<DWORD>(absolute & 0xffffffffu);
    operation->io.OffsetHigh = nx::cast<DWORD>(absolute >> 32u);
    const usize remaining =
        nx::cast<usize>(operation->request.size - operation->completed);
    const DWORD requested = nx::cast<DWORD>(nx::min(READ_CHUNK, remaining));
    const BOOL accepted = ::ReadFile(
        operation->file, operation->bytes.data() + operation->completed,
        requested, nullptr, &operation->io);
    if (!accepted) {
      const DWORD error = ::GetLastError();
      if (error != ERROR_IO_PENDING)
        finish(operation, err(map_error(error, io_error::ReadFailed)), false);
    }
  }

  void cancel_requested() noexcept {
    const bool stopping = m_stopping.load(std::memory_order_acquire);
    for (Operation *operation : m_active) {
      if (!operation->cancellation_sent &&
          (stopping || operation->request.cancellation.stop_requested())) {
        operation->cancellation_sent = true;
        (void)::CancelIoEx(operation->file, &operation->io);
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
    if (operation->file != INVALID_HANDLE_VALUE)
      ::CloseHandle(operation->file);
    operation->file = INVALID_HANDLE_VALUE;
    operation->completion(std::move(result), cancelled);
    nx::release(operation);
  }

  [[nodiscard]] bool pending_empty() noexcept {
    nx::scoped_lock lock(m_pending_lock);
    return m_pending.empty();
  }

  HANDLE m_port = nullptr;
  nx::thread m_thread;
  nx::mutex m_pending_lock;
  nx::vector<Operation *> m_pending;
  nx::vector<Operation *> m_active;
  std::atomic<bool> m_stopping{false};
};

} // namespace

nx::unique_ptr<NativeAsyncIo> make_native_async_io(const char *thread_name) {
  IocpAsyncIo *backend = nx::allocate<IocpAsyncIo>();
  if (backend == nullptr)
    return {};
  if (!backend->start(thread_name)) {
    nx::release(backend);
    return {};
  }
  return nx::unique_ptr<NativeAsyncIo>(backend);
}

} // namespace nx::vfs::detail

#endif
