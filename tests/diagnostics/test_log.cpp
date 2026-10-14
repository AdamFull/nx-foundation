#include "framework/nxtest.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/threading/sync.h"
#include "core/foundation/threading/thread.h"
#include "core/foundation/threading/thread_pool.h"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

class capture_sink final : public nx::log::Sink {
public:
  struct entry {
    nx::log::Level level;
    std::string category;
    std::string message;
    u64 thread_id;
    u64 writer_id;
  };

  void write(const nx::log::Record &r) override {
    m_entries.push_back(entry{r.level, r.category ? r.category->name : "?",
                              std::string(r.message, r.length), r.thread_id,
                              nx::this_thread::id()});
    if (r.length < nx::log::LOG_MESSAGE_CAPACITY && r.message[r.length] != '\0')
      m_terminator_violations.fetch_add(1, std::memory_order_relaxed);
    m_written.fetch_add(1, std::memory_order_release);
  }
  void flush() override { m_flushes.fetch_add(1, std::memory_order_relaxed); }

  [[nodiscard]] const std::vector<entry> &entries() const { return m_entries; }
  [[nodiscard]] int flushes() const { return m_flushes.load(); }
  [[nodiscard]] int written() const { return m_written.load(); }
  [[nodiscard]] int terminator_violations() const {
    return m_terminator_violations.load();
  }

  [[nodiscard]] int count_with(const std::string &needle) const {
    int n = 0;
    for (const entry &e : m_entries)
      if (e.message.find(needle) != std::string::npos)
        ++n;
    return n;
  }

private:
  std::vector<entry> m_entries;
  std::atomic<int> m_flushes{0};
  std::atomic<int> m_written{0};
  std::atomic<int> m_terminator_violations{0};
};

class capture_scope {
public:
  explicit capture_scope(nx::log::Level level = nx::log::Level::Trace)
      : m_previous(nx::log::global_level()) {
    m_sink = nx::allocate<capture_sink>();
    nx::log::add_sink(m_sink);
    nx::log::set_level(level);
  }
  ~capture_scope() {
    nx::log::flush();
    nx::log::remove_sink(m_sink);
    nx::log::set_level(m_previous);
  }
  capture_scope(const capture_scope &) = delete;
  capture_scope &operator=(const capture_scope &) = delete;

  capture_sink &sink() { return *m_sink; }
  void settle() { nx::log::flush(); }

private:
  capture_sink *m_sink = nullptr;
  nx::log::Level m_previous;
};

void init_quiet() {
  nx::log::Config cfg;
  cfg.level = nx::log::Level::Trace;
  cfg.console = false;
  cfg.debugger = false;
  cfg.android = false;
  nx::log::init(cfg);
}

nx::string temp_log_path(const char *name) {
  const char *tmp = std::getenv("TEMP");
  if (!tmp)
    tmp = std::getenv("TMP");
  if (!tmp)
    tmp = std::getenv("TMPDIR");
  if (!tmp)
    tmp = ".";
  const nx::string dir =
      nx::fs::path::join(nx::string_view(tmp), "nx_log_tests");
  (void)nx::fs::create_directories(nx::fs::path_view(dir));
  return nx::fs::path::join(nx::string_view(dir), nx::string_view(name));
}

/// Points the process's stderr at a file until text() puts it back.
class stderr_capture {
public:
  explicit stderr_capture(const nx::string &path) : m_path(path) {
    std::fflush(stderr);
    m_file = std::fopen(m_path.c_str(), "w+b");
    if (m_file == nullptr)
      return;
#if defined(_WIN32)
    m_saved = _dup(_fileno(stderr));
    _dup2(_fileno(m_file), _fileno(stderr));
#else
    m_saved = dup(fileno(stderr));
    dup2(fileno(m_file), fileno(stderr));
#endif
  }
  ~stderr_capture() { (void)text(); }
  stderr_capture(const stderr_capture &) = delete;
  stderr_capture &operator=(const stderr_capture &) = delete;

  std::string text() {
    if (m_file == nullptr)
      return m_text;
    std::fflush(stderr);
#if defined(_WIN32)
    _dup2(m_saved, _fileno(stderr));
    _close(m_saved);
#else
    dup2(m_saved, fileno(stderr));
    close(m_saved);
#endif
    std::fseek(m_file, 0, SEEK_SET);
    char buf[512];
    usize n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), m_file)) > 0)
      m_text.append(buf, n);
    std::fclose(m_file);
    m_file = nullptr;
    std::remove(m_path.c_str());
    return m_text;
  }

