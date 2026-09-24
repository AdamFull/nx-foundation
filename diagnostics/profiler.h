#pragma once

#include "core/foundation/core/foundation.h"

#if !defined(NX_PROFILE_ENABLED)
#define NX_PROFILE_ENABLED 0
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
