#include "framework/nxtest.h"

#include "core/foundation/diagnostics/crash.h"
#include "core/foundation/diagnostics/log.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/platform/os.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/threading/sync.h"
#include "core/foundation/threading/thread.h"
#include "core/foundation/threading/thread_pool.h"

#include "fibers/fiber_test_support.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>

#if NX_HAS_WINDOWS_API
#include <windows.h>
#endif

namespace {

// ctest re-invokes this executable once per TEST_CASE (one process each),
// so a shared path with no process-unique component races across whatever
// else -j lets run concurrently - one process's remove_all() can delete
// another's report directory mid-test. See vfs_fixture.h's scratch_root
// for the same fix applied to a failure this actually caused in CI.
nx::string report_dir() {
  const char *tmp = std::getenv("TEMP");
  if (!tmp)
    tmp = std::getenv("TMP");
  if (!tmp)
    tmp = std::getenv("TMPDIR");
  if (!tmp)
    tmp = ".";
  return nx::fs::path::join(
      nx::string_view(tmp),
      nx::string_view("nx_crash_tests_" +
                      std::to_string(nx::os::process_id())));
}

const nx::string &installed_dir() {
  static const nx::string where = [] {
    nx::string path = report_dir();
    (void)nx::fs::remove_all(path);
    nx::crash::Config config;
    config.directory = path;
    config.application = "nxtest";
    config.version = "0.0.0-test";
    REQUIRE(nx::crash::install(config));
    return path;
  }();
  return where;
}

nx::string read_last() {
  const nx::string_view path = nx::crash::last_report();
  REQUIRE_FALSE(path.empty());
  const auto text = nx::fs::file_read_text(path);
  REQUIRE(text);
  return text.value();
}

#if NX_HAS_WINDOWS_API
/// Runs @p test_case in a process of its own, reporting into @p where; what it
/// exited with.
DWORD run_child(const char *const test_case, const nx::string &where) {
  REQUIRE(_putenv_s("NX_CRASH_CHILD_DIR", where.c_str()) == 0);
  char exe[MAX_PATH] = {};
  REQUIRE(::GetModuleFileNameA(nullptr, exe, MAX_PATH) != 0);
  std::string line =
      std::string("\"") + exe + "\" \"--test-case=" + test_case + "\"";
  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  REQUIRE(::CreateProcessA(nullptr, line.data(), nullptr, nullptr, FALSE, 0,
                           nullptr, nullptr, &startup, &process) != 0);
  (void)_putenv_s("NX_CRASH_CHILD_DIR", "");
  const DWORD waited = ::WaitForSingleObject(process.hProcess, 90000);
  CHECK(waited == WAIT_OBJECT_0);
  if (waited != WAIT_OBJECT_0)
    ::TerminateProcess(process.hProcess, 1);
  DWORD code = 0;
  ::GetExitCodeProcess(process.hProcess, &code);
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  return code;
}

NX_NO_INLINE void stuck_here() {
  for (;;)
    nx::this_thread::sleep_for(std::chrono::milliseconds(50));
}
#endif

} // namespace

#if NX_HAS_WINDOWS_API
// Run by the case after it, in a process of its own: it does not return.
TEST_CASE("crash: (child) faults on two fibers at once") {
  const char *const where = std::getenv("NX_CRASH_CHILD_DIR");
  if (where == nullptr)
    return;
  nx::crash::Config config;
  config.directory = where;
  config.application = "nxtest";
  REQUIRE(nx::crash::install(config));
  std::atomic<u32> ready{0};
  const auto fault = [&] {
    nx::fiber::stack_pool pool;
    fiber_test::run_on_fiber(pool, [&] {
      ready.fetch_add(1);
      while (ready.load() < 2) {
      }
      volatile int *const nowhere = nullptr;
      *nowhere = 1;
    });
  };
  nx::thread first(fault);
  nx::thread second(fault);
  first.join();
  second.join();
}

TEST_CASE("crash: faults on two fibers at once leave a whole report") {
  const nx::string where =
      nx::fs::path::join(report_dir(), nx::string_view("two_fibers"));
  (void)nx::fs::remove_all(where);
  REQUIRE(nx::fs::create_directories(where));
  CHECK(run_child("crash: (child) faults on two fibers at once", where) ==
        0xC0000005u);

  bool whole = false;
  const auto listed = nx::fs::directory_entries(where);
  REQUIRE(listed);
  for (const nx::fs::directory_entry &entry : listed.value()) {
    if (!entry.path.ends_with(".txt"))
      continue;
    const auto text = nx::fs::file_read_text(entry.path);
    REQUIRE(text);
    const nx::string_view report = text.value();
    whole =
        report.find("EXCEPTION_ACCESS_VIOLATION") != nx::string_view::npos &&
        report.find("application  : nxtest") != nx::string_view::npos &&
        report.find("--- registers ---") != nx::string_view::npos;
  }
  CHECK(whole);
  (void)nx::fs::remove_all(where);
}