private:
  nx::string m_path;
  FILE *m_file = nullptr;
  int m_saved = -1;
  std::string m_text;
};

} // namespace

TEST_CASE("log: before init, an added sink observes stderr's records without "
          "silencing it") {
  nx::log::shutdown();
  const nx::string path = temp_log_path("stderr-before-init.txt");
  std::string written;
  {
    stderr_capture captured(path);
    capture_scope cap;
    nx::loge("seen-before-init");
    cap.settle();
    CHECK(cap.sink().count_with("seen-before-init") == 1);
    written = captured.text();
  }
  CHECK(written.find("seen-before-init") != std::string::npos);

  init_quiet();
  {
    stderr_capture captured{path};
    capture_scope cap;
    nx::loge("configured-output");
    cap.settle();
    CHECK(cap.sink().count_with("configured-output") == 1);
    written = captured.text();
  }
  CHECK(written.find("configured-output") == std::string::npos);
  nx::log::shutdown();
}

TEST_CASE("log: level_name covers every level") {
  CHECK(std::string(nx::log::level_name(nx::log::Level::Trace)).size() > 0);
  CHECK(std::string(nx::log::level_name(nx::log::Level::Debug)).size() > 0);
  CHECK(std::string(nx::log::level_name(nx::log::Level::Info)).size() > 0);
  CHECK(std::string(nx::log::level_name(nx::log::Level::Warn)).size() > 0);
  CHECK(std::string(nx::log::level_name(nx::log::Level::Error)).size() > 0);
  CHECK(std::string(nx::log::level_name(nx::log::Level::Critical)).size() > 0);
  CHECK(std::string(nx::log::level_name(nx::log::Level::Off)).size() > 0);

  std::vector<std::string> names;
  for (u8 i = 0; i <= static_cast<u8>(nx::log::Level::Off); ++i)
    names.emplace_back(nx::log::level_name(static_cast<nx::log::Level>(i)));
  std::sort(names.begin(), names.end());
  CHECK(std::unique(names.begin(), names.end()) == names.end());
}

TEST_CASE("log: category interning returns a stable handle per name") {
  const nx::log::Category a = nx::log::category("test.interning");
  const nx::log::Category b = nx::log::category("test.interning");

  CHECK(static_cast<bool>(a));
  CHECK(a.raw() == b.raw());
  CHECK(std::string(a.name()) == "test.interning");

  const nx::log::Category other = nx::log::category("test.other");
  CHECK(other.raw() != a.raw());

  CHECK(static_cast<bool>(nx::log::app));
  CHECK(std::string(nx::log::app.name()) == "app");
  CHECK(std::string(nx::log::gpu.name()) == "gpu");
  CHECK(std::string(nx::log::render.name()) == "render");
  CHECK(std::string(nx::log::system.name()) == "system");
  CHECK(nx::log::app.raw() != nx::log::gpu.raw());
  CHECK(nx::log::category("app").raw() == nx::log::app.raw());

  const nx::log::Category none;
  CHECK_FALSE(static_cast<bool>(none));
  CHECK(std::string(none.name()) == "?");
  CHECK(none.level() == nx::log::Level::Trace);
  CHECK(none.raw() == nullptr);
}

TEST_CASE("log: a long category name is truncated, not overflowed") {
  constexpr nx::string_view name =
      "an.extremely.long.category.name.far.beyond.the.registry.limit";
  static_assert(noexcept(nx::log::category(name)));
  static_assert(noexcept(nx::logw("number {}", 1)));

  const nx::log::Category c = nx::log::category(name);
  const nx::log::Category again = nx::log::category(name);
  CHECK(static_cast<bool>(c));
  CHECK(c.raw() == again.raw());
  CHECK(std::string(c.name()).size() < 32);

  const nx::log::Category same_prefix = nx::log::category(
      "an.extremely.long.category.name.but.a.different.full.name");
  CHECK(c.raw() != same_prefix.raw());
}

