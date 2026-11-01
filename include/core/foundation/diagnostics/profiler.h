#pragma once

#include "core/foundation/core/foundation.h"

#if !defined(NX_PROFILE_ENABLED)
#define NX_PROFILE_ENABLED 0
#endif

#if defined(NX_BUILD_SHIPPING) && !NX_PROFILE_ENABLED
#define NX_PROFILE_HOOKS 0
#elif NX_PROFILE_ENABLED
#define NX_PROFILE_HOOKS 0
#else
#define NX_PROFILE_HOOKS 1
#endif

#if !defined(NX_BUILD_SHIPPING)

#include <atomic>

namespace nx::profile {

/// A zone's fixed description; its address names the zone. The alignment
/// leaves a recorder the low bits of that address for a tag.
struct alignas(8) zone_site {
  const char *name = nullptr;
  u32 color = 0;
};

/// What the macros call while something records. Foundation keeps no trace of
/// its own. Zones hit these on any thread, the audio callback's included, so
/// they must not block, allocate or suspend. A table stays valid for the rest
/// of the process, because a zone open when it is replaced still ends through
/// it.
struct hooks {
  void (*zone_begin)(const zone_site &site) noexcept;
  void (*zone_begin_text)(const char *text, usize size) noexcept;
  void (*zone_end)() noexcept;
  void (*frame_mark)(const char *name) noexcept;
  void (*plot)(const char *name, f64 value) noexcept;
  /// The calling thread now runs fiber @p fiber, whichever thread it ran on
  /// before; 0 is the thread's own fiber, which runs no job.
  void (*fiber_switch)(u32 fiber) noexcept;
};

namespace detail {
inline std::atomic<const hooks *> g_hooks{nullptr};
inline thread_local const char *t_thread_name = nullptr;
} // namespace detail

inline void set_hooks(const hooks *const table) noexcept {
  detail::g_hooks.store(table, std::memory_order_relaxed);
}

[[nodiscard]] inline const hooks *active_hooks() noexcept {
  return detail::g_hooks.load(std::memory_order_relaxed);
}

/// Threads are named when they start, usually before anything records, so the
/// name waits here for a recorder to read. It must outlive the thread.
inline void set_thread_name(const char *const name) noexcept {
  detail::t_thread_name = name;
}

[[nodiscard]] inline const char *thread_name() noexcept {
  return detail::t_thread_name;
}

class zone_scope {
public:
  explicit zone_scope(const zone_site &site) noexcept
      : m_hooks(active_hooks()) {
    if (m_hooks != nullptr)
      m_hooks->zone_begin(site);
  }
  zone_scope(const char *const text, const usize size) noexcept
      : m_hooks(active_hooks()) {
    if (m_hooks != nullptr)
      m_hooks->zone_begin_text(text, size);
  }
  ~zone_scope() {
    if (m_hooks != nullptr)
      m_hooks->zone_end();
  }
  zone_scope(const zone_scope &) = delete;
  zone_scope &operator=(const zone_scope &) = delete;

private:
  const hooks *m_hooks;
};

} // namespace nx::profile

#endif

#if NX_PROFILE_ENABLED

#include <tracy/Tracy.hpp>

#include <cstring>

#if defined(TRACY_FIBERS) && defined(TRACY_ON_DEMAND)
namespace nx::fiber {
// Tracy drops fiber switches while no viewer is connected, so a thread tells a
// newly connected one which fiber it is in before its first zone.
void announce_to_profiler() noexcept;
} // namespace nx::fiber
#define NX_PROFILE_ANNOUNCE_FIBER() ::nx::fiber::announce_to_profiler()
#else
#define NX_PROFILE_ANNOUNCE_FIBER() ((void)0)
#endif

#define NX_PROFILE_ZONE(name)                                                  \
  NX_PROFILE_ANNOUNCE_FIBER();                                                 \
  ZoneScopedN(name)
#define NX_PROFILE_ZONE_COLOR(name, color)                                     \
  NX_PROFILE_ANNOUNCE_FIBER();                                                 \
  ZoneScopedNC(name, color)
#define NX_PROFILE_FUNCTION()                                                  \
  NX_PROFILE_ANNOUNCE_FIBER();                                                 \
  ZoneScoped
/// A zone named at run time. Tracy copies the text, and its statistics group
/// zones by that name.
#define NX_PROFILE_ZONE_TEXT(text, size)                                       \
  NX_PROFILE_ANNOUNCE_FIBER();                                                 \
  ::tracy::ScopedZone nx_profile_text_zone(                                    \
      TracyLine, TracyFile, sizeof(TracyFile) - 1, TracyFunction,              \
      std::strlen(TracyFunction), text, size, -1, true)
#define NX_PROFILE_FRAME_MARK() FrameMark
#define NX_PROFILE_FRAME_MARK_NAMED(name) FrameMarkNamed(name)
#define NX_PROFILE_SET_THREAD_NAME(name) ::tracy::SetThreadName(name)
/// Names the calling thread on first reach, for a thread whose creation we do
/// not own - a device callback, where the callback body is the only hook.
#define NX_PROFILE_SET_THREAD_NAME_ONCE(name)                                  \
  do {                                                                         \
    static thread_local bool nx_thread_named = false;                          \
    if (!nx_thread_named) {                                                    \
      nx_thread_named = true;                                                  \
      ::tracy::SetThreadName(name);                                            \
    }                                                                          \
  } while (0)
