#pragma once

#include "core/foundation/core/foundation.h"

#if !defined(NX_PROFILE_ENABLED)
#define NX_PROFILE_ENABLED 0
#endif

#if NX_PROFILE_ENABLED

#include <tracy/Tracy.hpp>

#define NX_PROFILE_ZONE(name) ZoneScopedN(name)
#define NX_PROFILE_ZONE_COLOR(name, color) ZoneScopedNC(name, color)
#define NX_PROFILE_FUNCTION() ZoneScoped
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
#define NX_PROFILE_PLOT(name, value) TracyPlot(name, value)
#define NX_PROFILE_MESSAGE(text) TracyMessageL(text)
#define NX_PROFILE_ALLOC(ptr, size) TracyAlloc(ptr, size)
#define NX_PROFILE_FREE(ptr) TracyFree(ptr)

#else

#define NX_PROFILE_ZONE(name) ((void)(name))
#define NX_PROFILE_ZONE_COLOR(name, color) ((void)(name), (void)(color))
#define NX_PROFILE_FUNCTION() ((void)0)
#define NX_PROFILE_FRAME_MARK() ((void)0)
#define NX_PROFILE_FRAME_MARK_NAMED(name) ((void)(name))
#define NX_PROFILE_SET_THREAD_NAME(name) ((void)(name))
/// Names the calling thread on first reach, for a thread whose creation we do
/// not own - a device callback, where the callback body is the only hook.
#define NX_PROFILE_SET_THREAD_NAME_ONCE(name) ((void)(name))
#define NX_PROFILE_PLOT(name, value) ((void)(name), (void)(value))
#define NX_PROFILE_MESSAGE(text) ((void)(text))
#define NX_PROFILE_ALLOC(ptr, size) ((void)(ptr), (void)(size))
#define NX_PROFILE_FREE(ptr) ((void)(ptr))

#endif
