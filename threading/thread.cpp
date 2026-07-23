#include "foundation/threading/thread.h"
#include "foundation/threading/sync.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if NX_HAS_WINDOWS_API
#include <windows.h>
// Do not reorder!
#include <process.h>
#elif NX_PLATFORM_POSIX
#include <cerrno>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

namespace nx {

// nx::thread

namespace {
struct thread_start {
  thread::entry_fn fn;
  char name[32] = {};
  u64 affinity_mask = 0;
  thread_priority priority = thread_priority::Normal;
};

void apply_start_options(const thread_start &s) noexcept {
  if (s.name[0])
    this_thread::set_name(s.name);
  if (s.affinity_mask)
    this_thread::set_affinity(s.affinity_mask);
  if (s.priority != thread_priority::Normal)
    this_thread::set_priority(s.priority);
}

#if NX_HAS_WINDOWS_API
unsigned __stdcall thread_entry(void *arg) {
  auto *start = nx::cast<thread_start *>(arg);
  apply_start_options(*start);
  thread::entry_fn fn = std::move(start->fn);
  release(start);
  fn();
  return 0;
}
#elif NX_PLATFORM_POSIX
void *thread_entry(void *arg) {
  auto *start = nx::cast<thread_start *>(arg);
  apply_start_options(*start);
  thread::entry_fn fn = std::move(start->fn);
  release(start);
  fn();
  return nullptr;
}
#endif
} // namespace

thread::thread(entry_fn fn, const thread_options &opts) {
  if (!fn)
    return;

  auto *start = allocate<thread_start>();
  if (!start)
    return;
  start->fn = std::move(fn);
  if (opts.name)
    snprintf(start->name, sizeof(start->name), "%s", opts.name);
  start->affinity_mask = opts.affinity_mask;
  start->priority = opts.priority;

#if NX_HAS_WINDOWS_API
  unsigned tid = 0;
  const uintptr_t h =
      _beginthreadex(nullptr, nx::cast<unsigned>(opts.stack_size), thread_entry,
                     start, 0, &tid);
  if (!h) {
    release(start);
    return;
  }
  handle_ = h;
  id_ = tid;
#elif NX_PLATFORM_POSIX
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  if (opts.stack_size)
    pthread_attr_setstacksize(&attr, opts.stack_size);

  pthread_t t;
  const int rc = pthread_create(&t, &attr, thread_entry, start);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    release(start);
    return;
  }
  handle_ = nx::cast<uintptr_t>(t);
  id_ = nx::cast<u64>(t);
#endif
}

void thread::join() {
  assert(joinable() && "thread::join on a non-joinable thread");
  assert(id_ != this_thread::id() && "thread::join on itself");

#if NX_HAS_WINDOWS_API
  WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), INFINITE);
  CloseHandle(reinterpret_cast<HANDLE>(handle_));
#elif NX_PLATFORM_POSIX
  pthread_join(nx::cast<pthread_t>(handle_), nullptr);
#endif
  handle_ = 0;
  id_ = 0;
}

void thread::detach() {
  assert(joinable() && "thread::detach on a non-joinable thread");

#if NX_HAS_WINDOWS_API
  CloseHandle(reinterpret_cast<HANDLE>(handle_));
#elif NX_PLATFORM_POSIX
  pthread_detach(nx::cast<pthread_t>(handle_));
#endif
  handle_ = 0;
  id_ = 0;
}

// nx::this_thread