// Run by the case after it, in a process of its own: it does not return.
TEST_CASE("crash: (child) a hang") {
  const char *const where = std::getenv("NX_CRASH_CHILD_DIR");
  if (where == nullptr)
    return;
  nx::crash::Config config;
  config.directory = where;
  config.application = "nxtest";
  REQUIRE(nx::crash::install(config));
  REQUIRE(nx::crash::start_hang_watch(300));
  nx::thread stuck([] {
    nx::this_thread::set_name("stuck-thread");
    stuck_here();
  });
  nx::crash::beat();
  stuck.join();
}

TEST_CASE("crash: a hang ends the process with a report of every thread") {
  const nx::string where =
      nx::fs::path::join(report_dir(), nx::string_view("a_hang"));
  (void)nx::fs::remove_all(where);
  REQUIRE(nx::fs::create_directories(where));
  CHECK(run_child("crash: (child) a hang", where) ==
        static_cast<DWORD>(nx::crash::HANG_EXIT_CODE));

  nx::string report;
  bool dump = false;
  const auto listed = nx::fs::directory_entries(where);
  REQUIRE(listed);
  for (const nx::fs::directory_entry &entry : listed.value()) {
    if (entry.path.ends_with(".dmp"))
      dump = true;
    if (entry.path.ends_with(".txt")) {
      const auto text = nx::fs::file_read_text(entry.path);
      REQUIRE(text);
      report = text.value();
    }
  }
  CHECK(dump);
  const nx::string_view text = report;
  CHECK(text.find("=== hang report ===") != nx::string_view::npos);
  CHECK(text.find("no frame for") != nx::string_view::npos);
  CHECK(text.find("application  : nxtest") != nx::string_view::npos);
  const usize stuck = text.find("stuck-thread");
  REQUIRE(stuck != nx::string_view::npos);
  // The stuck thread's stack reaches this executable's own code.
  CHECK(text.find(".exe+", text.find("--- backtrace ---", stuck)) !=
        nx::string_view::npos);
  (void)nx::fs::remove_all(where);
}
#endif

// A hang watch that went off would end this process, failing the run.
TEST_CASE("crash: a hang watch leaves alone what beats, what was never armed, "
          "and what is held") {
  (void)installed_dir();
  using namespace std::chrono_literals;
  REQUIRE(nx::crash::start_hang_watch(100));
  CHECK_FALSE(nx::crash::start_hang_watch(100));
  nx::this_thread::sleep_for(400ms);
  for (u32 i = 0; i < 30; ++i) {
    nx::crash::beat();
    nx::this_thread::sleep_for(15ms);
  }
  nx::crash::hold_hang_watch(true);
  nx::this_thread::sleep_for(400ms);
  nx::crash::beat();
  nx::crash::hold_hang_watch(false);
  nx::crash::beat();
  nx::crash::stop_hang_watch();
  nx::this_thread::sleep_for(200ms);
  REQUIRE(nx::crash::start_hang_watch(100));
  nx::crash::stop_hang_watch();
}

TEST_CASE("crash: a hang watch needs a timeout") {
  (void)installed_dir();
  CHECK_FALSE(nx::crash::start_hang_watch(0));
}

TEST_CASE("crash: install resolves a directory and creates it") {
  const nx::string &where = installed_dir();
  CHECK(nx::crash::installed());
  CHECK(nx::fs::exists(where));
  CHECK(nx::crash::directory() == where);
}

TEST_CASE("crash: a report carries the machine and the build it came from") {
  (void)installed_dir();
  REQUIRE(nx::crash::report({"a reason to write one", "and a detail"}));

  const nx::string text = read_last();
  CHECK(text.find("a reason to write one") != nx::string::npos);
  CHECK(text.find("and a detail") != nx::string::npos);
  CHECK(text.find("nxtest") != nx::string::npos);
  CHECK(text.find("=== nx2d") == nx::string::npos);
  CHECK(text.find("0.0.0-test") != nx::string::npos);

  CHECK(text.find(NX_PLATFORM_NAME) != nx::string::npos);
  CHECK(text.find(NX_ARCH_NAME) != nx::string::npos);
  CHECK(text.find(NX_COMPILER_NAME) != nx::string::npos);
  CHECK(text.find("cores        :") != nx::string::npos);
  CHECK(text.find("ram          :") != nx::string::npos);
  CHECK(text.find("uptime       :") != nx::string::npos);
  CHECK(text.find("os           :") != nx::string::npos);
}

