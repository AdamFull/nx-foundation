#include "core/foundation/diagnostics/profiler.h"
#include "core/foundation/fibers/scheduler.h"
#include "core/foundation/strings/utf8_string_view.h"

#include "framework/nxtest.h"

#if NX_PROFILE_HOOKS

namespace {

struct Seen {
  int begins = 0;
  int text_begins = 0;
  int ends = 0;
  int marks = 0;
  const nx::profile::zone_site *site = nullptr;
  const char *site_name = nullptr;
  u32 site_color = 0;
  nx::string_view text;
  const char *mark_name = nullptr;
  const char *plot_name = nullptr;
  f64 plot_value = 0.0;
} g_seen;

std::atomic<u32> g_switches{0};
std::atomic<u32> g_job_fibers{0};

constexpr nx::profile::hooks HOOKS{
    .zone_begin =
        [](const nx::profile::zone_site &site) noexcept {
          ++g_seen.begins;
          g_seen.site = &site;
          g_seen.site_name = site.name;
          g_seen.site_color = site.color;
        },
    .zone_begin_text =
        [](const char *const text, const usize size) noexcept {
          ++g_seen.text_begins;
          g_seen.text = nx::string_view(text, size);
        },
    .zone_end = []() noexcept { ++g_seen.ends; },
    .frame_mark =
        [](const char *const name) noexcept {
          ++g_seen.marks;
          g_seen.mark_name = name;
        },
    .plot =
        [](const char *const name, const f64 value) noexcept {
          g_seen.plot_name = name;
          g_seen.plot_value = value;
        },
    .fiber_switch =
        [](const u32 fiber) noexcept {
          g_switches.fetch_add(1, std::memory_order_relaxed);
          if (fiber != 0)
            g_job_fibers.fetch_add(1, std::memory_order_relaxed);
        },
};

struct Installed {
  Installed() {
    g_seen = {};
    nx::profile::set_hooks(&HOOKS);
  }
  ~Installed() { nx::profile::set_hooks(nullptr); }
};

} // namespace

TEST_CASE("profile macros: nothing is called without hooks") {
  g_seen = {};
  nx::profile::set_hooks(nullptr);
  {
    NX_PROFILE_ZONE("quiet");
    NX_PROFILE_ZONE_TEXT("quiet", 5);
    NX_PROFILE_FRAME_MARK();
    NX_PROFILE_PLOT("quiet", 1);
  }
  CHECK(g_seen.begins == 0);
  CHECK(g_seen.text_begins == 0);
  CHECK(g_seen.ends == 0);
  CHECK(g_seen.marks == 0);
  CHECK(g_seen.plot_name == nullptr);
}

TEST_CASE("profile macros: a plot's value is not worked out without hooks") {
  nx::profile::set_hooks(nullptr);
  int evaluated = 0;
  NX_PROFILE_PLOT("lazy", ++evaluated);
  CHECK(evaluated == 0);
}

TEST_CASE("profile macros: reach the installed hooks") {
  const Installed installed;
  {
    NX_PROFILE_ZONE_COLOR("coloured", 0x123456);
    CHECK(g_seen.begins == 1);
    CHECK(nx::string_view(g_seen.site_name) == "coloured");
    CHECK(g_seen.site_color == 0x123456u);
    CHECK(g_seen.ends == 0);
  }
  CHECK(g_seen.ends == 1);
  {
    const nx::string_view name = "system::run";
    NX_PROFILE_ZONE_TEXT(name.data(), name.size());
    CHECK(g_seen.text_begins == 1);
    CHECK(g_seen.text == "system::run");
  }
  CHECK(g_seen.ends == 2);
  NX_PROFILE_FRAME_MARK();
  CHECK(g_seen.marks == 1);
  CHECK(g_seen.mark_name == nullptr);
  NX_PROFILE_FRAME_MARK_NAMED("render");
  CHECK(nx::string_view(g_seen.mark_name) == "render");
  NX_PROFILE_PLOT("textures", 7);
  CHECK(nx::string_view(g_seen.plot_name) == "textures");
  CHECK(g_seen.plot_value == 7.0);
}

TEST_CASE("profile macros: a zone open when hooks go still ends") {
  const Installed installed;
  {
    NX_PROFILE_ZONE("spans the stop");
    nx::profile::set_hooks(nullptr);
  }
  CHECK(g_seen.begins == 1);
  CHECK(g_seen.ends == 1);
}

TEST_CASE(
    "profile macros: a zone begun before the hooks does not end in them") {
  nx::profile::set_hooks(nullptr);
  g_seen = {};
  {
    NX_PROFILE_ZONE("spans the start");
    nx::profile::set_hooks(&HOOKS);
  }
  nx::profile::set_hooks(nullptr);
  CHECK(g_seen.begins == 0);
  CHECK(g_seen.ends == 0);
}

TEST_CASE("profile macros: a site is one object per zone") {
  const Installed installed;
  const nx::profile::zone_site *first = nullptr;
  for (int i = 0; i < 2; ++i) {
    NX_PROFILE_ZONE("repeated");
    if (first != nullptr)
      CHECK(g_seen.site == first);
    first = g_seen.site;
  }
  CHECK(g_seen.begins == 2);
  CHECK(first != nullptr);
  {
    NX_PROFILE_ZONE("another");
  }
  CHECK(g_seen.site != first);
  CHECK(alignof(nx::profile::zone_site) >= 8);
}

TEST_CASE("profile hooks: the scheduler reports the fibers it switches to") {
  g_switches = 0;
  g_job_fibers = 0;
  nx::fiber::scheduler scheduler({.worker_count = 2, .blocking_threads = 1});
  const Installed installed;
  const nx::fiber::job job{[](void *) { nx::fiber::run_blocking([] {}); },
                           nullptr};
  nx::fiber::counter done;
  scheduler.run({&job, 1}, &done);
  scheduler.wait(done);
  nx::profile::set_hooks(nullptr);
  CHECK(g_switches.load() > 0);
  CHECK(g_job_fibers.load() > 0);
}

TEST_CASE("profile macros: a thread's name waits for a recorder") {
  nx::profile::set_thread_name(nullptr);
  CHECK(nx::profile::thread_name() == nullptr);
  NX_PROFILE_SET_THREAD_NAME("worker");
  CHECK(nx::string_view(nx::profile::thread_name()) == "worker");
  nx::profile::set_thread_name(nullptr);
}

#endif
