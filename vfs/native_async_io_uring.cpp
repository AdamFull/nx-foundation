#include "core/foundation/vfs/native_async_io.h"

#if defined(NX_PLATFORM_LINUX) || defined(NX_PLATFORM_ANDROID)

#if __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>

namespace nx::vfs::detail {
namespace {

constexpr u32 RING_ENTRIES = 256;
constexpr usize READ_CHUNK = 4u * 1024u * 1024u;
constexpr u64 CANCEL_USER_DATA = 1;

#if defined(SYS_io_uring_setup)
constexpr long SYS_SETUP = SYS_io_uring_setup;
constexpr long SYS_ENTER = SYS_io_uring_enter;
constexpr long SYS_REGISTER = SYS_io_uring_register;
#else
constexpr long SYS_SETUP = __NR_io_uring_setup;
constexpr long SYS_ENTER = __NR_io_uring_enter;
constexpr long SYS_REGISTER = __NR_io_uring_register;
#endif

[[nodiscard]] file_error map_errno_value(const i32 error,
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
  case EFBIG:
    kind = io_error::FileTooLarge;
    break;
  case EINTR:
  case ECANCELED:
    kind = io_error::Interrupted;
    break;
  default:
    break;
  }
  return {kind, error};
}

class Ring {
public:
  ~Ring() { close(); }

  [[nodiscard]] bool open() noexcept {
    io_uring_params params{};
    m_fd = nx::cast<i32>(::syscall(SYS_SETUP, RING_ENTRIES, &params));
    if (m_fd < 0)
      return false;

    m_sq_size =
        params.sq_off.array + params.sq_entries * nx::cast<u32>(sizeof(u32));
    m_cq_size = params.cq_off.cqes +
                params.cq_entries * nx::cast<u32>(sizeof(io_uring_cqe));
    if ((params.features & IORING_FEAT_SINGLE_MMAP) != 0)
      m_sq_size = m_cq_size = nx::max(m_sq_size, m_cq_size);

    m_sq = ::mmap(nullptr, m_sq_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd,
                  IORING_OFF_SQ_RING);
    if (m_sq == MAP_FAILED)
      return fail_open();
    if ((params.features & IORING_FEAT_SINGLE_MMAP) != 0) {
      m_cq = m_sq;
    } else {
      m_cq = ::mmap(nullptr, m_cq_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    m_fd, IORING_OFF_CQ_RING);
      if (m_cq == MAP_FAILED)
        return fail_open();
    }
    m_sqes_size = params.sq_entries * sizeof(io_uring_sqe);
    m_sqes = nx::cast<io_uring_sqe *>(::mmap(nullptr, m_sqes_size,
                                             PROT_READ | PROT_WRITE, MAP_SHARED,
                                             m_fd, IORING_OFF_SQES));
    if (m_sqes == MAP_FAILED)
      return fail_open();

    auto *sq = nx::cast<u8 *>(m_sq);
    auto *cq = nx::cast<u8 *>(m_cq);
    m_sq_head = reinterpret_cast<u32 *>(sq + params.sq_off.head);
    m_sq_tail = reinterpret_cast<u32 *>(sq + params.sq_off.tail);
    m_sq_mask = reinterpret_cast<u32 *>(sq + params.sq_off.ring_mask);
    m_sq_entries = reinterpret_cast<u32 *>(sq + params.sq_off.ring_entries);
    m_sq_array = reinterpret_cast<u32 *>(sq + params.sq_off.array);
    m_cq_head = reinterpret_cast<u32 *>(cq + params.cq_off.head);
    m_cq_tail = reinterpret_cast<u32 *>(cq + params.cq_off.tail);
    m_cq_mask = reinterpret_cast<u32 *>(cq + params.cq_off.ring_mask);
    m_cqes = reinterpret_cast<io_uring_cqe *>(cq + params.cq_off.cqes);
    return true;
  }

  [[nodiscard]] io_uring_sqe *acquire() noexcept {
    const u32 head =
        std::atomic_ref<u32>(*m_sq_head).load(std::memory_order_acquire);
    const u32 tail =
        std::atomic_ref<u32>(*m_sq_tail).load(std::memory_order_relaxed);
    if (tail - head >= *m_sq_entries)
      return nullptr;
    const u32 index = tail & *m_sq_mask;
    io_uring_sqe *sqe = &m_sqes[index];
    std::memset(sqe, 0, sizeof(*sqe));
    m_sq_array[index] = index;
    std::atomic_ref<u32>(*m_sq_tail).store(tail + 1, std::memory_order_release);
    ++m_to_submit;
    return sqe;
  }

