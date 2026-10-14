#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/format.h"

#include <atomic>

namespace nx {
class thread_pool;
}

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

/// Shipping starts quiet unless an application explicitly opts into a more
/// verbose level. Warnings, errors and critical failures still pass.
#if defined(NX_BUILD_SHIPPING)
inline constexpr Level DEFAULT_LEVEL = Level::Warn;
#else
inline constexpr Level DEFAULT_LEVEL = Level::Info;
#endif

/// Max bytes of a single formatted message (longer messages are truncated).
inline constexpr usize LOG_MESSAGE_CAPACITY = 1024;

/// Registry entry backing a category. Addresses are stable for the process
/// lifetime; never construct one directly, obtain a Category via category().
struct CategoryState {
  char name[32];
  u64 key;
  std::atomic<u8> level;
};

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
  u64 timestamp_ns = 0;
  u64 thread_id = 0;
  const CategoryState *category = nullptr;
  Level level = Level::Info;
  u16 length = 0;
  char message[LOG_MESSAGE_CAPACITY] = {};
};

class Sink {
public:
  virtual ~Sink() = default;
  virtual void write(const Record &record) = 0;
  virtual void flush() {}
};

/// Returns the handle for @p name, registering it on first use. Thread-safe
/// and idempotent: the same name always yields the same handle.
Category category(string_view name) noexcept;

void set_level(Level level);
void set_level(Category category, Level level);
[[nodiscard]] Level global_level() noexcept;

const char *level_name(Level level) noexcept;

struct Config {
  Level level = DEFAULT_LEVEL;
  bool console = true;
  bool console_color = true;
  bool debugger = true;
  bool android = true;
  const char *file_path = nullptr;
  u64 file_max_bytes = 8ull << 20;
  u32 file_max_files = 4;
};

void init(const Config &config = {});
void shutdown();
/// Writes every queued record on the calling thread, then flushes the sinks.
void flush();

/// After init(), records queue and a low-priority job on @p pool writes them.
/// Without an attached pool, or when the queue is full, the thread that logs
/// writes them itself. Attaching while another pool is attached does nothing.
void attach_drain_pool(thread_pool &pool);
/// Returns once no drain job of @p pool is running and every queued record is
/// written. Does nothing unless @p pool is the attached one.
void detach_drain_pool(thread_pool &pool);

/// Until init(), records go to stderr whatever sinks are added: a sink such as
/// an engine's in-game console then observes the log without silencing it.
/// Adds a sink; the logger takes ownership and destroys it (via nx::release)
/// on remove_sink()/shutdown(). The sink must therefore be created with
/// nx::allocate — the make_*_sink() factories below do this for you.
/// remove_sink() on a sink the logger no longer holds, such as one shutdown()
/// already destroyed, does nothing.
void add_sink(Sink *sink);
void remove_sink(Sink *sink);

/// Built-in sinks (ownership passes to the logger via add_sink()).
Sink *make_console_sink(bool color = true);
Sink *make_debugger_sink();
Sink *make_file_sink(string_view path, u64 max_bytes = 8ull << 20,
                     u32 max_files = 4);
Sink *make_android_sink();

namespace detail {
inline std::atomic<u8> g_global_level{nx::cast<u8>(DEFAULT_LEVEL)};

/// Copies a preformatted, null-terminated message into a Record and queues it
/// for the drain job, or writes it on this thread (see attach_drain_pool).
void submit(Level level, Category category, const char *message) noexcept;
}

[[nodiscard]] NX_FORCE_INLINE bool should_log(Level level,
                                              Category category) noexcept {
  const u8 lvl = nx::cast<u8>(level);
  return lvl != nx::cast<u8>(Level::Off) &&
         lvl >= detail::g_global_level.load(std::memory_order_relaxed) &&
         lvl >= nx::cast<u8>(category.level());
}

inline const Category app = category("app");
inline const Category gpu = category("gpu");
inline const Category render = category("render");
inline const Category rhi = category("rhi");
inline const Category system = category("system");

}

namespace nx {

#define NX_LOG_FUNC(fn, LVL)                                                   \
  template <typename... Args>                                                  \
  void fn(log::Category cat, string_view fmt, Args &&...args) noexcept {       \
    if (!log::should_log(log::Level::LVL, cat))                                \
      return;                                                                  \
    char buf[log::LOG_MESSAGE_CAPACITY];                                       \
    log::detail::submit(log::Level::LVL, cat,                                  \
                        format_to(buf, fmt, std::forward<Args>(args)...));     \
  }                                                                            \
  template <typename... Args>                                                  \
  void fn(string_view fmt, Args &&...args) noexcept {                          \
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

}
