#include "core/foundation/platform/os.h"

#include "core/foundation/core/scope_guard.h"
#include "core/foundation/core/time.h"
#include "core/foundation/strings/format.h"

#include <algorithm>
#include <cerrno>
#include <chrono>

#if NX_USE_MIMALLOC
#include <mimalloc-stats.h>
#endif

#if NX_HAS_WINDOWS_API
#include <windows.h>
// Do not reorder!
#include <bcrypt.h>
#include <psapi.h>
#elif NX_PLATFORM_POSIX
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#if NX_ARCH_X64 || NX_ARCH_X86
#if defined(NX_COMPILER_MSVC) && !defined(NX_COMPILER_CLANG)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#if NX_PLATFORM_ANDROID
#include <sys/system_properties.h>
#endif

#if NX_PLATFORM_APPLE
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
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
}

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
}

namespace os {
MemStats mem_get_stats() noexcept {
  MemStats s = {};

#if NX_USE_MIMALLOC
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

namespace {

usize say(char *const out, const usize capacity, const char *const text,
          const usize length) noexcept {
  if (out == nullptr || capacity == 0)
    return 0;
  const usize kept = length < capacity - 1 ? length : capacity - 1;
  for (usize i = 0; i < kept; ++i)
    out[i] = text[i];
  out[kept] = '\0';
  return kept;
}

usize say(char *const out, const usize capacity,
          const char *const text) noexcept {
  usize length = 0;
  while (text[length] != '\0')
    ++length;
  return say(out, capacity, text, length);
}

}

#if NX_PLATFORM_ANDROID
namespace {
usize android_property(const char *const name, char *const out,
                       const usize capacity) noexcept {
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get(name, value) <= 0)
    return say(out, capacity, "");
  return say(out, capacity, value);
}
}
#endif

usize os_version(char *const out, const usize capacity) noexcept {
#if NX_HAS_WINDOWS_API
  using RtlGetVersionFn = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
  const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  RTL_OSVERSIONINFOW info = {};
  info.dwOSVersionInfoSize = sizeof(info);
  if (ntdll != nullptr) {
    const auto query = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void *>(::GetProcAddress(ntdll, "RtlGetVersion")));
    if (query != nullptr && query(&info) == 0) {
      char text[96];
      return say(out, capacity,
                 nx::format_to(text, "Windows {}.{} build {}",
                               info.dwMajorVersion, info.dwMinorVersion,
                               info.dwBuildNumber));
    }
  }
  return say(out, capacity, "Windows");
#elif NX_PLATFORM_ANDROID
  char release[64] = {};
  char sdk[64] = {};
  android_property("ro.build.version.release", release, sizeof(release));
  android_property("ro.build.version.sdk", sdk, sizeof(sdk));
  char text[160];
  return say(out, capacity,
             nx::format_to(text, "Android {} (API {})",
                           nx::string_view(release), nx::string_view(sdk)));
#elif NX_PLATFORM_POSIX
  utsname info = {};
  if (::uname(&info) != 0)
    return say(out, capacity, "");
  char text[256];
  return say(out, capacity,
             nx::format_to(text, "{} {} {}", nx::string_view(info.sysname),
                           nx::string_view(info.release),
                           nx::string_view(info.machine)));
#else
  return say(out, capacity, "");
#endif
}

usize device_model(char *const out, const usize capacity) noexcept {
#if NX_PLATFORM_ANDROID
  char brand[64] = {};
  char model[64] = {};
  android_property("ro.product.manufacturer", brand, sizeof(brand));
  android_property("ro.product.model", model, sizeof(model));
  char text[160];
  return say(out, capacity,
             nx::format_to(text, "{} {}", nx::string_view(brand),
                           nx::string_view(model)));
#elif NX_PLATFORM_APPLE
  char model[128] = {};
  usize size = sizeof(model);
  if (::sysctlbyname("hw.model", model, &size, nullptr, 0) != 0)
    return say(out, capacity, "");
  return say(out, capacity, model);
#elif NX_HAS_WINDOWS_API
  char model[128] = {};
  DWORD size = sizeof(model);
  if (::RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS",
                     "SystemProductName", RRF_RT_REG_SZ, nullptr, model,
                     &size) != ERROR_SUCCESS)
    return say(out, capacity, "");
  return say(out, capacity, model);
#else
  return say(out, capacity, "");
#endif
}