TEST_CASE("crash: notes reach the report and can be replaced and cleared") {
  (void)installed_dir();
  nx::crash::note("scene", "the wrong one");
  nx::crash::note("scene", "battle.nxscene");
  nx::crash::note("doomed", "gone before it is written");
  nx::crash::clear_note("doomed");

  REQUIRE(nx::crash::report({"with notes"}));
  const nx::string text = read_last();
  CHECK(text.find("scene : battle.nxscene") != nx::string::npos);
  CHECK(text.find("the wrong one") == nx::string::npos);
  CHECK(text.find("doomed") == nx::string::npos);
  nx::crash::clear_note("scene");
}

TEST_CASE("crash: a value longer than a note slot is truncated, not refused") {
  (void)installed_dir();
  const nx::string huge(nx::crash::NOTE_VALUE_CAPACITY * 3, 'x');
  nx::crash::note("huge", huge);

  REQUIRE(nx::crash::report({"with a long note"}));
  const nx::string text = read_last();
  const usize at = text.find("huge : ");
  REQUIRE(at != nx::string::npos);
  const usize end = text.find('\n', at);
  REQUIRE(end != nx::string::npos);
  CHECK(end - at < nx::crash::NOTE_VALUE_CAPACITY + 16);
  CHECK(end - at > 64);
  nx::crash::clear_note("huge");
}

TEST_CASE("crash: notes are coherent while another thread replaces them") {
  (void)installed_dir();
  const nx::string first(192, 'a');
  const nx::string second(192, 'b');
  nx::crash::note("concurrent", first);

  std::atomic<bool> stop{false};
  nx::thread writer(
      [&] {
        bool alternate = false;
        while (!stop.load(std::memory_order_acquire)) {
          nx::crash::note("concurrent", alternate ? first : second);
          alternate = !alternate;
        }
      },
      {.name = "crash-note-test"});

  bool reports_written = true;
  bool snapshots_coherent = true;
  for (usize i = 0; i < 8 && reports_written; ++i) {
    reports_written = nx::crash::report({"while replacing a note"});
    if (!reports_written)
      break;
    const nx::string path(nx::crash::last_report());
    const auto read = nx::fs::file_read_text(path);
    if (!read) {
      snapshots_coherent = false;
      break;
    }
    const nx::string &text = read.value();
    const usize begin = text.find("concurrent : ");
    if (begin == nx::string::npos) {
      snapshots_coherent = false;
      break;
    }
    const usize value = begin + nx::string_view("concurrent : ").size();
    const usize end = text.find('\n', value);
    if (end == nx::string::npos) {
      snapshots_coherent = false;
      break;
    }
    const nx::string_view snapshot(text.data() + value, end - value);
    if (snapshot != first && snapshot != second) {
      snapshots_coherent = false;
      break;
    }
  }

  stop.store(true, std::memory_order_release);
  writer.join();
  nx::crash::clear_note("concurrent");
  CHECK(reports_written);
  CHECK(snapshots_coherent);
}

TEST_CASE("crash: the report ends with the tail of the log") {
  (void)installed_dir();
  nx::logi(nx::log::system, "crash test: a line that must survive");
  nx::log::flush();

  REQUIRE(nx::crash::report({"with a log tail"}));
  const nx::string text = read_last();
  CHECK(text.find("--- log (most recent") != nx::string::npos);
  CHECK(text.find("a line that must survive") != nx::string::npos);
}

TEST_CASE("crash: a record still queued for the log reaches the tail") {
  (void)installed_dir();
  nx::log::Config quiet;
  quiet.console = false;
  quiet.debugger = false;
  quiet.android = false;
  nx::log::init(quiet);
  // The drain job runs on the low-priority lane, behind this.
  nx::event release;
  nx::event busy;
  nx::thread_pool pool(nx::thread_pool::config{.thread_count = 1});
  pool.submit([&] {
    busy.set();
    release.wait();
  });
  busy.wait();
  nx::log::attach_drain_pool(pool);
  nx::logi(nx::log::system, "crash test: queued when the report is written");

  const bool reported = nx::crash::report({"with a queued record"});
  release.set();
  nx::log::detach_drain_pool(pool);
  REQUIRE(reported);
  CHECK(read_last().find("queued when the report is written") !=
        nx::string::npos);
}

