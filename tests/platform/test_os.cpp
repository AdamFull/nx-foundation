#include "framework/nxtest.h"

#include "core/foundation/platform/os.h"
#include "core/foundation/threading/thread.h"

#if NX_HAS_WINDOWS_API
#include <windows.h>
#else
#include <stdlib.h>
#endif

#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace vmem = nx::vmem;

TEST_CASE("vmem: page size and allocation granularity are sane powers of two") {
  const usize page = vmem::get_page_size();
  const usize gran = vmem::get_allocation_granularity();

  CHECK(page >= 512);
  CHECK(page <= 1u << 20);
  CHECK(nx::is_pow2(page));

  CHECK(gran >= page);
  CHECK(nx::is_pow2(gran));
  CHECK(gran % page == 0);

  CHECK(vmem::get_page_size() == page);
  CHECK(vmem::get_allocation_granularity() == gran);
}

TEST_CASE("vmem: reserve and release round-trip") {
  const usize gran = vmem::get_allocation_granularity();
  const usize bytes = gran * 4;

  void *base = nullptr;
  REQUIRE(vmem::reserve(&base, bytes));
  REQUIRE(base != nullptr);
  CHECK(reinterpret_cast<uintptr_t>(base) % gran == 0);

  CHECK(vmem::release(base, bytes));

  CHECK_FALSE(vmem::release(nullptr, bytes));
}

TEST_CASE("vmem: committed pages are readable and writable") {
  const usize page = vmem::get_page_size();
  const usize gran = vmem::get_allocation_granularity();
  const usize bytes = nx::max(gran * 2, page * 4);

  void *base = nullptr;
  REQUIRE(vmem::reserve(&base, bytes));
  REQUIRE(vmem::commit(base, page * 2));

  auto *p = static_cast<u8 *>(base);
  for (usize i = 0; i < page * 2; ++i)
    p[i] = static_cast<u8>(i);
  for (usize i = 0; i < page * 2; ++i)
    REQUIRE(p[i] == static_cast<u8>(i));

  REQUIRE(vmem::commit(p + page * 2, page));
  std::memset(p + page * 2, 0xAB, page);
  CHECK(p[page * 2] == 0xAB);
  CHECK(p[page * 3 - 1] == 0xAB);

  CHECK(vmem::commit(base, page));

  CHECK(vmem::release(base, bytes));
}

TEST_CASE("vmem: a reservation can be grown one page at a time") {
  const usize page = vmem::get_page_size();
  const usize gran = vmem::get_allocation_granularity();
  const usize reserve_bytes = nx::max(gran, page * 16);

  void *base = nullptr;
  REQUIRE(vmem::reserve(&base, reserve_bytes));
  auto *p = static_cast<u8 *>(base);

  const usize pages = reserve_bytes / page;
  for (usize i = 0; i < pages; ++i) {
    REQUIRE(vmem::commit(p + i * page, page));
    p[i * page] = static_cast<u8>(i);
    p[i * page + page - 1] = static_cast<u8>(i);
  }
  for (usize i = 0; i < pages; ++i) {
    REQUIRE(p[i * page] == static_cast<u8>(i));
    REQUIRE(p[i * page + page - 1] == static_cast<u8>(i));
  }

  CHECK(vmem::release(base, reserve_bytes));
}

TEST_CASE("vmem: protect changes access on a committed range") {
  const usize page = vmem::get_page_size();
  const usize gran = vmem::get_allocation_granularity();

  void *base = nullptr;
  REQUIRE(vmem::reserve(&base, gran));
  REQUIRE(vmem::commit(base, page));

  auto *p = static_cast<u8 *>(base);
  p[0] = 42;

  CHECK(vmem::protect(base, page, vmem::MemProtFlag::Read));
  CHECK(p[0] == 42);

  CHECK(vmem::protect(base, page,
                      vmem::MemProtFlag::Read | vmem::MemProtFlag::Write));
  p[0] = 43;
  CHECK(p[0] == 43);

  CHECK(vmem::release(base, gran));
}