namespace this_thread {

u64 id() noexcept {
  static thread_local u64 cached = 0;
  if (NX_UNLIKELY(cached == 0)) {
#if NX_HAS_WINDOWS_API
    cached = nx::cast<u64>(GetCurrentThreadId());
#elif NX_PLATFORM_POSIX
    cached = nx::cast<u64>(syscall(SYS_gettid));
#endif
  }
  return cached;
}

void yield() noexcept {
#if NX_HAS_WINDOWS_API
  SwitchToThread();
#elif NX_PLATFORM_POSIX
  sched_yield();
#endif
}

#if NX_HAS_WINDOWS_API
// Windows 10 1803+; define it here so the SDK's _WIN32_WINNT level cannot
// silently drop us back to the coarse path.
#if !defined(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

/// One high-resolution timer per thread, closed when the thread exits.
struct waitable_timer {
  HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
  ~waitable_timer() {
    if (handle)
      CloseHandle(handle);
  }
};

/// Current scheduler tick, in 100 ns units.
///
/// This matters because a high-resolution waitable timer only keeps its
/// sub-millisecond accuracy while a single wait stays under the scheduler
/// period; past that it degrades back to tick granularity. sleep_for() slices
/// its waits against this value.
///
/// Read once. Another process raising the global timer resolution can change
/// it later, but that only ever makes our slices conservatively small.
u64 timer_period_100ns() noexcept {
  // NtQueryTimerResolution is the only way to read the period actually in
  // force (timeGetDevCaps reports the supported range, not the current value).
  // Undocumented, but present and stable since NT 3.1.
  using query_fn = LONG(NTAPI *)(PULONG, PULONG, PULONG);

  if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    const auto query = reinterpret_cast<query_fn>(reinterpret_cast<void *>(
        GetProcAddress(ntdll, "NtQueryTimerResolution")));
    ULONG coarsest = 0, finest = 0, current = 0;
    if (query != nullptr && query(&coarsest, &finest, &current) == 0 &&
        current != 0)
      return current;
  }
  return 156250; // 15.625 ms, the default tick.
}

} // namespace
#endif

void sleep_for(const std::chrono::nanoseconds d) noexcept {
  if (d <= std::chrono::nanoseconds::zero())
    return;

#if NX_HAS_WINDOWS_API
  // Sleep() only resolves to the scheduler tick (~15.6 ms by default), so a
  // 14 ms request can block for 23 ms - enough to turn a 60 Hz frame budget
  // into 42 fps. A high-resolution waitable timer avoids that without
  // timeBeginPeriod()'s process-global (and battery-hostile) side effects.
  static thread_local waitable_timer timer;

  if (timer.handle != nullptr) {
    static const u64 period = timer_period_100ns();
    // Stay just under the scheduler period, where the timer is accurate.
    const i64 fine_slice = nx::cast<i64>(period * 95 / 100);
    const auto deadline = std::chrono::steady_clock::now() + d;

    for (;;) {
      const auto remaining = deadline - std::chrono::steady_clock::now();
      const i64 remaining_100ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(remaining)
              .count() /
          100;
      if (remaining_100ns <= 0)
        break;

      i64 slice;
      if (remaining_100ns > 2 * nx::cast<i64>(period)) {
        // Far out: cover the distance in one wait, deliberately stopping a
        // full period short. Tick-granularity overshoot then cannot carry us
        // past the deadline, and a multi-second sleep costs one wakeup here
        // instead of one per tick.
        slice = remaining_100ns - nx::cast<i64>(period);
      } else {
        // Close in, where precision is what matters: sub-period slices.
        slice = remaining_100ns < fine_slice ? remaining_100ns : fine_slice;
      }

      LARGE_INTEGER due;
      due.QuadPart = -slice; // negative = relative, in 100 ns units
      if (!SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE))
        break;
      WaitForSingleObject(timer.handle, INFINITE);
    }
    return;
  }

  // Pre-1803 Windows: round down to whole ticks so we never overshoot, and let
  // the caller's spin (if any) cover the remainder.
  const auto ms = std::chrono::ceil<std::chrono::milliseconds>(d);
  Sleep(nx::cast<DWORD>(ms.count()));
#elif NX_PLATFORM_POSIX
  timespec ts;
  ts.tv_sec = nx::cast<time_t>(d.count() / 1000000000ll);
  ts.tv_nsec = nx::cast<long>(d.count() % 1000000000ll);
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
  }
#endif
}

void set_name(const char *name) noexcept {
  if (!name)
    return;

#if NX_HAS_WINDOWS_API
  wchar_t wide[64];
  const int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, wide,
                                    nx::cast<int>(array_size(wide)));
  if (n > 0)
    SetThreadDescription(GetCurrentThread(), wide);
#elif NX_PLATFORM_POSIX
  char truncated[16]; // Linux limits thread names to 15 chars + NUL
  snprintf(truncated, sizeof(truncated), "%s", name);
  pthread_setname_np(pthread_self(), truncated);
#endif
}

bool set_affinity(const u64 mask) noexcept {
  if (!mask)
    return false;

#if NX_HAS_WINDOWS_API
  return SetThreadAffinityMask(GetCurrentThread(), nx::cast<DWORD_PTR>(mask)) !=
         0;
#elif NX_PLATFORM_POSIX
  cpu_set_t set;
  CPU_ZERO(&set);
  for (u32 i = 0; i < 64; ++i)
    if (mask & (1ull << i))
      CPU_SET(i, &set);
#if defined(NX_PLATFORM_ANDROID)
  // Bionic has no pthread_setaffinity_np; go through the syscall wrapper
  // instead. Passing tid 0 means "the calling thread".
  return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#endif
#endif
}

