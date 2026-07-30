#pragma once

/**
 * @file log.h
 * @brief Asynchronous, multi-sink logging (namespace nx::log).
 *
 * Log records are formatted on the calling thread, then handed to a
 * background worker that fans them out to every registered sink (console,
 * debugger, rotating file, or your own). The hot path is a level check plus
 * a bounded copy into a lock-free queue; all I/O happens off-thread. When the
 * queue is full or the worker has not started yet, delivery falls back to a
 * synchronous write so records are never silently dropped.
 *
 * Categories are open and interned at run time: nx::log::category("physics")
 * returns a stable handle whose minimum level can be tuned independently.
 * Handles for the common engine categories are predefined (app, gpu, render,
 * system).
 *
 * Typical use goes through the nx::logv / logd / logi / logw / loge / logc
 * helpers:
 *
 *     nx::logi("startup: {} cores", nx::cpu::logical_core_count());
 *     nx::loge(nx::log::gpu, "vkCreateBuffer failed: {}", result);
 *
 * Call nx::log::init() once at startup (installs the default sinks and starts
 * the worker) and nx::log::shutdown() at exit (drains, flushes, joins).
 */

#include "foundation/core/foundation.h"
#include "foundation/strings/format.h"

#include <atomic>

namespace nx::log {

/// Severity ordering. A record is emitted only when its level is at least the
/// effective minimum (the higher of the global and per-category thresholds).
enum class Level : u8 {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
  Critical = 5,
  Off = 6,
};

/// Max bytes of a single formatted message (longer messages are truncated).
inline constexpr usize LOG_MESSAGE_CAPACITY = 1024;

/// Registry entry backing a category. Addresses are stable for the process
/// lifetime; never construct one directly, obtain a Category via category().
struct CategoryState {
  char name[32];
  std::atomic<u8> level; // minimum Level for this category
};

/// Lightweight handle to a log category (a pointer into the registry).
class Category {
public:
  Category() = default;
  explicit Category(CategoryState *state) noexcept : m_state(state) {}

  [[nodiscard]] const char *name() const noexcept {
    return m_state ? m_state->name : "?";
  }
  [[nodiscard]] Level level() const noexcept {
    return m_state
               ? nx::cast<Level>(m_state->level.load(std::memory_order_relaxed))
               : Level::Trace;
  }
  [[nodiscard]] CategoryState *raw() const noexcept { return m_state; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return m_state != nullptr;
  }

private:
  CategoryState *m_state = nullptr;
};

/// A single log entry as delivered to sinks. POD so it rides the lock-free
/// queue by value.
struct Record {
  u64 timestamp_ns = 0; // wall-clock, system_clock epoch
  u64 thread_id = 0;
  const CategoryState *category = nullptr;
  Level level = Level::Info;
  u16 length = 0;
  char message[LOG_MESSAGE_CAPACITY] = {};
};

/// Output destination. write() and flush() are always invoked serialized (the
/// logger holds an internal mutex), so implementations need no locking of
/// their own.
class Sink {
public:
  virtual ~Sink() = default;
  virtual void write(const Record &record) = 0;
  virtual void flush() {}
};

/// Returns the handle for @p name, registering it on first use. Thread-safe
/// and idempotent: the same name always yields the same handle.
Category category(string_view name);

/// Sets the global minimum level (applies across all categories).
void set_level(Level level);
/// Raises/lowers the minimum level for a single category.
void set_level(Category category, Level level);
/// Current global minimum level.
[[nodiscard]] Level global_level() noexcept;

const char *level_name(Level level) noexcept;

/// Configuration for init(). Zero-cost defaults: colored console + debugger,
/// no file.
struct Config {
  Level level = Level::Info;
  bool console = true;
  bool console_color = true;
  bool debugger = true;            // no-op off Windows
  bool android = true;             // logcat; no-op off Android
  const char *file_path = nullptr; // enables the rotating file sink when set
  u64 file_max_bytes = 8ull << 20; // 8 MiB per file
  u32 file_max_files = 4;          // engine.log + engine.log.1 .. .3
};

/// Installs the configured sinks and starts the background worker. Idempotent.
void init(const Config &config = {});
/// Drains the queue, flushes and destroys all sinks, and joins the worker.
void shutdown();
/// Blocks until the queue is drained, then flushes every sink.
void flush();

/// Adds a sink; the logger takes ownership and destroys it (via nx::release)
/// on remove_sink()/shutdown(). The sink must therefore be created with
/// nx::allocate — the make_*_sink() factories below do this for you.
void add_sink(Sink *sink);
/// Removes and destroys a previously added sink.
void remove_sink(Sink *sink);

/// Built-in sinks (ownership passes to the logger via add_sink()).
Sink *make_console_sink(bool color = true);
Sink *make_debugger_sink();
Sink *make_file_sink(string_view path, u64 max_bytes = 8ull << 20,
                     u32 max_files = 4);
/// Routes to Android logcat (category name becomes the logcat tag). Returns
/// nullptr off Android.
Sink *make_android_sink();

namespace detail {
inline std::atomic<u8> g_global_level{nx::cast<u8>(Level::Info)};

/// Copies a preformatted, null-terminated message into a Record and enqueues
/// it (or writes synchronously on overflow / before init()).
void submit(Level level, Category category, const char *message) noexcept;
} // namespace detail

/// Cheap early-out used by the log helpers before any formatting happens.
[[nodiscard]] NX_FORCE_INLINE bool should_log(Level level,
                                              Category category) noexcept {
  const u8 lvl = nx::cast<u8>(level);
  return lvl != nx::cast<u8>(Level::Off) &&
         lvl >= detail::g_global_level.load(std::memory_order_relaxed) &&
         lvl >= nx::cast<u8>(category.level());
}

/// Predefined engine categories.
inline const Category app = category("app");
inline const Category gpu = category("gpu");
inline const Category render = category("render");
inline const Category system = category("system");

} // namespace nx::log

namespace nx {

#define NX_LOG_FUNC(fn, LVL)                                                   \
  template <typename... Args>                                                  \
  void fn(log::Category cat, string_view fmt, Args &&...args) {                \
    if (!log::should_log(log::Level::LVL, cat))                                \
      return;                                                                  \
    char buf[log::LOG_MESSAGE_CAPACITY];                                       \
    log::detail::submit(log::Level::LVL, cat,                                  \
                        format_to(buf, fmt, std::forward<Args>(args)...));     \
  }                                                                            \
  template <typename... Args> void fn(string_view fmt, Args &&...args) {       \
    fn(log::app, fmt, std::forward<Args>(args)...);                            \
  }

NX_LOG_FUNC(logv, Trace)
NX_LOG_FUNC(logt, Trace)
NX_LOG_FUNC(logd, Debug)
NX_LOG_FUNC(logi, Info)
NX_LOG_FUNC(logw, Warn)
NX_LOG_FUNC(loge, Error)
NX_LOG_FUNC(logc, Critical)

#undef NX_LOG_FUNC

} // namespace nx
