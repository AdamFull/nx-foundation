# nx foundation - the bottom layer of the engine: platform/compiler detection,
# allocators, containers, strings, threading, filesystem and logging.
#
# Mostly header-only, but the platform backends (threading, filesystem, os,
# logging) are real translation units, so this ships as a static library rather
# than an interface target.

set(NX_FOUNDATION_HEADERS
  core/callable.h
  core/enum.h
  core/foundation.h
  core/handle.h
  core/hash.h
  core/random.h
  core/result.h
  core/scope_guard.h
  core/time.h
  core/type_index.h

  reflection/aggregate.h
  reflection/attributes.h
  reflection/registry.h
  reflection/type.h

  containers/blob.h
  containers/concurrent_queue.h
  containers/fixed_vector.h
  containers/flat_map.h
  containers/hash_map.h
  containers/ring_buffer.h
  containers/slot_map.h
  containers/radix_sort.h
  containers/small_vector.h
  containers/sparse_set.h
  containers/stream_compact.h
  containers/string_map.h
  containers/stable_pool.h

  strings/format.h
  strings/id_string.h
  strings/utf8_string.h
  strings/utf8_string_view.h

  serialization/json.h
  serialization/json_util.h
  serialization/reflect_json.h

  threading/sync.h
  threading/thread.h
  threading/thread_pool.h

  platform/filesystem.h
  platform/native_window.h
  platform/os.h

  vfs/vfs.h
  vfs/vfs_internal.h

  diagnostics/diagnostic.h
  diagnostics/log.h
  diagnostics/profiler.h

  ecs/registry.h)

# Platform selection happens inside these files (#if NX_PLATFORM_*), so the
# source list is the same everywhere.
set(NX_FOUNDATION_SOURCES
  core/foundation.cpp
  core/random.cpp
  threading/sync.cpp
  threading/thread.cpp
  threading/thread_pool.cpp
  platform/filesystem.cpp
  platform/os.cpp
  vfs/vfs.cpp
  vfs/host_device.cpp
  vfs/memory_device.cpp
  vfs/zip_device.cpp
  diagnostics/log.cpp
  serialization/json.cpp
  reflection/registry.cpp
  serialization/reflect_json.cpp)

add_library(nx_foundation STATIC ${NX_FOUNDATION_SOURCES} ${NX_FOUNDATION_HEADERS})
add_library(nx::foundation ALIAS nx_foundation)

# Includes are rooted at engine/, so every include reads
# #include "foundation/<scope>/<file>.h" - unambiguous from anywhere.
target_include_directories(nx_foundation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

# PUBLIC because these are used from public headers: foundation/core/foundation.h
# includes <mimalloc.h>, and diagnostics/profiler.h includes <tracy/Tracy.hpp>
# when profiling is enabled. Linking TracyClient unconditionally keeps the link
# line the same across configs; it is inert unless TRACY_ENABLE is set.
# minizip-ng backs nx::vfs::ZipDevice, but only inside vfs/zip_device.cpp.
target_link_libraries(nx_foundation
  PUBLIC nx::mimalloc Tracy::TracyClient
  PRIVATE nx::compile_options nx::minizip)

if(WIN32)
  # Synchronization: WaitOnAddress / WakeByAddress* (threading/sync.cpp)
  # Psapi:           GetProcessMemoryInfo           (platform/os.cpp)
  target_link_libraries(nx_foundation PRIVATE Synchronization Psapi)
elseif(ANDROID)
  target_link_libraries(nx_foundation PRIVATE log)
elseif(UNIX)
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  target_link_libraries(nx_foundation PUBLIC Threads::Threads PRIVATE ${CMAKE_DL_LIBS})
endif()

# Debugger visualizers for the custom containers (MSVC embeds them in the PDB).
if(MSVC)
  target_sources(nx_foundation PRIVATE nx.natvis)
endif()

set_target_properties(nx_foundation PROPERTIES FOLDER "engine")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}"
  FILES ${NX_FOUNDATION_SOURCES} ${NX_FOUNDATION_HEADERS})