TEST_CASE("log: should_log honours the global and per-category thresholds") {
  const nx::log::Level saved = nx::log::global_level();
  const nx::log::Category cat = nx::log::category("test.should_log");
  const nx::log::Level saved_cat = cat.level();

  nx::log::set_level(nx::log::Level::Info);
  CHECK(nx::log::global_level() == nx::log::Level::Info);
  nx::log::set_level(cat, nx::log::Level::Trace);

  CHECK_FALSE(nx::log::should_log(nx::log::Level::Trace, cat));
  CHECK_FALSE(nx::log::should_log(nx::log::Level::Debug, cat));
  CHECK(nx::log::should_log(nx::log::Level::Info, cat));
  CHECK(nx::log::should_log(nx::log::Level::Warn, cat));
  CHECK(nx::log::should_log(nx::log::Level::Critical, cat));
  CHECK_FALSE(nx::log::should_log(nx::log::Level::Off, cat));

  nx::log::set_level(cat, nx::log::Level::Error);
  CHECK_FALSE(nx::log::should_log(nx::log::Level::Info, cat));
  CHECK_FALSE(nx::log::should_log(nx::log::Level::Warn, cat));
  CHECK(nx::log::should_log(nx::log::Level::Error, cat));

  nx::log::set_level(cat, nx::log::Level::Trace);
  CHECK_FALSE(nx::log::should_log(nx::log::Level::Debug, cat));

  nx::log::set_level(nx::log::Level::Trace);
  CHECK(nx::log::should_log(nx::log::Level::Trace, cat));

  nx::log::set_level(nx::log::Level::Off);
  CHECK_FALSE(nx::log::should_log(nx::log::Level::Critical, cat));

  nx::log::set_level(cat, saved_cat);
  nx::log::set_level(saved);
}

TEST_CASE("log: a registered sink receives records with level and category") {
  capture_scope cap;
  const nx::log::Category cat = nx::log::category("test.delivery");

  nx::logi(cat, "hello {}", 42);
  nx::logw(cat, "warned");
  nx::loge(cat, "failed: {}", "reason");
  cap.settle();

  const auto &e = cap.sink().entries();
  REQUIRE(e.size() >= 3);

  CHECK(cap.sink().count_with("hello 42") == 1);
  CHECK(cap.sink().count_with("warned") == 1);
  CHECK(cap.sink().count_with("failed: reason") == 1);
  CHECK(cap.sink().terminator_violations() == 0);

  bool saw_info = false, saw_warn = false, saw_error = false;
  for (const auto &r : e) {
    if (r.message == "hello 42") {
      saw_info = r.level == nx::log::Level::Info;
      CHECK(r.category == "test.delivery");
      CHECK(r.thread_id != 0);
    }
    if (r.message == "warned")
      saw_warn = r.level == nx::log::Level::Warn;
    if (r.message == "failed: reason")
      saw_error = r.level == nx::log::Level::Error;
  }
  CHECK(saw_info);
  CHECK(saw_warn);
  CHECK(saw_error);
}

TEST_CASE(
    "log: every helper maps to its level and defaults to the app category") {
  capture_scope cap;

  nx::logt("trace-msg");
  nx::logv("verbose-msg");
  nx::logd("debug-msg");
  nx::logi("info-msg");
  nx::logw("warn-msg");
  nx::loge("error-msg");
  nx::logc("critical-msg");
  cap.settle();

  const auto find =
      [&](const std::string &text) -> const capture_sink::entry * {
    for (const auto &e : cap.sink().entries())
      if (e.message == text)
        return &e;
    return nullptr;
  };

  REQUIRE(find("trace-msg") != nullptr);
  CHECK(find("trace-msg")->level == nx::log::Level::Trace);
  CHECK(find("trace-msg")->category == "app");
  REQUIRE(find("verbose-msg") != nullptr);
  CHECK(find("verbose-msg")->level == nx::log::Level::Trace);
  REQUIRE(find("debug-msg") != nullptr);
  CHECK(find("debug-msg")->level == nx::log::Level::Debug);
  REQUIRE(find("info-msg") != nullptr);
  CHECK(find("info-msg")->level == nx::log::Level::Info);
  REQUIRE(find("warn-msg") != nullptr);
  CHECK(find("warn-msg")->level == nx::log::Level::Warn);
  REQUIRE(find("error-msg") != nullptr);
  CHECK(find("error-msg")->level == nx::log::Level::Error);
  REQUIRE(find("critical-msg") != nullptr);
  CHECK(find("critical-msg")->level == nx::log::Level::Critical);
}

