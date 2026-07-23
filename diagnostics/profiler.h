#pragma once

/**
 * @file profiler.h
 * @brief Profiler instrumentation macros (namespace nx).
 *
 * The macros below are the only profiling surface engine code is allowed to
 * use. They compile to nothing unless NX_PROFILE_ENABLED is 1, so call sites
 * can be left in shipping builds. A real backend (Tracy) is not wired up yet;
 * when it is, only this header changes.
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

#error "NX_PROFILE_ENABLED is set but no profiler backend is compiled in"

#else

/// Times the enclosing scope under a literal name.
#define NX_PROFILE_ZONE(name) ((void)(name))
/// Times the enclosing scope under a literal name and an explicit color.
#define NX_PROFILE_ZONE_COLOR(name, color) ((void)(name), (void)(color))
/// Times the enclosing scope, named after the enclosing function.
#define NX_PROFILE_FUNCTION() ((void)0)
/// Marks the end of a frame on the default frame timeline.
#define NX_PROFILE_FRAME_MARK() ((void)0)
/// Marks the end of a frame on a named (secondary) timeline.
#define NX_PROFILE_FRAME_MARK_NAMED(name) ((void)(name))
/// Names the calling thread in the profiler's thread list.
#define NX_PROFILE_SET_THREAD_NAME(name) ((void)(name))
/// Records a value on a named plot.
#define NX_PROFILE_PLOT(name, value) ((void)(name), (void)(value))
/// Emits a one-off message into the profiler timeline.
#define NX_PROFILE_MESSAGE(text) ((void)(text))
/// Reports an allocation / free to the profiler's memory view.
#define NX_PROFILE_ALLOC(ptr, size) ((void)(ptr), (void)(size))
#define NX_PROFILE_FREE(ptr) ((void)(ptr))

#endif
