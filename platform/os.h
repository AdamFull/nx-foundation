#pragma once

#include "core/foundation/core/enum.h"

namespace nx::vmem {
enum class MemProtFlag : u32 {
  None = 1 << 0,
  Read = 1 << 1,
  Write = 1 << 2,
  Exec = 1 << 3
};
NX_ENUM_FLAGS_DEFINE(MemProtFlag, MemProtFlags)
}

namespace nx::vmem {
usize get_page_size();
usize get_allocation_granularity();

bool reserve(void **out_base, usize reserve_bytes);
bool release(void *base, usize reserve_bytes);
bool commit(void *addr, usize size);
bool protect(void *addr, usize size, MemProtFlags prot);
} // namespace nx::vmem

namespace nx::os {
struct MemStats {
  u64 rss = 0;
  u64 peak_rss = 0;
  u64 heap_committed = 0;
};

MemStats mem_get_stats() noexcept;

// Monotonic clock since an unspecified epoch; use for measuring intervals.
u64 ticks_ns() noexcept;
u64 ticks_ms() noexcept;

// Total physical system memory in megabytes (0 if it cannot be determined).
u32 system_ram_mb() noexcept;
}