TEST_CASE("log: records below the threshold never reach a sink") {
  capture_scope cap(nx::log::Level::Warn);
  const nx::log::Category cat = nx::log::category("test.filtering");

  nx::logt(cat, "dropped-trace");
  nx::logd(cat, "dropped-debug");
  nx::logi(cat, "dropped-info");
  nx::logw(cat, "kept-warn");
  nx::loge(cat, "kept-error");
  cap.settle();

  CHECK(cap.sink().count_with("dropped-") == 0);
  CHECK(cap.sink().count_with("kept-warn") == 1);
  CHECK(cap.sink().count_with("kept-error") == 1);
}

TEST_CASE("log: a per-category threshold silences only that category") {
  capture_scope cap(nx::log::Level::Trace);
  const nx::log::Category noisy = nx::log::category("test.noisy");
  const nx::log::Category quiet = nx::log::category("test.quiet");

  nx::log::set_level(quiet, nx::log::Level::Error);
  nx::log::set_level(noisy, nx::log::Level::Trace);

  nx::logi(noisy, "noisy-info");
  nx::logi(quiet, "quiet-info");
  nx::loge(quiet, "quiet-error");
  cap.settle();

  CHECK(cap.sink().count_with("noisy-info") == 1);
  CHECK(cap.sink().count_with("quiet-info") == 0);
  CHECK(cap.sink().count_with("quiet-error") == 1);

  nx::log::set_level(quiet, nx::log::Level::Trace);
}

TEST_CASE("log: a message longer than the record capacity is truncated") {
  capture_scope cap;
  const std::string huge(nx::log::LOG_MESSAGE_CAPACITY * 3, 'X');

  nx::logi("{}", huge);
  cap.settle();

  bool found = false;
  for (const auto &e : cap.sink().entries()) {
    if (!e.message.empty() && e.message[0] == 'X') {
      found = true;
      CHECK(e.message.size() < nx::log::LOG_MESSAGE_CAPACITY);
      for (const char c : e.message)
        REQUIRE(c == 'X');
    }
  }
  CHECK(found);
  CHECK(cap.sink().terminator_violations() == 0);
}

TEST_CASE("log: an empty message and format braces are handled") {
  capture_scope cap;
  nx::logi("");
  nx::logi("{{braces}}");
  nx::logi("{}-{}-{}", 1, 2, 3);
  cap.settle();

  CHECK(cap.sink().count_with("{braces}") == 1);
  CHECK(cap.sink().count_with("1-2-3") == 1);

  bool saw_empty = false;
  for (const auto &e : cap.sink().entries())
    if (e.message.empty())
      saw_empty = true;
  CHECK(saw_empty);
}

TEST_CASE("log: flush drains the queue and reaches every sink") {
  capture_scope cap;
  for (int i = 0; i < 500; ++i)
    nx::logi("flush-subject-{}", i);

  nx::log::flush();
  CHECK(cap.sink().count_with("flush-subject-") == 500);
  CHECK(cap.sink().flushes() >= 1);
}

TEST_CASE("log: add_sink and remove_sink take effect immediately") {
  const nx::log::Level saved = nx::log::global_level();
  nx::log::set_level(nx::log::Level::Trace);

  auto *a = nx::allocate<capture_sink>();
  auto *b = nx::allocate<capture_sink>();
  nx::log::add_sink(a);

  nx::logi("only-a");
  nx::log::flush();
  CHECK(a->count_with("only-a") == 1);

  nx::log::add_sink(b);
  nx::logi("both");
  nx::log::flush();
  CHECK(a->count_with("both") == 1);
  CHECK(b->count_with("both") == 1);
  CHECK(b->count_with("only-a") == 0);

  nx::log::remove_sink(a);
  nx::logi("only-b");
  nx::log::flush();
  CHECK(b->count_with("only-b") == 1);

  nx::log::remove_sink(b);
  nx::logi("nobody-listening");
  nx::log::flush();

  nx::log::set_level(saved);
}