bool set_priority(const thread_priority prio) noexcept {
#if NX_HAS_WINDOWS_API
  int native = THREAD_PRIORITY_NORMAL;
  switch (prio) {
  case thread_priority::Low:
    native = THREAD_PRIORITY_LOWEST;
    break;
  case thread_priority::BelowNormal:
    native = THREAD_PRIORITY_BELOW_NORMAL;
    break;
  case thread_priority::Normal:
    native = THREAD_PRIORITY_NORMAL;
    break;
  case thread_priority::AboveNormal:
    native = THREAD_PRIORITY_ABOVE_NORMAL;
    break;
  case thread_priority::High:
    native = THREAD_PRIORITY_HIGHEST;
    break;
  }
  return SetThreadPriority(GetCurrentThread(), native) != 0;
#elif NX_PLATFORM_POSIX
  // Nice values apply per-thread on Linux. Raising priority (negative nice)
  // requires CAP_SYS_NICE and may fail; treat that as best-effort.
  int nice_value = 0;
  switch (prio) {
  case thread_priority::Low:
    nice_value = 10;
    break;
  case thread_priority::BelowNormal:
    nice_value = 5;
    break;
  case thread_priority::Normal:
    nice_value = 0;
    break;
  case thread_priority::AboveNormal:
    nice_value = -5;
    break;
  case thread_priority::High:
    nice_value = -10;
    break;
  }
  return setpriority(PRIO_PROCESS, nx::cast<id_t>(syscall(SYS_gettid)),
                     nice_value) == 0;
#endif
}

} // namespace this_thread

// nx::cpu

namespace cpu {
namespace {

#if NX_HAS_WINDOWS_API

void add_cache(topology_info &t, u8 level, cache_type type, u32 line_size,
               u64 size) noexcept {
  for (cache_info &c : t.caches) {
    if (c.level == level && c.type == type && c.size == size) {
      ++c.instance_count;
      return;
    }
  }
  if (t.caches.size() < t.caches.capacity())
    t.caches.push_back({level, type, line_size, size, 1});
}

void query_topology(topology_info &t) noexcept {
  t.logical_count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);

  DWORD len = 0;
  GetLogicalProcessorInformationEx(RelationAll, nullptr, &len);
  if (len == 0)
    return;
  auto *buffer = nx::cast<u8 *>(mem_alloc(len));
  if (!buffer)
    return;
  if (!GetLogicalProcessorInformationEx(
          RelationAll,
          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer),
          &len)) {
    mem_free(buffer);
    return;
  }

  small_vector<u64, 8> package_masks;

  for (DWORD offset = 0; offset < len;) {
    const auto *info =
        reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(
            buffer + offset);
    switch (info->Relationship) {
    case RelationProcessorCore: {
      core_info core;
      core.index = nx::cast<u32>(t.cores.size());
      core.efficiency_class = info->Processor.EfficiencyClass;
      for (WORD g = 0; g < info->Processor.GroupCount; ++g) {
        const GROUP_AFFINITY &ga = info->Processor.GroupMask[g];
        core.logical_count += nx::cast<u32>(std::popcount(ga.Mask));
        if (ga.Group == 0) // logical_mask only covers the first group
          core.logical_mask |= nx::cast<u64>(ga.Mask);
      }
      t.cores.push_back(core);
      t.efficiency_class_count =
          max(t.efficiency_class_count, core.efficiency_class + 1);
      break;
    }
    case RelationProcessorPackage: {
      u64 mask = 0;
      for (WORD g = 0; g < info->Processor.GroupCount; ++g)
        if (info->Processor.GroupMask[g].Group == 0)
          mask |= nx::cast<u64>(info->Processor.GroupMask[g].Mask);
      package_masks.push_back(mask);
      break;
    }
    case RelationNumaNode:
      ++t.numa_node_count;
      break;
    case RelationCache: {
      const CACHE_RELATIONSHIP &c = info->Cache;
      if (c.Level >= 1 && c.Level <= 3) {
        const cache_type type = c.Type == CacheData ? cache_type::Data
                                : c.Type == CacheInstruction
                                    ? cache_type::Instruction
                                    : cache_type::Unified;
        add_cache(t, c.Level, type, c.LineSize, c.CacheSize);
        if (c.Level == 1 && type != cache_type::Instruction)
          t.cache_line_size = c.LineSize;
      }
      break;
    }
    default:
      break;
    }
    offset += info->Size;
  }
  mem_free(buffer);