  [[nodiscard]] bool submit() noexcept {
    while (m_to_submit != 0) {
      const i32 submitted = nx::cast<i32>(
          ::syscall(SYS_ENTER, m_fd, m_to_submit, 0u, 0u, nullptr, 0u));
      if (submitted < 0) {
        if (errno == EINTR)
          continue;
        return false;
      }
      if (submitted == 0)
        return false;
      m_to_submit -= nx::cast<u32>(submitted);
    }
    return true;
  }

  template <typename F> void consume(F &&callback) noexcept {
    u32 head = std::atomic_ref<u32>(*m_cq_head).load(std::memory_order_relaxed);
    const u32 tail =
        std::atomic_ref<u32>(*m_cq_tail).load(std::memory_order_acquire);
    while (head != tail) {
      callback(m_cqes[head & *m_cq_mask]);
      ++head;
    }
    std::atomic_ref<u32>(*m_cq_head).store(head, std::memory_order_release);
  }

  [[nodiscard]] bool register_event(const i32 event_fd) noexcept {
    return ::syscall(SYS_REGISTER, m_fd, IORING_REGISTER_EVENTFD, &event_fd,
                     1u) == 0;
  }

  [[nodiscard]] bool supports_required_operations() noexcept {
    alignas(io_uring_probe) std::array<u8, sizeof(io_uring_probe) +
                                               sizeof(io_uring_probe_op) * 255u>
        storage{};
    auto *probe = reinterpret_cast<io_uring_probe *>(storage.data());
    probe->ops_len = 255;
    if (::syscall(SYS_REGISTER, m_fd, IORING_REGISTER_PROBE, probe, 255u) != 0)
      return false;

    bool read = false;
    bool open_at = false;
    bool cancel = false;
    for (u32 i = 0; i < probe->ops_len; ++i) {
      if ((probe->ops[i].flags & IO_URING_OP_SUPPORTED) == 0)
        continue;
      read |= probe->ops[i].op == IORING_OP_READ;
      open_at |= probe->ops[i].op == IORING_OP_OPENAT;
      cancel |= probe->ops[i].op == IORING_OP_ASYNC_CANCEL;
    }
    return read && open_at && cancel;
  }

private:
  [[nodiscard]] bool fail_open() noexcept {
    close();
    return false;
  }

  void close() noexcept {
    if (m_sqes != nullptr && m_sqes != MAP_FAILED)
      ::munmap(m_sqes, m_sqes_size);
    if (m_cq != nullptr && m_cq != MAP_FAILED && m_cq != m_sq)
      ::munmap(m_cq, m_cq_size);
    if (m_sq != nullptr && m_sq != MAP_FAILED)
      ::munmap(m_sq, m_sq_size);
    if (m_fd >= 0)
      ::close(m_fd);
    m_fd = -1;
    m_sq = nullptr;
    m_cq = nullptr;
    m_sqes = nullptr;
  }

  i32 m_fd = -1;
  void *m_sq = nullptr;
  void *m_cq = nullptr;
  io_uring_sqe *m_sqes = nullptr;
  usize m_sq_size = 0;
  usize m_cq_size = 0;
  usize m_sqes_size = 0;
  u32 *m_sq_head = nullptr;
  u32 *m_sq_tail = nullptr;
  u32 *m_sq_mask = nullptr;
  u32 *m_sq_entries = nullptr;
  u32 *m_sq_array = nullptr;
  u32 *m_cq_head = nullptr;
  u32 *m_cq_tail = nullptr;
  u32 *m_cq_mask = nullptr;
  io_uring_cqe *m_cqes = nullptr;
  u32 m_to_submit = 0;
};

class IoUringAsyncIo final : public NativeAsyncIo {
  enum class Phase : u8 { Opening, Reading };

  struct Operation {
    Operation(NativeAsyncReadRequest io_request,
              NativeAsyncReadCompletion io_completion) noexcept
        : request(std::move(io_request)), completion(std::move(io_completion)) {
    }

    NativeAsyncReadRequest request;
    NativeAsyncReadCompletion completion;
    nx::blob<u8> bytes;
    i32 file = -1;
    u64 completed = 0;
    Phase phase = Phase::Opening;
    bool cancellation_sent = false;
  };

public:
  ~IoUringAsyncIo() override { shutdown(); }