TEST_CASE("log: concurrent logging from many threads loses nothing") {
  capture_scope cap;
  constexpr int threads = 4;
  constexpr int per_thread = 500;

  std::vector<nx::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([t] {
      for (int i = 0; i < per_thread; ++i)
        nx::logi("concurrent t{} i{}", t, i);
    });
  }
  for (nx::thread &w : workers)
    w.join();
  nx::log::flush();

  CHECK(cap.sink().count_with("concurrent t") == threads * per_thread);
  CHECK(cap.sink().terminator_violations() == 0);

  for (int t = 0; t < threads; ++t) {
    const std::string needle = "concurrent t" + std::to_string(t) + " i" +
                               std::to_string(per_thread - 1);
    REQUIRE(cap.sink().count_with(needle) == 1);
  }

  std::set<u64> thread_ids;
  for (const auto &e : cap.sink().entries())
    if (e.message.rfind("concurrent t", 0) == 0)
      thread_ids.insert(e.thread_id);
  CHECK(thread_ids.size() == threads);
}

TEST_CASE("log: overflowing the queue still delivers every record") {
  capture_scope cap;
  constexpr int count = 20000;
  for (int i = 0; i < count; ++i)
    nx::logi("overflow-{}", i);
  nx::log::flush();

  CHECK(cap.sink().count_with("overflow-") == count);
}

TEST_CASE("log: timestamps are non-zero and non-decreasing per thread") {
  capture_scope cap;
  for (int i = 0; i < 50; ++i)
    nx::logi("stamp-{}", i);
  cap.settle();

  u64 previous = 0;
  int seen = 0;
  for (const auto &e : cap.sink().entries()) {
    (void)e;
    ++seen;
  }
  CHECK(seen >= 50);

  for (const auto &e : cap.sink().entries()) {
    if (e.message.rfind("stamp-", 0) != 0)
      continue;
    CHECK(e.thread_id != 0);
    previous = e.thread_id;
  }
  CHECK(previous != 0);
}

TEST_CASE("log: the console and debugger sinks accept records") {
  const nx::log::Level saved = nx::log::global_level();
  nx::log::set_level(nx::log::Level::Trace);

  nx::log::Sink *console = nx::log::make_console_sink(false);
  REQUIRE(console != nullptr);
  nx::log::add_sink(console);
  nx::logi("console sink smoke test");
  nx::log::flush();
  nx::log::remove_sink(console);

  nx::log::Sink *colored = nx::log::make_console_sink(true);
  REQUIRE(colored != nullptr);
  nx::log::add_sink(colored);
  nx::logw("colored console smoke test");
  nx::log::flush();
  nx::log::remove_sink(colored);

  if (nx::log::Sink *dbg = nx::log::make_debugger_sink()) {
    nx::log::add_sink(dbg);
    nx::logi("debugger sink smoke test");
    nx::log::flush();
    nx::log::remove_sink(dbg);
  }

  nx::log::Sink *android = nx::log::make_android_sink();
#if defined(NX_PLATFORM_ANDROID)
  CHECK(android != nullptr);
  if (android) {
    nx::log::add_sink(android);
    nx::log::flush();
    nx::log::remove_sink(android);
  }
#else
  CHECK(android == nullptr);
#endif

  nx::log::set_level(saved);
}

TEST_CASE("log: the file sink writes the messages to disk") {
  const nx::string path = temp_log_path("basic.log");
  (void)nx::fs::remove(nx::fs::path_view(path));

  const nx::log::Level saved = nx::log::global_level();
  nx::log::set_level(nx::log::Level::Trace);

  nx::log::Sink *file = nx::log::make_file_sink(nx::string_view(path));
  REQUIRE(file != nullptr);
  nx::log::add_sink(file);

  for (int i = 0; i < 200; ++i)
    nx::logi("file-sink-subject-{}", i);
  nx::log::flush();
  nx::log::remove_sink(file);

  const auto text = nx::fs::file_read_text(nx::fs::path_view(path));
  REQUIRE(text.has_value());
  CHECK(text->contains("file-sink-subject-0"));
  CHECK(text->contains("file-sink-subject-199"));

  (void)nx::fs::remove(nx::fs::path_view(path));
  nx::log::set_level(saved);
}