TEST_CASE("crash: the tail keeps the newest records, not the oldest") {
  (void)installed_dir();
  for (usize i = 0; i < nx::crash::LOG_TAIL_CAPACITY + 40; ++i)
    nx::logi(nx::log::system, "crash test: filler {}", i);
  nx::logi(nx::log::system, "crash test: the very last one");
  nx::log::flush();

  REQUIRE(nx::crash::report({"with a wrapped tail"}));
  const nx::string text = read_last();
  CHECK(text.find("crash test: filler 0\n") == nx::string::npos);

  const usize last = text.find("the very last one");
  REQUIRE(last != nx::string::npos);
  char newest[64];
  const usize newest_filler = text.rfind(
      nx::format_to(newest, "filler {}", nx::crash::LOG_TAIL_CAPACITY + 39));
  REQUIRE(newest_filler != nx::string::npos);
  CHECK(newest_filler < last);
  CHECK(text.find('\n', last) + 1 == text.size());
}

TEST_CASE("crash: earlier reports are listed newest first") {
  (void)installed_dir();
  REQUIRE(nx::crash::report({"one"}));
  // File times can be a millisecond apart at best.
  nx::this_thread::sleep_for(std::chrono::milliseconds(20));
  REQUIRE(nx::crash::report({"two"}));

  const nx::vector<nx::string> found = nx::crash::pending_reports();
  REQUIRE(found.size() >= 2u);
  CHECK(nx::fs::path::filename(found[0].view()) ==
        nx::fs::path::filename(nx::crash::last_report()));
}

TEST_CASE("crash: reports are ordered by age, not by kind") {
  const nx::string &where = installed_dir();
  const nx::string older = nx::fs::path::join(where.view(), "zzz-older.txt");
  const nx::string newer = nx::fs::path::join(where.view(), "aaa-newer.txt");
  REQUIRE(nx::fs::file_write_text(nx::fs::path_view(older), "old").has_value());
  nx::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(nx::fs::file_write_text(nx::fs::path_view(newer), "new").has_value());

  const nx::vector<nx::string> found = nx::crash::pending_reports();
  const auto at = [&](const nx::string_view name) {
    return std::find_if(found.begin(), found.end(),
                        [&](const nx::string &path) {
                          return nx::fs::path::filename(path.view()) == name;
                        }) -
           found.begin();
  };
  CHECK(at("aaa-newer.txt") < at("zzz-older.txt"));
  CHECK(at("zzz-older.txt") < nx::cast<isize>(found.size()));
  (void)nx::fs::remove(nx::fs::path_view(older));
  (void)nx::fs::remove(nx::fs::path_view(newer));
}

TEST_CASE("crash: kind names the file, so a script fault is filed as one") {
  (void)installed_dir();
  REQUIRE(nx::crash::report({"the guest raised", "traceback", "script-fault"}));
  CHECK(nx::crash::last_report().find("script-fault") != nx::string_view::npos);
}

TEST_CASE("crash: bounded binary attachments are written beside the report") {
  (void)installed_dir();
  constexpr u8 payload[] = {0x4e, 0x58, 0x47, 0x50, 0x55};
  const nx::crash::Attachment attachment{"vendor.bin", payload};
  REQUIRE(nx::crash::report(
      {"device lost", "fault details", "gpu-crash", {&attachment, 1}}));

  const nx::string report(nx::crash::last_report());
  REQUIRE(report.ends_with(".txt"));
  nx::string sidecar(report.view().substr(0, report.size() - 4));
  sidecar.append("-vendor.bin");
  const auto bytes = nx::fs::file_read(sidecar);
  REQUIRE(bytes);
  CHECK(bytes->size() == nx::array_size(payload));
  CHECK(std::equal(bytes->begin(), bytes->end(), payload));

  const nx::string text = read_last();
  CHECK(text.find("vendor.bin") != nx::string::npos);
  CHECK(text.find(sidecar) != nx::string::npos);
  (void)nx::fs::remove(sidecar);
}

TEST_CASE("crash: attachment names cannot escape the report directory") {
  (void)installed_dir();
  constexpr u8 payload[] = {1};
  const nx::crash::Attachment attachment{"../escaped.bin", payload};
  REQUIRE(
      nx::crash::report({"device lost", {}, "gpu-crash", {&attachment, 1}}));
  const nx::string text = read_last();
  CHECK(text.find("rejected invalid attachment name") != nx::string::npos);
}