#if defined(TRACY_FIBERS)
#define NX_PROFILE_FIBER_ENTER(name) TracyFiberEnter(name)
#define NX_PROFILE_FIBER_LEAVE() TracyFiberLeave
#else
#define NX_PROFILE_FIBER_ENTER(name) ((void)(name))
#define NX_PROFILE_FIBER_LEAVE() ((void)0)
#endif
#define NX_PROFILE_PLOT(name, value) TracyPlot(name, value)
#define NX_PROFILE_MESSAGE(text) TracyMessageL(text)
#define NX_PROFILE_ALLOC(ptr, size) TracyAlloc(ptr, size)
#define NX_PROFILE_FREE(ptr) TracyFree(ptr)

#elif NX_PROFILE_HOOKS

#define NX_PROFILE_DETAIL_JOIN2(a, b) a##b
#define NX_PROFILE_DETAIL_JOIN(a, b) NX_PROFILE_DETAIL_JOIN2(a, b)

#define NX_PROFILE_DETAIL_ZONE(name, color)                                    \
  static constexpr ::nx::profile::zone_site NX_PROFILE_DETAIL_JOIN(            \
      nx_profile_site_, __LINE__){name, color};                                \
  const ::nx::profile::zone_scope NX_PROFILE_DETAIL_JOIN(nx_profile_zone_,     \
                                                         __LINE__)(            \
      NX_PROFILE_DETAIL_JOIN(nx_profile_site_, __LINE__))

#define NX_PROFILE_ZONE(name) NX_PROFILE_DETAIL_ZONE(name, 0)
#define NX_PROFILE_ZONE_COLOR(name, color) NX_PROFILE_DETAIL_ZONE(name, color)
#define NX_PROFILE_FUNCTION() NX_PROFILE_DETAIL_ZONE(__FUNCTION__, 0)
/// A zone named at run time. The recorder copies the text.
#define NX_PROFILE_ZONE_TEXT(text, size)                                       \
  const ::nx::profile::zone_scope NX_PROFILE_DETAIL_JOIN(nx_profile_zone_,     \
                                                         __LINE__)(text, size)
#define NX_PROFILE_FRAME_MARK() NX_PROFILE_FRAME_MARK_NAMED(nullptr)
#define NX_PROFILE_FRAME_MARK_NAMED(name)                                      \
  do {                                                                         \
    if (const ::nx::profile::hooks *const nx_hooks =                           \
            ::nx::profile::active_hooks())                                     \
      nx_hooks->frame_mark(name);                                              \
  } while (0)
#define NX_PROFILE_SET_THREAD_NAME(name) ::nx::profile::set_thread_name(name)
/// Names the calling thread, for a thread whose creation we do not own - a
/// device callback, where the callback body is the only hook.
#define NX_PROFILE_SET_THREAD_NAME_ONCE(name)                                  \
  ::nx::profile::set_thread_name(name)
#define NX_PROFILE_FIBER_ENTER(name) ((void)(name))
#define NX_PROFILE_FIBER_LEAVE() ((void)0)
#define NX_PROFILE_PLOT(name, value)                                           \
  do {                                                                         \
    if (const ::nx::profile::hooks *const nx_hooks =                           \
            ::nx::profile::active_hooks())                                     \
      nx_hooks->plot(name, static_cast<f64>(value));                           \
  } while (0)
#define NX_PROFILE_MESSAGE(text) ((void)(text))
#define NX_PROFILE_ALLOC(ptr, size) ((void)(ptr), (void)(size))
#define NX_PROFILE_FREE(ptr) ((void)(ptr))

#else

#define NX_PROFILE_ZONE(name) ((void)(name))
#define NX_PROFILE_ZONE_COLOR(name, color) ((void)(name), (void)(color))
#define NX_PROFILE_FUNCTION() ((void)0)
#define NX_PROFILE_ZONE_TEXT(text, size) ((void)(text), (void)(size))
#define NX_PROFILE_FRAME_MARK() ((void)0)
#define NX_PROFILE_FRAME_MARK_NAMED(name) ((void)(name))
#define NX_PROFILE_SET_THREAD_NAME(name) ((void)(name))
/// Names the calling thread on first reach, for a thread whose creation we do
/// not own - a device callback, where the callback body is the only hook.
#define NX_PROFILE_SET_THREAD_NAME_ONCE(name) ((void)(name))
#define NX_PROFILE_FIBER_ENTER(name) ((void)(name))
#define NX_PROFILE_FIBER_LEAVE() ((void)0)
#define NX_PROFILE_PLOT(name, value) ((void)(name), (void)(value))
#define NX_PROFILE_MESSAGE(text) ((void)(text))
#define NX_PROFILE_ALLOC(ptr, size) ((void)(ptr), (void)(size))
#define NX_PROFILE_FREE(ptr) ((void)(ptr))

#endif
