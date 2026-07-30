#include "foundation/core/foundation.h"

#if NX_ALLOC_GUARD

namespace nx::detail {

// Out of line rather than an inline variable in the header: MSVC has been
// known to give a header-defined thread_local a per-translation-unit address,
// which would let one TU arm the guard and another read zero.
thread_local u32 t_no_alloc_depth = 0;

} // namespace nx::detail

#endif
