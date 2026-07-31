#pragma once

/**
 * @file profiler.h
 * @brief Profiler instrumentation macros (namespace nx).
 *
 * The macros below are the only profiling surface engine code is allowed to
 * use. They compile to nothing unless NX_PROFILE_ENABLED is 1 (the profile
 * configuration; see CMakePresets.json), so call sites can be left in shipping
 * builds. When enabled they forward to the Tracy client; nothing outside this
 * header includes a Tracy header, so swapping backends touches only this file.
 *
 *     void worker_loop() {
 *       NX_PROFILE_SET_THREAD_NAME("nx-worker");
 *       for (;;) {
 *         NX_PROFILE_ZONE("worker tick");
 *         ...
 *         NX_PROFILE_FRAME_MARK();
 *       }
 *     }
 */

#include "foundation/core/foundation.h"

#if !defined(NX_PROFILE_ENABLED)
#define NX_PROFILE_ENABLED 0
#endif

#if NX_PROFILE_ENABLED

#include <tracy/Tracy.hpp>

/// Times the enclosing scope under a literal name.
#define NX_PROFILE_ZONE(name) ZoneScopedN(name)
/// Times the enclosing scope under a literal name and an explicit 0xRRGGBB
/// color.
#define NX_PROFILE_ZONE_COLOR(name, color) ZoneScopedNC(name, color)
/// Times the enclosing scope, named after the enclosing function.
#define NX_PROFILE_FUNCTION() ZoneScoped
/// Marks the end of a frame on the default frame timeline.
#define NX_PROFILE_FRAME_MARK() FrameMark
/// Marks the end of a frame on a named (secondary) timeline.
#define NX_PROFILE_FRAME_MARK_NAMED(name) FrameMarkNamed(name)
/// Names the calling thread in the profiler's thread list.
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
/// Records a value on a named plot.
#define NX_PROFILE_PLOT(name, value) TracyPlot(name, value)
/// Emits a one-off message (literal C string) into the profiler timeline.
#define NX_PROFILE_MESSAGE(text) TracyMessageL(text)
/// Reports an allocation / free to the profiler's memory view.
#define NX_PROFILE_ALLOC(ptr, size) TracyAlloc(ptr, size)
#define NX_PROFILE_FREE(ptr) TracyFree(ptr)

#else

/// Times the enclosing scope under a literal name.
#define NX_PROFILE_ZONE(name) ((void)(name))
/// Times the enclosing scope under a literal name and an explicit 0xRRGGBB
/// color.
#define NX_PROFILE_ZONE_COLOR(name, color) ((void)(name), (void)(color))
/// Times the enclosing scope, named after the enclosing function.
#define NX_PROFILE_FUNCTION() ((void)0)
/// Marks the end of a frame on the default frame timeline.
#define NX_PROFILE_FRAME_MARK() ((void)0)
/// Marks the end of a frame on a named (secondary) timeline.
#define NX_PROFILE_FRAME_MARK_NAMED(name) ((void)(name))
/// Names the calling thread in the profiler's thread list.
#define NX_PROFILE_SET_THREAD_NAME(name) ((void)(name))
/// Names the calling thread on first reach, for a thread whose creation we do
/// not own - a device callback, where the callback body is the only hook.
#define NX_PROFILE_SET_THREAD_NAME_ONCE(name) ((void)(name))
/// Records a value on a named plot.
#define NX_PROFILE_PLOT(name, value) ((void)(name), (void)(value))
/// Emits a one-off message (literal C string) into the profiler timeline.
#define NX_PROFILE_MESSAGE(text) ((void)(text))
/// Reports an allocation / free to the profiler's memory view.
#define NX_PROFILE_ALLOC(ptr, size) ((void)(ptr), (void)(size))
#define NX_PROFILE_FREE(ptr) ((void)(ptr))

#endif