  t.physical_count = nx::cast<u32>(t.cores.size());
  t.package_count = nx::cast<u32>(package_masks.size());
  for (core_info &core : t.cores)
    for (u32 p = 0; p < package_masks.size(); ++p)
      if (core.logical_mask & package_masks[p]) {
        core.package = p;
        break;
      }
}

#elif NX_PLATFORM_POSIX

bool read_sys_u64(const char *path, u64 &out) noexcept {
  FILE *f = fopen(path, "r");
  if (!f)
    return false;
  char buf[64] = {};
  const bool ok = fgets(buf, sizeof(buf), f) != nullptr;
  fclose(f);
  if (!ok)
    return false;
  out = strtoull(buf, nullptr, 10);
  return true;
}

bool read_sys_str(const char *path, char *buf, usize size) noexcept {
  FILE *f = fopen(path, "r");
  if (!f)
    return false;
  const bool ok = fgets(buf, nx::cast<int>(size), f) != nullptr;
  fclose(f);
  if (ok) {
    if (char *nl = strchr(buf, '\n'))
      *nl = '\0';
  }
  return ok;
}

// Parses "32K" / "1024K" / "8M" style sysfs cache sizes.
u64 parse_cache_size(const char *s) noexcept {
  char *end = nullptr;
  u64 v = strtoull(s, &end, 10);
  if (end) {
    if (*end == 'K' || *end == 'k')
      v *= 1024;
    else if (*end == 'M' || *end == 'm')
      v *= 1024 * 1024;
  }
  return v;
}

// First cpu index in a "0-3,8-11" style cpu list.
i64 parse_first_cpu(const char *s) noexcept {
  if (!s[0])
    return -1;
  return nx::cast<i64>(strtoll(s, nullptr, 10));
}

