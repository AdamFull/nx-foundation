#pragma once

#include "core/foundation/core/callable.h"
#include "core/foundation/threading/cancellation.h"
#include "core/foundation/vfs/async_io.h"

namespace nx::vfs::detail {

struct NativeAsyncReadRequest {
  nx::string path;
  u64 offset = 0;
  u64 size = 0;
  cancellation_token cancellation;
};

using NativeAsyncReadCompletion =
    nx::unique_function<void(io_result<nx::blob<u8>>, bool), 64>;

/// Platform-neutral kernel I/O boundary. Implementations own file handles and
/// request state until the completion callback has run. submit() never blocks
/// on filesystem I/O and callbacks may run on the backend completion thread.
class NativeAsyncIo {
public:
  virtual ~NativeAsyncIo() = default;

  NativeAsyncIo(const NativeAsyncIo &) = delete;
  NativeAsyncIo &operator=(const NativeAsyncIo &) = delete;

  [[nodiscard]] virtual bool
  submit(NativeAsyncReadRequest request,
         NativeAsyncReadCompletion completion) noexcept = 0;
  [[nodiscard]] virtual AsyncIoBackend backend() const noexcept = 0;
  /// Stops accepting work, requests cancellation, drains completions, and
  /// joins backend-owned threads before returning.
  virtual void shutdown() noexcept = 0;

protected:
  NativeAsyncIo() = default;
};

[[nodiscard]] nx::unique_ptr<NativeAsyncIo>
make_native_async_io(const char *thread_name);

} // namespace nx::vfs::detail
