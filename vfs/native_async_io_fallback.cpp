#include "core/foundation/vfs/native_async_io.h"

namespace nx::vfs::detail {

nx::unique_ptr<NativeAsyncIo> make_native_async_io(const char *) { return {}; }

} // namespace nx::vfs::detail