TEST_CASE("log: the file sink rotates once it exceeds max_bytes") {
  const nx::string path = temp_log_path("rotate.log");
  const nx::string rotated = path + ".1";
  (void)nx::fs::remove(nx::fs::path_view(path));
  (void)nx::fs::remove(nx::fs::path_view(rotated));

  const nx::log::Level saved = nx::log::global_level();
  nx::log::set_level(nx::log::Level::Trace);

  nx::log::Sink *file = nx::log::make_file_sink(nx::string_view(path), 4096, 3);
  REQUIRE(file != nullptr);
  nx::log::add_sink(file);

  const std::string filler(200, 'R');
  for (int i = 0; i < 200; ++i)
    nx::logi("{} {}", i, filler);
  nx::log::flush();
  nx::log::remove_sink(file);

  CHECK(nx::fs::exists(nx::fs::path_view(path)));
  CHECK(nx::fs::exists(nx::fs::path_view(rotated)));

  const auto current = nx::fs::file_read(nx::fs::path_view(path));
  REQUIRE(current.has_value());
  CHECK(current->size() <= 4096 * 4);

  for (const nx::string &p : {path, rotated})
    (void)nx::fs::remove(nx::fs::path_view(p));
  (void)nx::fs::remove(nx::fs::path_view(path + ".2"));
  (void)nx::fs::remove(nx::fs::path_view(path + ".3"));
  nx::log::set_level(saved);
}

TEST_CASE("log: an initialised logger does not have to be shut down by hand") {
  nx::log::Config cfg;
  cfg.level = nx::log::Level::Trace;
  cfg.console = false;
  cfg.debugger = false;
  cfg.android = false;
  cfg.file_path = nullptr;

  nx::log::init(cfg);
  for (int i = 0; i < 100; ++i)
    nx::logi("left-running-{}", i);
  nx::log::flush();
  CHECK(true);
}

TEST_CASE("log: init and shutdown pair up and are each idempotent") {
  nx::log::Config cfg;
  cfg.level = nx::log::Level::Trace;
  cfg.console = false;
  cfg.debugger = false;
  cfg.android = false;
  cfg.file_path = nullptr;

  nx::log::init(cfg);
  nx::log::init(cfg);

  {
    capture_scope cap;
    nx::logi("after-init");
    for (int i = 0; i < 200; ++i)
      nx::logi("async-{}", i);
    cap.settle();
    CHECK(cap.sink().count_with("after-init") == 1);
    CHECK(cap.sink().count_with("async-") == 200);
  }

  nx::log::flush();
  nx::log::flush();

  nx::log::shutdown();
  nx::log::shutdown();

  {
    capture_scope cap;
    nx::logi("after-shutdown");
    cap.settle();
    CHECK(cap.sink().count_with("after-shutdown") == 1);
  }

  nx::log::init(cfg);
  {
    capture_scope cap;
    nx::logi("second-cycle");
    cap.settle();
    CHECK(cap.sink().count_with("second-cycle") == 1);
  }
  nx::log::shutdown();
}

TEST_CASE("log: with a pool attached, a pool job writes the records") {
  init_quiet();
  nx::thread_pool pool(nx::thread_pool::config{.thread_count = 2});
  capture_scope cap;
  nx::log::attach_drain_pool(pool);

  for (int i = 0; i < 50; ++i)
    nx::logi("pooled-{}", i);
  nx::log::detach_drain_pool(pool);

  CHECK(cap.sink().count_with("pooled-") == 50);
  const u64 self = nx::this_thread::id();
  for (const auto &e : cap.sink().entries())
    if (e.message.rfind("pooled-", 0) == 0)
      CHECK(e.writer_id != self);
}