usize region_of_locale(const char *const locale, char *const out,
                       const usize capacity) noexcept {
  if (locale == nullptr)
    return say(out, capacity, "");
  // Subtags after the language, up to an encoding or modifier.
  usize at = 0;
  while (locale[at] != '\0' && locale[at] != '_' && locale[at] != '-' &&
         locale[at] != '.' && locale[at] != '@')
    ++at;
  char found[3] = {};
  while (locale[at] == '_' || locale[at] == '-') {
    const usize start = ++at;
    while (locale[at] != '\0' && locale[at] != '_' && locale[at] != '-' &&
           locale[at] != '.' && locale[at] != '@')
      ++at;
    const auto letter = [](const char c) {
      return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    if (at - start == 2 && letter(locale[start]) && letter(locale[start + 1]))
      for (usize i = 0; i < 2; ++i) {
        const char c = locale[start + i];
        found[i] = c >= 'a' && c <= 'z' ? nx::cast<char>(c - 'a' + 'A') : c;
      }
  }
  return say(out, capacity, found);
}

usize region(char *const out, const usize capacity) noexcept {
#if NX_HAS_WINDOWS_API
  wchar_t iso[8] = {};
  const GEOID nation = ::GetUserGeoID(GEOCLASS_NATION);
  if (nation != GEOID_NOT_AVAILABLE &&
      ::GetGeoInfoW(nation, GEO_ISO2, iso, 8, 0) == 3) {
    const char text[] = {'x', '-', nx::cast<char>(iso[0]),
                         nx::cast<char>(iso[1]), '\0'};
    return region_of_locale(text, out, capacity);
  }
  wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
  if (::GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) == 0)
    return say(out, capacity, "");
  char narrow[LOCALE_NAME_MAX_LENGTH] = {};
  for (usize i = 0; i + 1 < LOCALE_NAME_MAX_LENGTH && name[i] != 0; ++i)
    narrow[i] = name[i] < 128 ? nx::cast<char>(name[i]) : '?';
  return region_of_locale(narrow, out, capacity);
#elif NX_PLATFORM_ANDROID
  // Set when the user picks a language; the build's own locale otherwise.
  char locale[96] = {};
  if (android_property("persist.sys.locale", locale, sizeof(locale)) == 0)
    android_property("ro.product.locale", locale, sizeof(locale));
  return region_of_locale(locale, out, capacity);
#elif NX_PLATFORM_APPLE
  const CFLocaleRef locale = ::CFLocaleCopyCurrent();
  if (locale == nullptr)
    return say(out, capacity, "");
  char code[8] = {};
  const auto country = static_cast<CFStringRef>(
      ::CFLocaleGetValue(locale, kCFLocaleCountryCode));
  const bool read =
      country != nullptr &&
      ::CFStringGetCString(country, code, sizeof(code), kCFStringEncodingUTF8);
  ::CFRelease(locale);
  char name[16] = {};
  nx::format_to(name, "x-{}", nx::string_view(read ? code : ""));
  return region_of_locale(name, out, capacity);
#elif NX_PLATFORM_POSIX
  for (const char *const variable : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    const char *const value = ::getenv(variable);
    if (value != nullptr && value[0] != '\0')
      return region_of_locale(value, out, capacity);
  }
  return say(out, capacity, "");
#else
  return say(out, capacity, "");
#endif
}

usize cpu_brand(char *const out, const usize capacity) noexcept {
#if NX_ARCH_X64 || NX_ARCH_X86
  // The 48-byte brand string, CPUID leaves 0x80000002..4.
  u32 regs[4] = {};
  char brand[49] = {};
#if defined(NX_COMPILER_MSVC) && !defined(NX_COMPILER_CLANG)
  __cpuid(reinterpret_cast<int *>(regs), nx::cast<int>(0x80000000u));
#else
  __get_cpuid(0x80000000u, &regs[0], &regs[1], &regs[2], &regs[3]);
#endif
  if (regs[0] < 0x80000004u)
    return say(out, capacity, "");
  for (u32 leaf = 0; leaf < 3; ++leaf) {
#if defined(NX_COMPILER_MSVC) && !defined(NX_COMPILER_CLANG)
    __cpuid(reinterpret_cast<int *>(regs), nx::cast<int>(0x80000002u + leaf));
#else
    __get_cpuid(0x80000002u + leaf, &regs[0], &regs[1], &regs[2], &regs[3]);
#endif
    for (u32 i = 0; i < 4; ++i)
      for (u32 b = 0; b < 4; ++b)
        brand[leaf * 16 + i * 4 + b] =
            nx::cast<char>((regs[i] >> (b * 8)) & 0xffu);
  }
  const char *start = brand;
  while (*start == ' ')
    ++start;
  return say(out, capacity, start);
#elif NX_PLATFORM_APPLE
  char brand[128] = {};
  usize size = sizeof(brand);
  if (::sysctlbyname("machdep.cpu.brand_string", brand, &size, nullptr, 0) != 0)
    return say(out, capacity, "");
  return say(out, capacity, brand);
#elif NX_PLATFORM_ANDROID
  char hardware[64] = {};
  android_property("ro.hardware", hardware, sizeof(hardware));
  return say(out, capacity, hardware);
#else
  return say(out, capacity, "");
#endif
}

