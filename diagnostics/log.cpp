#include "core/foundation/diagnostics/log.h"

#include "core/foundation/containers/concurrent_queue.h"
#include "core/foundation/containers/small_vector.h"
#include "core/foundation/core/hash.h"
#include "core/foundation/threading/sync.h"
#include "core/foundation/threading/thread.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

#if NX_HAS_WINDOWS_API
#include <io.h>
#include <windows.h>
#elif NX_PLATFORM_POSIX
#include <unistd.h>
#endif

#if NX_PLATFORM_ANDROID
#include <android/log.h>
#endif

namespace nx::log {

namespace {

constexpr usize MAX_CATEGORIES = 64;
constexpr usize LOG_QUEUE_CAPACITY = 1024;
constexpr usize LINE_CAPACITY = LOG_MESSAGE_CAPACITY + 160;

// category registry

struct Registry {
  CategoryState entries[MAX_CATEGORIES];
  std::atomic<u32> count{0};
  mutex mtx;
};

Registry &registry() noexcept {
  static Registry r;
  return r;
}

// formatting

const char *level_color(Level lvl) noexcept {
  switch (lvl) {
  case Level::Trace:
    return "\x1b[90m"; // bright black
  case Level::Debug:
    return "\x1b[36m"; // cyan
  case Level::Info:
    return "\x1b[32m"; // green
  case Level::Warn:
    return "\x1b[33m"; // yellow
  case Level::Error:
    return "\x1b[31m"; // red
  case Level::Critical:
    return "\x1b[1;31m"; // bold red
  default:
    return "";
  }
}

u64 now_ns() noexcept {
  return nx::cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count());
}

// Renders "HH:MM:SS.mmm LEVEL [category] message\n" into out, returning the
// byte count written (excluding the terminator). When with_thread is set the
// originating thread id is included for post-mortem correlation.
usize format_record(char *out, usize cap, const Record &r, bool color,
                    bool with_thread) noexcept {
  const auto secs = nx::cast<time_t>(r.timestamp_ns / 1000000000ull);
  const auto ms = nx::cast<u32>((r.timestamp_ns / 1000000ull) % 1000ull);
  std::tm tmv = {};
#if NX_HAS_WINDOWS_API
  localtime_s(&tmv, &secs);
#else
  localtime_r(&secs, &tmv);
#endif

  const char *lvl = level_name(r.level);
  const char *cat = r.category ? r.category->name : "?";
  const char *cpre = color ? level_color(r.level) : "";
  const char *crst = color ? "\x1b[0m" : "";

  int n;
  if (with_thread)
    n = snprintf(out, cap, "%02d:%02d:%02d.%03u %s%-5s%s [%s] (t%llu) %.*s\n",
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms, cpre, lvl, crst, cat,
                 nx::cast<unsigned long long>(r.thread_id),
                 nx::cast<int>(r.length), r.message);
  else
    n = snprintf(out, cap, "%02d:%02d:%02d.%03u %s%-5s%s [%s] %.*s\n",
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms, cpre, lvl, crst, cat,
                 nx::cast<int>(r.length), r.message);

  if (n < 0)
    return 0;
  return min(nx::cast<usize>(n), cap - 1);
}

bool stderr_is_tty() noexcept {
#if NX_HAS_WINDOWS_API
  return _isatty(_fileno(stderr)) != 0;
#elif NX_PLATFORM_POSIX
  return isatty(fileno(stderr)) != 0;
#else
  return false;
#endif
}

#if NX_HAS_WINDOWS_API
void enable_virtual_terminal() noexcept {
  const HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  DWORD mode = 0;
  if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
#endif

// sinks

class ConsoleSink final : public Sink {
public:
  explicit ConsoleSink(bool color) : m_color(color && stderr_is_tty()) {
#if NX_HAS_WINDOWS_API
    if (m_color) {
      static once_flag once;
      call_once(once, enable_virtual_terminal);
    }
#endif
  }

  void write(const Record &r) override {
    char buf[LINE_CAPACITY];
    const usize n = format_record(buf, sizeof(buf), r, m_color, false);
    fwrite(buf, 1, n, stderr);
  }

  void flush() override { fflush(stderr); }

private:
  bool m_color;
};

class DebuggerSink final : public Sink {
public:
  void write(const Record &r) override {
#if NX_HAS_WINDOWS_API
    char buf[LINE_CAPACITY];
    const usize n = format_record(buf, sizeof(buf), r, false, false);
    buf[n] = '\0';
    OutputDebugStringA(buf);
#else
    (void)r;
#endif
  }
};

#if NX_PLATFORM_ANDROID
class AndroidSink final : public Sink {
public:
  void write(const Record &r) override {
    // logcat stamps its own time and shows the priority + tag columns, so we
    // hand it the bare message and let the category be the tag.
    const char *tag = r.category ? r.category->name : "nx";
    __android_log_write(to_android_priority(r.level), tag, r.message);
  }

private:
  static int to_android_priority(Level lvl) noexcept {
    switch (lvl) {
    case Level::Trace:
      return ANDROID_LOG_VERBOSE;
    case Level::Debug:
      return ANDROID_LOG_DEBUG;
    case Level::Info:
      return ANDROID_LOG_INFO;
    case Level::Warn:
      return ANDROID_LOG_WARN;
    case Level::Error:
      return ANDROID_LOG_ERROR;
    case Level::Critical:
      return ANDROID_LOG_FATAL;
    default:
      return ANDROID_LOG_DEFAULT;
    }
  }
};
#endif

class FileSink final : public Sink {
public:
  FileSink(string_view path, u64 max_bytes, u32 max_files)
      : m_max_bytes(max_bytes), m_max_files(max(max_files, 1u)) {
    const usize n = min(path.size(), sizeof(m_path) - 1);
    memcpy(m_path, path.data(), n);
    m_path[n] = '\0';
    m_file = fopen(m_path, "wb");
  }

  ~FileSink() override {
    if (m_file)
      fclose(m_file);
  }

  void write(const Record &r) override {
    if (!m_file)
      return;
    char buf[LINE_CAPACITY];
    const usize n = format_record(buf, sizeof(buf), r, false, true);
    fwrite(buf, 1, n, m_file);
    m_bytes += n;
    if (m_bytes >= m_max_bytes)
      rotate();
  }

  void flush() override {
    if (m_file)
      fflush(m_file);
  }

private:
  // engine.log -> engine.log.1 -> ... -> engine.log.<max_files>, oldest
  // dropped.
  void rotate() noexcept {
    if (m_file) {
      fclose(m_file);
      m_file = nullptr;
    }

    char from[sizeof(m_path) + 16];
    char to[sizeof(m_path) + 16];

    snprintf(to, sizeof(to), "%s.%u", m_path, m_max_files);
    remove(to);
    for (u32 i = m_max_files; i > 1; --i) {
      snprintf(from, sizeof(from), "%s.%u", m_path, i - 1);
      snprintf(to, sizeof(to), "%s.%u", m_path, i);
      rename(from, to);
    }
    snprintf(to, sizeof(to), "%s.1", m_path);
    rename(m_path, to);

    m_file = fopen(m_path, "wb");
    m_bytes = 0;
  }

  FILE *m_file = nullptr;
  u64 m_max_bytes = 0;
  u64 m_bytes = 0;
  u32 m_max_files = 1;
  char m_path[248] = {};
};

// logger

struct Logger {
  mpmc::Queue<Record, LOG_QUEUE_CAPACITY> *queue = nullptr;
  nx::thread worker;

  std::atomic<u32> signal{0};
  std::atomic<u32> sleeping{0};
  std::atomic<u64> enqueued{0};  // records handed to the queue
  std::atomic<u64> processed{0}; // records the worker has written to sinks
  std::atomic<bool> running{false};
  std::atomic<bool> stop{false};

  mutex sink_mutex;
  small_vector<Sink *, 8> sinks;
  ~Logger();
};

Logger &logger() noexcept {
  static Logger l;
  return l;
}

// Serialized fan-out to every sink. Before init() (or if the user cleared all
// sinks) it falls back to a colored stderr write so records are never lost.
void write_record(Logger &l, const Record &r) noexcept {
  scoped_lock lock(l.sink_mutex);
  if (l.sinks.empty()) {
    char buf[LINE_CAPACITY];
    const usize n = format_record(buf, sizeof(buf), r, stderr_is_tty(), false);
    fwrite(buf, 1, n, stderr);
    return;
  }
  for (Sink *s : l.sinks)
    s->write(r);
}

void drain(Logger &l) noexcept {
  Record r;
  while (l.queue->try_pop(r)) {
    write_record(l, r);
    l.processed.fetch_add(1, std::memory_order_release);
  }
}

void worker_main(Logger &l) noexcept {
  for (;;) {
    const u32 sig = l.signal.load(std::memory_order_acquire);
    drain(l);

    if (l.stop.load(std::memory_order_acquire)) {
      drain(l);
      break;
    }

    // Re-check signal after registering as a sleeper so a submit() that raced
    // the drain cannot leave a record stranded until the next wake.
    l.sleeping.fetch_add(1, std::memory_order_seq_cst);
    if (l.signal.load(std::memory_order_acquire) == sig &&
        !l.stop.load(std::memory_order_acquire))
      futex::wait(&l.signal, sig, nx::detail::WAIT_FOREVER);
    l.sleeping.fetch_sub(1, std::memory_order_relaxed);
  }
}

} // namespace

const char *level_name(Level level) noexcept {
  switch (level) {
  case Level::Trace:
    return "TRACE";
  case Level::Debug:
    return "DEBUG";
  case Level::Info:
    return "INFO";
  case Level::Warn:
    return "WARN";
  case Level::Error:
    return "ERROR";
  case Level::Critical:
    return "CRIT";
  default:
    return "OFF";
  }
}

Category category(const string_view name) noexcept {
  Registry &r = registry();
  scoped_lock lock(r.mtx);

  const u64 key = hash_fnv1a64(name);
  const usize length = min(name.size(), sizeof(CategoryState::name) - 1);
  const u32 count = r.count.load(std::memory_order_relaxed);
  for (u32 i = 0; i < count; ++i)
    if (r.entries[i].key == key &&
        string_view(r.entries[i].name) == name.substr(0, length))
      return Category{&r.entries[i]};

  if (count >= MAX_CATEGORIES)
    return Category{&r.entries[0]}; // registry full: fall back to first

  CategoryState &e = r.entries[count];
  memcpy(e.name, name.data(), length);
  e.name[length] = '\0';
  e.key = key;
  e.level.store(nx::cast<u8>(Level::Trace), std::memory_order_relaxed);
  r.count.store(count + 1, std::memory_order_release);
  return Category{&e};
}

void set_level(Level level) {
  detail::g_global_level.store(nx::cast<u8>(level), std::memory_order_relaxed);
}

void set_level(Category category, Level level) {
  if (CategoryState *s = category.raw())
    s->level.store(nx::cast<u8>(level), std::memory_order_relaxed);
}

Level global_level() noexcept {
  return nx::cast<Level>(
      detail::g_global_level.load(std::memory_order_relaxed));
}

void add_sink(Sink *sink) {
  if (!sink)
    return;
  Logger &l = logger();
  scoped_lock lock(l.sink_mutex);
  l.sinks.push_back(sink);
}

void remove_sink(Sink *sink) {
  if (!sink)
    return;
  Logger &l = logger();
  scoped_lock lock(l.sink_mutex);
  for (usize i = 0; i < l.sinks.size(); ++i)
    if (l.sinks[i] == sink) {
      l.sinks.erase(l.sinks.begin() + i);
      break;
    }
  release(sink);
}

Sink *make_console_sink(bool color) { return allocate<ConsoleSink>(color); }
Sink *make_debugger_sink() { return allocate<DebuggerSink>(); }
Sink *make_file_sink(string_view path, u64 max_bytes, u32 max_files) {
  return allocate<FileSink>(path, max_bytes, max_files);
}
Sink *make_android_sink() {
#if NX_PLATFORM_ANDROID
  return allocate<AndroidSink>();
#else
  return nullptr;
#endif
}

void init(const Config &config) {
  Logger &l = logger();
  if (l.running.load(std::memory_order_acquire))
    return;

  set_level(config.level);

  if (config.console)
    add_sink(make_console_sink(config.console_color));
  if (config.debugger)
    add_sink(make_debugger_sink());
#if NX_PLATFORM_ANDROID
  if (config.android)
    add_sink(make_android_sink());
#endif
  if (config.file_path)
    add_sink(make_file_sink(config.file_path, config.file_max_bytes,
                            config.file_max_files));

  l.queue = allocate<mpmc::Queue<Record, LOG_QUEUE_CAPACITY>>();
  l.stop.store(false, std::memory_order_relaxed);
  l.running.store(true, std::memory_order_release);
  l.worker = nx::thread([&l] { worker_main(l); }, {.name = "nx-log"});
}

void shutdown_impl(Logger &l) noexcept {
  if (l.running.load(std::memory_order_acquire)) {
    l.stop.store(true, std::memory_order_release);
    l.signal.fetch_add(1, std::memory_order_seq_cst);
    futex::wake_all(&l.signal);
    if (l.worker.joinable())
      l.worker.join();
    l.running.store(false, std::memory_order_release);

    if (l.queue) {
      drain(l);
      release(l.queue);
      l.queue = nullptr;
    }
  }

  scoped_lock lock(l.sink_mutex);
  for (Sink *s : l.sinks) {
    s->flush();
    release(s);
  }
  l.sinks.clear();
}

Logger::~Logger() { shutdown_impl(*this); }

void shutdown() { shutdown_impl(logger()); }

void flush() {
  Logger &l = logger();
  // Wait until the worker has written every record enqueued so far, not just
  // dequeued them: a popped record may not yet be in its sink.
  if (l.running.load(std::memory_order_acquire)) {
    const u64 target = l.enqueued.load(std::memory_order_acquire);
    while (l.processed.load(std::memory_order_acquire) < target)
      this_thread::yield();
  }
  scoped_lock lock(l.sink_mutex);
  for (Sink *s : l.sinks)
    s->flush();
}

namespace detail {

void submit(Level level, Category category, const char *message) noexcept {
  Logger &l = logger();

  Record r;
  r.timestamp_ns = now_ns();
  r.thread_id = this_thread::id();
  r.category = category.raw();
  r.level = level;

  usize n = 0;
  if (message)
    while (message[n] && n < LOG_MESSAGE_CAPACITY - 1) {
      r.message[n] = message[n];
      ++n;
    }
  r.message[n] = '\0';
  r.length = nx::cast<u16>(n);

  if (l.running.load(std::memory_order_acquire) && l.queue) {
    if (l.queue->try_push(r)) {
      l.enqueued.fetch_add(1, std::memory_order_release);
      l.signal.fetch_add(1, std::memory_order_release);
      if (l.sleeping.load(std::memory_order_acquire) > 0)
        futex::wake(&l.signal, 1);
      return;
    }
    // Queue full: worker is behind, write synchronously rather than drop.
  }
  write_record(l, r);
}

} // namespace detail

} // namespace nx::log