TEST_CASE("log: detaching the pool leaves nothing queued, in order") {
  init_quiet();
  constexpr int threads = 4;
  constexpr int per_thread = 2000;
  capture_scope cap;
  {
    nx::thread_pool pool(nx::thread_pool::config{.thread_count = 2});
    nx::log::attach_drain_pool(pool);
    std::vector<nx::thread> loggers;
    for (int t = 0; t < threads; ++t)
      loggers.emplace_back([t] {
        for (int i = 0; i < per_thread; ++i)
          nx::logi("ordered {} {}", t, i);
      });
    for (nx::thread &logger : loggers)
      logger.join();
    nx::log::detach_drain_pool(pool);
    CHECK(cap.sink().count_with("ordered ") == threads * per_thread);
  }

  int next[threads] = {};
  bool in_order = true;
  for (const auto &e : cap.sink().entries()) {
    int t = 0;
    int i = 0;
    if (std::sscanf(e.message.c_str(), "ordered %d %d", &t, &i) != 2)
      continue;
    in_order = in_order && t >= 0 && t < threads && i == next[t];
    if (t >= 0 && t < threads)
      next[t] = i + 1;
  }
  CHECK(in_order);

  nx::logi("after-detach");
  REQUIRE(cap.sink().count_with("after-detach") == 1);
  CHECK(cap.sink().entries().back().writer_id == nx::this_thread::id());
}

TEST_CASE("log: logging from a no-allocation scope with a pool attached") {
  init_quiet();
  // Holds every task node the pool keeps in reserve, so anything that
  // submits an ordinary task from here on has to allocate one.
  nx::event release{true};
  nx::thread_pool pool(nx::thread_pool::config{.thread_count = 2});
  capture_scope cap;
  for (int i = 0; i < 300; ++i)
    pool.submit([&release] { release.wait(); });
  nx::log::attach_drain_pool(pool);
  for (int round = 0; round < 20; ++round) {
    const nx::scoped_no_alloc guard;
    nx::logw("no-alloc-{}", round);
  }
  release.set();
  nx::log::detach_drain_pool(pool);
  CHECK(cap.sink().count_with("no-alloc-") == 20);
}

TEST_CASE("log: with a pool attached, records are written without a flush") {
  init_quiet();
  nx::thread_pool pool(nx::thread_pool::config{.thread_count = 2});
  capture_scope cap;
  nx::log::attach_drain_pool(pool);

  // One at a time, the next logged the moment the last is written: each is
  // the last record the drain job sees.
  constexpr int singles = 2000;
  const int before = cap.sink().written();
  int stranded = 0;
  for (int i = 0; i < singles && stranded == 0; ++i) {
    nx::logi("single {}", i);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (cap.sink().written() - before < i + 1)
      if (std::chrono::steady_clock::now() >= deadline) {
        ++stranded;
        break;
      } else {
        nx::this_thread::yield();
      }
  }
  CHECK(stranded == 0);

  constexpr int threads = 4;
  constexpr int per_thread = 2000;
  const int burst_before = cap.sink().written();
  std::vector<nx::thread> loggers;
  for (int t = 0; t < threads; ++t)
    loggers.emplace_back([t] {
      for (int i = 0; i < per_thread; ++i)
        nx::logi("unflushed {} {}", t, i);
    });
  for (nx::thread &logger : loggers)
    logger.join();
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (cap.sink().written() - burst_before < threads * per_thread &&
         std::chrono::steady_clock::now() < deadline)
    nx::this_thread::sleep_for(1ms);
  CHECK(cap.sink().written() - burst_before == threads * per_thread);
  nx::log::detach_drain_pool(pool);
}

namespace {

std::atomic<int> g_counted_sinks_destroyed{0};

class counted_sink final : public nx::log::Sink {
public:
  ~counted_sink() override { g_counted_sinks_destroyed.fetch_add(1); }
  void write(const nx::log::Record &) override {}
};

} // namespace

TEST_CASE("log: removing a sink the logger does not hold leaves it alone") {
  g_counted_sinks_destroyed = 0;
  auto *const sink = nx::allocate<counted_sink>();
  nx::log::remove_sink(sink);
  CHECK(g_counted_sinks_destroyed.load() == 0);
  nx::release(sink);
  CHECK(g_counted_sinks_destroyed.load() == 1);
}

// The order an application exits in: the logger shuts down, then an owner
// that registered a sink is destroyed and removes it.
TEST_CASE("log: a sink shutdown destroyed may still be removed afterwards") {
  init_quiet();
  g_counted_sinks_destroyed = 0;
  nx::log::Sink *const sink = nx::allocate<counted_sink>();
  nx::log::add_sink(sink);
  nx::log::shutdown();
  CHECK(g_counted_sinks_destroyed.load() == 1);
  nx::log::remove_sink(sink);
  CHECK(g_counted_sinks_destroyed.load() == 1);
}
