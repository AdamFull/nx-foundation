#include "core/foundation/platform/os.h"

#include "core/foundation/core/time.h"
#include "core/foundation/core/scope_guard.h"

#include <chrono>

#if NX_USE_MIMALLOC
#include <mimalloc-stats.h>
#endif

#if NX_HAS_WINDOWS_API
#include <windows.h>
// Do not reorder!
#include <psapi.h>
#elif NX_PLATFORM_POSIX
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace nx {
namespace vmem {
namespace detail {
#if NX_HAS_WINDOWS_API
inline DWORD translate_protection_flags(const MemProtFlags p) {
  if (p.none() || p.has(MemProtFlag::None))
    return PAGE_NOACCESS;

  const bool has_write = p.has(MemProtFlag::Write);
  const bool has_exec = p.has(MemProtFlag::Exec);

  if (has_write) {
    if (has_exec)
      return PAGE_EXECUTE_READWRITE;
    return PAGE_READWRITE;
  }

  if (has_exec)
    return PAGE_EXECUTE_READ;

  return PAGE_READONLY;
}
#elif NX_PLATFORM_POSIX
inline i32 translate_protection_flags(const MemProtFlags p) {
  if (p.none() || p.has(MemProtFlag::None))
    return PROT_NONE;

  i32 flags = 0;
  if (p.has(MemProtFlag::Read))
    flags |= PROT_READ;
  if (p.has(MemProtFlag::Write))
    flags |= PROT_WRITE;
  if (p.has(MemProtFlag::Exec))
    flags |= PROT_EXEC;
  return flags;
}
#endif
} // namespace detail

usize get_page_size() {
#if NX_HAS_WINDOWS_API
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwPageSize;
#elif NX_PLATFORM_POSIX
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    return 4096;
  return nx::cast<usize>(page_size);
#endif
}

usize get_allocation_granularity() {
#if NX_HAS_WINDOWS_API
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwAllocationGranularity;
#elif NX_PLATFORM_POSIX
  return get_page_size();
#endif
}

bool reserve(void **out_base, const usize reserve_bytes) {
  if (!out_base)
    return false;

#if NX_HAS_WINDOWS_API
  void *base = VirtualAlloc(nullptr, reserve_bytes, MEM_RESERVE, PAGE_NOACCESS);
  if (!base)
    return false;
#elif NX_PLATFORM_POSIX
  void *base = mmap(nullptr, reserve_bytes, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED)
    return false;
#endif

  *out_base = base;
  return true;
}

bool release(void *base, const usize reserve_bytes) {
  if (!base)
    return false;

  (void)reserve_bytes;
#if NX_HAS_WINDOWS_API
  return VirtualFree(base, 0, MEM_RELEASE) != 0;
#elif NX_PLATFORM_POSIX
  return munmap(base, reserve_bytes) == 0;
#endif
}

bool commit(void *addr, const usize size) {
  if (!addr)
    return false;

#if NX_HAS_WINDOWS_API
  return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#elif NX_PLATFORM_POSIX
  return mprotect(addr, size, PROT_READ | PROT_WRITE) == 0;
#endif
}

bool protect(void *addr, const usize size, const MemProtFlags prot) {
  if (!addr)
    return false;

#if NX_HAS_WINDOWS_API
  DWORD old;
  if (const DWORD new_prot = detail::translate_protection_flags(prot);
      !VirtualProtect(addr, size, new_prot, &old))
    return false;
#elif NX_PLATFORM_POSIX
  i32 flags = detail::translate_protection_flags(prot);
  if (mprotect(addr, size, flags) != 0)
    return false;
#endif

  return true;
}
} // namespace vmem

namespace os {
MemStats mem_get_stats() noexcept {
  MemStats s = {};

#if NX_USE_MIMALLOC
  // mi_process_info wants size_t*, which is not u64* everywhere (it is
  // unsigned long long on Darwin and unsigned long here), so go through
  // temporaries rather than reinterpreting the struct's fields.
  size_t rss = 0, peak_rss = 0, committed = 0;
  mi_process_info(nullptr, nullptr, nullptr, &rss, &peak_rss, &committed,
                  nullptr, nullptr);
  s.rss = rss;
  s.peak_rss = peak_rss;
  s.heap_committed = committed;

#elif defined(NX_PLATFORM_WINDOWS)
  PROCESS_MEMORY_COUNTERS_EX pmc = {};
  pmc.cb = sizeof(pmc);
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc),
                           sizeof(pmc))) {
    s.rss = pmc.WorkingSetSize;
    s.peak_rss = pmc.PeakWorkingSetSize;
    s.heap_committed = pmc.PrivateUsage;
  }
#elif defined(NX_PLATFORM_MACOS)
  task_vm_info_data_t info = {};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO,
                reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
    s.rss = nx::cast<u64>(info.phys_footprint);
    s.heap_committed = nx::cast<u64>(info.internal + info.compressed);
  }
  struct rusage ru = {};
  getrusage(RUSAGE_SELF, &ru);
  s.peak_rss = nx::cast<u64>(ru.ru_maxrss); // bytes on macOS
#elif defined(NX_PLATFORM_LINUX) || defined(NX_PLATFORM_ANDROID)
  if (FILE *f = fopen("/proc/self/status", "r")) {
    defer { fclose(f); };
    char line[128];
    while (fgets(line, sizeof(line), f)) {
      u64 kb = 0;
      if (sscanf(line, "VmRSS: %llu kB", &kb) == 1)
        s.rss = kb * 1024u;
      else if (sscanf(line, "VmHWM: %llu kB", &kb) == 1)
        s.peak_rss = kb * 1024u;
    }
    s.heap_committed = s.rss;
  }
#endif
  return s;
}

u64 ticks_ns() noexcept { return nx::timestamp::now().nanos(); }

u64 ticks_ms() noexcept { return ticks_ns() / 1'000'000u; }

u32 system_ram_mb() noexcept {
#if NX_HAS_WINDOWS_API
  MEMORYSTATUSEX ms = {};
  ms.dwLength = sizeof(ms);
  if (!GlobalMemoryStatusEx(&ms))
    return 0;
  return nx::cast<u32>(ms.ullTotalPhys / (1024ull * 1024ull));
#elif NX_PLATFORM_POSIX
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long page_size = sysconf(_SC_PAGE_SIZE);
  if (pages <= 0 || page_size <= 0)
    return 0;
  return nx::cast<u32>((nx::cast<u64>(pages) * nx::cast<u64>(page_size)) /
                       (1024ull * 1024ull));
#else
  return 0;
#endif
}
} // namespace os

} // namespace nx