  [[nodiscard]] bool start(const char *thread_name) {
    if (!m_ring.open() || !m_ring.supports_required_operations())
      return false;
    m_event = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_event < 0)
      return false;
    m_event_registered = m_ring.register_event(m_event);
    m_thread =
        nx::thread([this] { run(); },
                   {.name = thread_name != nullptr ? thread_name : "nx-uring"});
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
    wake();
    return true;
  }

  [[nodiscard]] AsyncIoBackend backend() const noexcept override {
    return AsyncIoBackend::IoUring;
  }

  void shutdown() noexcept override {
    if (m_event < 0)
      return;
    m_stopping.store(true, std::memory_order_release);
    wake();
    if (m_thread.joinable())
      m_thread.join();
    ::close(m_event);
    m_event = -1;
  }

private:
  void run() noexcept {
    for (;;) {
      drain_event();
      drain_submissions();
      cancel_requested();
      (void)m_ring.submit();
      m_ring.consume([this](const io_uring_cqe &completion) {
        consume_completion(completion);
      });

      if (m_stopping.load(std::memory_order_acquire) && m_active.empty() &&
          pending_empty())
        return;

      pollfd event{.fd = m_event, .events = POLLIN, .revents = 0};
      // A registered eventfd is signalled by CQ completions as well as submit;
      // otherwise the short timeout remains the portable safety net.
      (void)::poll(&event, 1, m_event_registered ? 20 : 2);
    }
  }

  void drain_submissions() noexcept {
    nx::vector<Operation *> pending;
    {
      nx::scoped_lock lock(m_pending_lock);
      pending.swap(m_pending);
    }
    usize consumed = 0;
    for (; consumed < pending.size(); ++consumed) {
      Operation *operation = pending[consumed];
      if (m_stopping.load(std::memory_order_acquire) ||
          operation->request.cancellation.stop_requested()) {
        operation->completion(err(file_error{io_error::Interrupted, ECANCELED}),
                              true);
        nx::release(operation);
        continue;
      }
      try {
        m_active.push_back(operation);
      } catch (...) {
        operation->completion(err(file_error{io_error::ReadFailed, ENOMEM}),
                              false);
        nx::release(operation);
        continue;
      }
      io_uring_sqe *sqe = m_ring.acquire();
      if (sqe == nullptr) {
        m_active.pop_back();
        break;
      }
      sqe->opcode = IORING_OP_OPENAT;
      sqe->fd = AT_FDCWD;
      sqe->addr = nx::cast<u64>(
          reinterpret_cast<uintptr_t>(operation->request.path.c_str()));
      sqe->open_flags = O_RDONLY | O_CLOEXEC;
      sqe->user_data = nx::cast<u64>(reinterpret_cast<uintptr_t>(operation));
    }
    if (consumed != pending.size()) {
      bool requeue_failed = false;
      {
        nx::scoped_lock lock(m_pending_lock);
        try {
          m_pending.insert(m_pending.begin(), pending.begin() + consumed,
                           pending.end());
        } catch (...) {
          requeue_failed = true;
        }
      }
      if (requeue_failed) {
        for (usize i = consumed; i < pending.size(); ++i) {
          pending[i]->completion(err(file_error{io_error::ReadFailed, ENOMEM}),
                                 false);
          nx::release(pending[i]);
        }
      }
    }
  }

  void consume_completion(const io_uring_cqe &entry) noexcept {
    if (entry.user_data == CANCEL_USER_DATA)
      return;
    Operation *operation =
        reinterpret_cast<Operation *>(nx::cast<uintptr_t>(entry.user_data));
    if (entry.res < 0) {
      const i32 error = -entry.res;
      finish(operation,
             err(map_errno_value(error, operation->phase == Phase::Opening
                                            ? io_error::OpenFailed
                                            : io_error::ReadFailed)),
             error == ECANCELED &&
                 (operation->cancellation_sent ||
                  operation->request.cancellation.stop_requested() ||
                  m_stopping.load(std::memory_order_acquire)));
      return;
    }

    if (operation->phase == Phase::Opening) {
      operation->file = entry.res;
      if (operation->request.cancellation.stop_requested() ||
          m_stopping.load(std::memory_order_acquire)) {
        finish(operation, err(file_error{io_error::Interrupted, ECANCELED}),
               true);
        return;
      }
      if (operation->request.size == 0) {
        finish(operation, nx::blob<u8>{}, false);
        return;
      }
      if (operation->request.size > std::numeric_limits<usize>::max()) {
        finish(operation, err(file_error{io_error::FileTooLarge, EFBIG}),
               false);
        return;
      }
      try {
        operation->bytes =
            nx::blob<u8>(nx::cast<usize>(operation->request.size));
      } catch (...) {
        finish(operation, err(file_error{io_error::ReadFailed, ENOMEM}), false);
        return;
      }
      operation->phase = Phase::Reading;
      issue_read(operation);
      return;
    }

    if (entry.res == 0 && operation->completed != operation->request.size) {
      finish(operation, err(file_error{io_error::ReadFailed, 0}), false);
      return;
    }
    operation->completed += nx::cast<u64>(entry.res);
    if (operation->completed == operation->request.size)
      finish(operation, std::move(operation->bytes), false);
    else
      issue_read(operation);
  }