void query_topology(topology_info &t) noexcept {
  const long online = sysconf(_SC_NPROCESSORS_ONLN);
  t.logical_count = online > 0 ? nx::cast<u32>(online) : 1;

  char path[128];
  char buf[256];

  // Physical cores: group logical cpus by (package_id, core_id).
  small_vector<u64, 64> core_keys; // (package << 32) | core_id
  small_vector<u64, 64> capacities;
  bool has_topology = false;

  for (u32 lp = 0; lp < t.logical_count && lp < 64; ++lp) {
    u64 core_id = lp, pkg_id = 0;
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%u/topology/core_id", lp);
    const bool got_core = read_sys_u64(path, core_id);
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%u/topology/physical_package_id", lp);
    read_sys_u64(path, pkg_id);
    has_topology = has_topology || got_core;

    u64 capacity = 1024; // arm big.LITTLE exposes relative cpu_capacity
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpu_capacity",
             lp);
    read_sys_u64(path, capacity);

    const u64 key = (pkg_id << 32) | (core_id & 0xFFFFFFFFull);
    usize core_index = core_keys.size();
    for (usize i = 0; i < core_keys.size(); ++i)
      if (core_keys[i] == key) {
        core_index = i;
        break;
      }
    if (core_index == core_keys.size()) {
      core_keys.push_back(key);
      capacities.push_back(capacity);
      core_info core;
      core.index = nx::cast<u32>(core_index);
      core.package = nx::cast<u32>(pkg_id);
      t.cores.push_back(core);
      t.package_count = max(t.package_count, nx::cast<u32>(pkg_id) + 1);
    }
    t.cores[core_index].logical_mask |= 1ull << lp;
    t.cores[core_index].logical_count += 1;
  }

  if (!has_topology || t.cores.empty()) {
    // sysfs unavailable (restricted sandbox): assume one logical per core.
    t.cores.clear();
    for (u32 i = 0; i < t.logical_count && i < 64; ++i) {
      core_info core;
      core.index = i;
      core.logical_count = 1;
      core.logical_mask = 1ull << i;
      t.cores.push_back(core);
    }
    t.package_count = 1;
  }
  t.physical_count = nx::cast<u32>(t.cores.size());

  // Efficiency classes: rank of the core's capacity among distinct values.
  small_vector<u64, 8> distinct;
  for (usize i = 0; i < capacities.size(); ++i) {
    bool known = false;
    for (u64 d : distinct)
      known = known || d == capacities[i];
    if (!known)
      distinct.push_back(capacities[i]);
  }
  // Insertion sort ascending: lowest capacity = efficiency class 0.
  for (usize i = 1; i < distinct.size(); ++i)
    for (usize j = i; j > 0 && distinct[j - 1] > distinct[j]; --j)
      std::swap(distinct[j - 1], distinct[j]);
  t.efficiency_class_count = max<u32>(1, nx::cast<u32>(distinct.size()));
  for (usize i = 0; i < capacities.size() && i < t.cores.size(); ++i)
    for (u32 rank = 0; rank < distinct.size(); ++rank)
      if (distinct[rank] == capacities[i]) {
        t.cores[i].efficiency_class = rank;
        break;
      }

  // Caches: layout from cpu0, instance count = number of cpus that are the
  // first member of their shared set for that cache index.
  for (u32 index = 0; index < 10; ++index) {
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu0/cache/index%u/level", index);
    u64 level = 0;
    if (!read_sys_u64(path, level))
      break;
    if (level < 1 || level > 3)
      continue;

    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu0/cache/index%u/type", index);
    cache_type type = cache_type::Unified;
    if (read_sys_str(path, buf, sizeof(buf))) {
      if (strcmp(buf, "Data") == 0)
        type = cache_type::Data;
      else if (strcmp(buf, "Instruction") == 0)
        type = cache_type::Instruction;
    }

    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu0/cache/index%u/size", index);
    u64 size = 0;
    if (read_sys_str(path, buf, sizeof(buf)))
      size = parse_cache_size(buf);

    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu0/cache/index%u/coherency_line_size",
             index);
    u64 line = 0;
    read_sys_u64(path, line);
    if (level == 1 && type != cache_type::Instruction && line)
      t.cache_line_size = nx::cast<u32>(line);

    u32 instances = 0;
    for (u32 lp = 0; lp < t.logical_count; ++lp) {
      snprintf(path, sizeof(path),
               "/sys/devices/system/cpu/cpu%u/cache/index%u/shared_cpu_list",
               lp, index);
      if (read_sys_str(path, buf, sizeof(buf)) &&
          parse_first_cpu(buf) == nx::cast<i64>(lp))
        ++instances;
    }

    if (t.caches.size() < t.caches.capacity())
      t.caches.push_back({nx::cast<u8>(level), type, nx::cast<u32>(line), size,
                          max<u32>(instances, 1)});
  }

  // NUMA nodes.
  for (u32 node = 0;; ++node) {
    snprintf(path, sizeof(path), "/sys/devices/system/node/node%u/cpulist",
             node);
    if (!read_sys_str(path, buf, sizeof(buf)))
      break;
    ++t.numa_node_count;
  }
  if (t.numa_node_count == 0)
    t.numa_node_count = 1;
}

#endif

void finalize_topology(topology_info &t) noexcept {
  if (t.logical_count == 0)
    t.logical_count = 1;
  if (t.physical_count == 0)
    t.physical_count = t.logical_count;
  if (t.package_count == 0)
    t.package_count = 1;
  if (t.numa_node_count == 0)
    t.numa_node_count = 1;
  if (t.cache_line_size == 0)
    t.cache_line_size = 64;
  if (t.efficiency_class_count == 0)
    t.efficiency_class_count = 1;
}

topology_info g_topology;
once_flag g_topology_once;

} // namespace

const topology_info &topology() noexcept {
  call_once(g_topology_once, [] {
    query_topology(g_topology);
    finalize_topology(g_topology);
  });
  return g_topology;
}

u32 logical_core_count() noexcept { return topology().logical_count; }
u32 physical_core_count() noexcept { return topology().physical_count; }
u32 cache_line_size() noexcept { return topology().cache_line_size; }

u32 performance_core_count() noexcept {
  const topology_info &t = topology();
  if (t.efficiency_class_count <= 1)
    return t.physical_count;
  u32 count = 0;
  for (const core_info &c : t.cores)
    if (c.efficiency_class == t.efficiency_class_count - 1)
      ++count;
  return count;
}

u32 efficiency_core_count() noexcept {
  const topology_info &t = topology();
  return t.physical_count - performance_core_count();
}

} // namespace cpu

} // namespace nx