TEST_CASE("vmem: MemProtFlags compose as a flag set") {
  const vmem::MemProtFlags rw =
      vmem::MemProtFlag::Read | vmem::MemProtFlag::Write;
  CHECK(rw.has(vmem::MemProtFlag::Read));
  CHECK(rw.has(vmem::MemProtFlag::Write));
  CHECK_FALSE(rw.has(vmem::MemProtFlag::Exec));
  CHECK(rw.has_any(vmem::MemProtFlag::Write));
  CHECK(rw.has_all(vmem::MemProtFlag::Read | vmem::MemProtFlag::Write));

  const vmem::MemProtFlags none{vmem::MemProtFlag::None};
  CHECK(none.any());
  CHECK_FALSE(none.has(vmem::MemProtFlag::Read));

  vmem::MemProtFlags f;
  CHECK(f.none());
  f.set(vmem::MemProtFlag::Exec);
  CHECK(f.has(vmem::MemProtFlag::Exec));
  f.clear(vmem::MemProtFlag::Exec);
  CHECK(f.none());
}

TEST_CASE("vmem: a zero-byte reservation does not hand back a usable range") {
  void *base = nullptr;
  if (vmem::reserve(&base, 0)) {
    CHECK(vmem::release(base, 0));
  } else {
    CHECK(base == nullptr);
  }
}

TEST_CASE("os: an environment variable is read as UTF-8, empty when set "
          "empty, and not at all when unset") {
  constexpr const char *NAME = "NX_TEST_ENVIRONMENT_UTF8";
  // "звук ☃", set through the platform's own interface as a launcher would.
  const nx::string_view utf8 = "\xD0\xB7\xD0\xB2\xD1\x83\xD0\xBA \xE2\x98\x83";
  const auto set = [&](const char *value) {
#if NX_HAS_WINDOWS_API
    const wchar_t *const wide = value == nullptr ? nullptr
                                : value[0] == '\0'
                                    ? L""
                                    : L"\u0437\u0432\u0443\u043a \u2603";
    CHECK(::SetEnvironmentVariableW(L"NX_TEST_ENVIRONMENT_UTF8", wide) != 0);
#else
    if (value == nullptr)
      CHECK(::unsetenv(NAME) == 0);
    else
      CHECK(::setenv(NAME, value, 1) == 0);
#endif
  };
  set(nx::string(utf8).c_str());
  const std::optional<nx::string> read = nx::os::environment_variable(NAME);
  REQUIRE(read.has_value());
  CHECK(read->view() == utf8);
  set("");
  const std::optional<nx::string> empty = nx::os::environment_variable(NAME);
  REQUIRE(empty.has_value());
  CHECK(empty->empty());
  set(nullptr);
  CHECK_FALSE(nx::os::environment_variable(NAME).has_value());
  CHECK_FALSE(nx::os::environment_variable("").has_value());
}

TEST_CASE("os: mem_get_stats reports plausible numbers") {
  const nx::os::MemStats s = nx::os::mem_get_stats();

  CHECK(s.rss > 0);
  CHECK(s.peak_rss >= s.rss);
  CHECK(s.rss < (u64{1} << 40));
  CHECK(s.peak_rss < (u64{1} << 40));
}

TEST_CASE("os: rss grows after a large committed allocation") {
  const nx::os::MemStats before = nx::os::mem_get_stats();

  constexpr usize bytes = 32u << 20;
  std::vector<u8> big(bytes);
  const usize page = vmem::get_page_size();
  for (usize i = 0; i < bytes; i += page)
    big[i] = static_cast<u8>(i);

  const nx::os::MemStats after = nx::os::mem_get_stats();
  CHECK(after.rss > before.rss);
  CHECK(after.peak_rss >= after.rss);
  CHECK(after.peak_rss >= before.peak_rss);

  CHECK(big[0] == 0);
}

TEST_CASE("os: ticks_ns is monotonic and advances") {
  const u64 a = nx::os::ticks_ns();
  CHECK(a > 0);

  u64 prev = a;
  for (int i = 0; i < 10000; ++i) {
    const u64 now = nx::os::ticks_ns();
    REQUIRE(now >= prev);
    prev = now;
  }

  const u64 start = nx::os::ticks_ns();
  nx::this_thread::sleep_for(50ms);
  const u64 elapsed = nx::os::ticks_ns() - start;
  CHECK(elapsed >= 20ull * 1000 * 1000);
  CHECK(elapsed < 10ull * 1000000000ull);
}