  void issue_read(Operation *operation) noexcept {
    if (operation->request.cancellation.stop_requested() ||
        m_stopping.load(std::memory_order_acquire)) {
      finish(operation, err(file_error{io_error::Interrupted, ECANCELED}),
             true);
      return;
    }
    io_uring_sqe *sqe = m_ring.acquire();
    if (sqe == nullptr) {
      // The completed SQE which led here has already freed capacity; this is
      // only reachable with an externally corrupted ring.
      finish(operation, err(file_error{io_error::ReadFailed, EBUSY}), false);
      return;
    }
    const usize remaining =
        nx::cast<usize>(operation->request.size - operation->completed);
    sqe->opcode = IORING_OP_READ;
    sqe->fd = operation->file;
    sqe->off = operation->request.offset + operation->completed;
    sqe->addr = nx::cast<u64>(reinterpret_cast<uintptr_t>(
        operation->bytes.data() + operation->completed));
    sqe->len = nx::cast<u32>(nx::min(READ_CHUNK, remaining));
    sqe->user_data = nx::cast<u64>(reinterpret_cast<uintptr_t>(operation));
  }

  void cancel_requested() noexcept {
    const bool stopping = m_stopping.load(std::memory_order_acquire);
    for (Operation *operation : m_active) {
      if (operation->cancellation_sent ||
          (!stopping && !operation->request.cancellation.stop_requested()))
        continue;
      io_uring_sqe *sqe = m_ring.acquire();
      if (sqe == nullptr)
        return;
      operation->cancellation_sent = true;
      sqe->opcode = IORING_OP_ASYNC_CANCEL;
      sqe->addr = nx::cast<u64>(reinterpret_cast<uintptr_t>(operation));
      sqe->user_data = CANCEL_USER_DATA;
    }
  }

  void finish(Operation *operation, io_result<nx::blob<u8>> result,
              const bool cancelled) noexcept {
    const auto found = std::find(m_active.begin(), m_active.end(), operation);
    if (found != m_active.end()) {
      *found = m_active.back();
      m_active.pop_back();
    }
    if (operation->file >= 0)
      ::close(operation->file);
    operation->file = -1;
    operation->completion(std::move(result), cancelled);
    nx::release(operation);
  }

  void wake() noexcept {
    const u64 one = 1;
    (void)::write(m_event, &one, sizeof(one));
  }

  void drain_event() noexcept {
    u64 value = 0;
    while (::read(m_event, &value, sizeof(value)) > 0) {
    }
  }

  [[nodiscard]] bool pending_empty() noexcept {
    nx::scoped_lock lock(m_pending_lock);
    return m_pending.empty();
  }

  Ring m_ring;
  i32 m_event = -1;
  bool m_event_registered = false;
  nx::thread m_thread;
  nx::mutex m_pending_lock;
  nx::vector<Operation *> m_pending;
  nx::vector<Operation *> m_active;
  std::atomic<bool> m_stopping{false};
};

} // namespace

nx::unique_ptr<NativeAsyncIo> make_native_async_io(const char *thread_name) {
  IoUringAsyncIo *backend = nx::allocate<IoUringAsyncIo>();
  if (backend == nullptr)
    return {};
  if (!backend->start(thread_name)) {
    nx::release(backend);
    return {};
  }
  return nx::unique_ptr<NativeAsyncIo>(backend);
}

} // namespace nx::vfs::detail

#else

namespace nx::vfs::detail {
nx::unique_ptr<NativeAsyncIo> make_native_async_io(const char *) { return {}; }
} // namespace nx::vfs::detail

#endif
#endif