u32 process_id() noexcept {
#if NX_HAS_WINDOWS_API
  return nx::cast<u32>(::GetCurrentProcessId());
#elif NX_PLATFORM_POSIX
  return nx::cast<u32>(::getpid());
#else
  return 0;
#endif
}

std::optional<string> environment_variable(const string_view name) {
  if (name.empty())
    return std::nullopt;
#if NX_HAS_WINDOWS_API
  const int name_length = ::MultiByteToWideChar(
      CP_UTF8, 0, name.data(), nx::cast<int>(name.size()), nullptr, 0);
  if (name_length <= 0)
    return std::nullopt;
  vector<wchar_t> wide_name(nx::cast<usize>(name_length) + 1, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, name.data(), nx::cast<int>(name.size()),
                        wide_name.data(), name_length);
  vector<wchar_t> value;
  DWORD length = 0;
  // Another thread may lengthen the value between the two calls.
  for (DWORD room = 256;;) {
    value.resize(room);
    ::SetLastError(ERROR_SUCCESS);
    length = ::GetEnvironmentVariableW(wide_name.data(), value.data(), room);
    if (length < room)
      break;
    room = length;
  }
  // 0 is both an empty value and none at all.
  if (length == 0)
    return ::GetLastError() == ERROR_ENVVAR_NOT_FOUND
               ? std::nullopt
               : std::optional<string>(string());
  const int bytes =
      ::WideCharToMultiByte(CP_UTF8, 0, value.data(), nx::cast<int>(length),
                            nullptr, 0, nullptr, nullptr);
  string out;
  out.resize(nx::cast<usize>(bytes));
  ::WideCharToMultiByte(CP_UTF8, 0, value.data(), nx::cast<int>(length),
                        out.data(), bytes, nullptr, nullptr);
  return out;
#else
  const string key(name);
  const char *const value = ::getenv(key.c_str());
  return value != nullptr ? std::optional<string>(string(value)) : std::nullopt;
#endif
}

bool random_bytes(const std::span<u8> out) noexcept {
#if NX_HAS_WINDOWS_API
  u8 *at = out.data();
  usize left = out.size();
  while (left > 0) {
    const ULONG chunk = nx::cast<ULONG>(std::min<usize>(left, 1u << 30));
    if (!BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, at, chunk,
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
      return false;
    at += chunk;
    left -= chunk;
  }
  return true;
#elif NX_PLATFORM_APPLE
  ::arc4random_buf(out.data(), out.size());
  return true;
#elif NX_PLATFORM_POSIX
  // Not getrandom(): bionic has it only from API 28.
  const int file = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (file < 0)
    return false;
  usize done = 0;
  while (done < out.size()) {
    const ssize_t got = ::read(file, out.data() + done, out.size() - done);
    if (got <= 0) {
      if (got < 0 && errno == EINTR)
        continue;
      break;
    }
    done += nx::cast<usize>(got);
  }
  ::close(file);
  return done == out.size();
#else
  (void)out;
  return false;
#endif
}

usize executable_path(char *const out, const usize capacity) noexcept {
#if NX_HAS_WINDOWS_API
  char path[1024] = {};
  const DWORD length = ::GetModuleFileNameA(nullptr, path, sizeof(path));
  return say(out, capacity, path, nx::cast<usize>(length));
#elif NX_PLATFORM_APPLE
  char path[1024] = {};
  u32 size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) != 0)
    return say(out, capacity, "");
  return say(out, capacity, path);
#elif NX_PLATFORM_POSIX
  char path[1024] = {};
  const ssize_t length = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (length <= 0)
    return say(out, capacity, "");
  return say(out, capacity, path, nx::cast<usize>(length));
#else
  return say(out, capacity, "");
#endif
}
}

}