TEST_CASE("os: ticks_ms is monotonic and agrees with ticks_ns") {
  const u64 a = nx::os::ticks_ms();
  u64 prev = a;
  for (int i = 0; i < 2000; ++i) {
    const u64 now = nx::os::ticks_ms();
    REQUIRE(now >= prev);
    prev = now;
  }

  const u64 ns_start = nx::os::ticks_ns();
  const u64 ms_start = nx::os::ticks_ms();
  nx::this_thread::sleep_for(60ms);
  const u64 ns_delta = nx::os::ticks_ns() - ns_start;
  const u64 ms_delta = nx::os::ticks_ms() - ms_start;

  CHECK(ms_delta >= 20);
  CHECK(ms_delta < 10000);
  const u64 ns_as_ms = ns_delta / 1000000ull;
  const u64 diff =
      ns_as_ms > ms_delta ? ns_as_ms - ms_delta : ms_delta - ns_as_ms;
  CHECK(diff <= 50);
}

TEST_CASE("os: the tick clocks are consistent across threads") {
  const u64 main_before = nx::os::ticks_ns();
  u64 from_thread = 0;
  nx::thread t([&] { from_thread = nx::os::ticks_ns(); });
  t.join();
  const u64 main_after = nx::os::ticks_ns();

  CHECK(from_thread >= main_before);
  CHECK(from_thread <= main_after);
}

TEST_CASE("os: system_ram_mb reports a plausible amount") {
  const u32 ram = nx::os::system_ram_mb();
  if (ram != 0) {
    CHECK(ram >= 256);
    CHECK(ram <= 16u * 1024u * 1024u);
  }
  CHECK(nx::os::system_ram_mb() == ram);
}

TEST_CASE("os: random_bytes fills every byte, differently each time") {
  std::array<u8, 64> first{};
  std::array<u8, 64> second{};
  REQUIRE(nx::os::random_bytes(first));
  REQUIRE(nx::os::random_bytes(second));
  CHECK(first != second);
  // A byte left unwritten stays zero; a random one is zero one time in 256.
  usize zeros = 0;
  for (const u8 byte : first)
    zeros += byte == 0 ? 1u : 0u;
  CHECK(zeros < 16u);
  std::array<u8, 4096> large{};
  REQUIRE(nx::os::random_bytes(large));
  usize tail_zeros = 0;
  for (usize i = large.size() - 64; i < large.size(); ++i)
    tail_zeros += large[i] == 0 ? 1u : 0u;
  CHECK(tail_zeros < 16u);
  CHECK(nx::os::random_bytes({}));
}

TEST_CASE("os: a locale's region is its two-letter country") {
  const auto of = [](const char *const locale) {
    char out[8] = {};
    nx::os::region_of_locale(locale, out, sizeof(out));
    return std::string(out);
  };
  CHECK(of("en_US.UTF-8") == "US");
  CHECK(of("en-GB") == "GB");
  CHECK(of("ru_RU") == "RU");
  CHECK(of("zh-Hans-CN") == "CN");
  CHECK(of("de_de@euro") == "DE");
  CHECK(of("ja") == "");
  CHECK(of("C") == "");
  CHECK(of("C.UTF-8") == "");
  CHECK(of("es-419") == "");
  CHECK(of("") == "");
  CHECK(of(nullptr) == "");
  CHECK(of("en-U1") == "");
  char small[2] = {};
  CHECK(nx::os::region_of_locale("en_US", small, sizeof(small)) == 1u);
}

TEST_CASE("os: region is two upper-case letters or nothing") {
  char out[8] = {};
  const usize size = nx::os::region(out, sizeof(out));
  CHECK((size == 0 || size == 2));
  for (usize i = 0; i < size; ++i)
    CHECK((out[i] >= 'A' && out[i] <= 'Z'));
}
