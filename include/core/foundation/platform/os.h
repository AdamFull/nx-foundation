#pragma once

#include "core/foundation/core/enum.h"
#include "core/foundation/strings/utf8_string.h"

#include <optional>
#include <span>

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
}

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

// A moment as the user's clock and calendar show it, in their time zone.
struct LocalTime {
  i32 year = 1970;
  u8 month = 1;
  u8 day = 1;
  u8 hour = 0;
  u8 minute = 0;
  u8 second = 0;
};

// @p seconds since 1970 as the user's clock shows it; nothing where it
// cannot be told.
[[nodiscard]] std::optional<LocalTime> local_time(i64 seconds) noexcept;

// Total physical system memory in megabytes (0 if it cannot be determined).
u32 system_ram_mb() noexcept;

usize os_version(char *out, usize capacity) noexcept;
usize device_model(char *out, usize capacity) noexcept;
// The country or region the user's settings name, as two upper-case ISO 3166
// letters ("GB"); empty where it cannot be told.
usize region(char *out, usize capacity) noexcept;
// The region a locale name holds: "en_US.UTF-8", "en-GB" and "zh-Hans-CN"
// give "US", "GB" and "CN"; "C" and "es-419" give nothing.
usize region_of_locale(const char *locale, char *out, usize capacity) noexcept;
usize cpu_brand(char *out, usize capacity) noexcept;

u32 process_id() noexcept;
usize executable_path(char *out, usize capacity) noexcept;

// The value of the environment variable @p name, in UTF-8 whatever the
// platform keeps it in (Windows' narrow environment is the ANSI code page);
// nothing when it is not set.
[[nodiscard]] std::optional<string> environment_variable(string_view name);

// Fills @p out from the operating system's cryptographically secure generator,
// for keys, nonces and the like; false where it has none or it failed.
[[nodiscard]] bool random_bytes(std::span<u8> out) noexcept;
}
