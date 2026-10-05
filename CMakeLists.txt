
set(NX_FOUNDATION_HEADERS
  core/callable.h
  core/enum.h
  core/foundation.h
  core/handle.h
  core/hash.h
  core/random.h
  core/result.h
  core/scope_guard.h
  core/sha256.h
  core/time.h
  core/type_index.h

  reflection/aggregate.h
  reflection/attributes.h
  reflection/layout.h
  reflection/registry.h
  reflection/type.h

  containers/blob.h
  containers/concurrent_queue.h
  containers/fixed_vector.h
  containers/flat_map.h
  containers/hash_map.h
  containers/hash_set.h
  containers/owned_blob_slice.h
  containers/ring_buffer.h
  containers/slot_map.h
  containers/radix_sort.h
  containers/raw_hash_table.h
  containers/small_vector.h
  containers/sparse_set.h
  containers/stream_compact.h
  containers/string_map.h
  containers/string_set.h
  containers/stable_pool.h
  containers/sync_slot_map.h

  strings/format.h
  strings/base64.h
  strings/id_string.h
  strings/utf8_string.h
  strings/utf8_string_view.h

  serialization/json.h
  serialization/ini.h
  serialization/number_parse.h
  serialization/arguments.h
  serialization/asset_contract.h
  serialization/asset_policy.h
  serialization/json_binary.h
  serialization/json_document.h
  serialization/resource_bundle.h
  serialization/json_util.h
  serialization/content_length.h
  serialization/nva.h
  serialization/reflect_json.h
  serialization/reflect_binary.h

  fibers/context.h
  fibers/scheduler.h
  fibers/stack.h
  fibers/sync.h
  fibers/wait.h

  threading/cancellation.h
  threading/sync.h
  threading/thread.h
  threading/serial_queue.h
  threading/thread_pool.h
  threading/workload.h

  platform/filesystem.h
  platform/windows_path.h
  platform/native_window.h
  platform/os.h
  platform/process.h
  platform/shared_library.h
  platform/android_jni.h
  platform/socket.h

  vfs/vfs.h
  vfs/vfs_internal.h
  vfs/async_io.h
  vfs/native_async_io.h
  vfs/asset_pipeline.h
  vfs/pack_manifest.h

  diagnostics/crash.h
  diagnostics/diagnostic.h
  diagnostics/log.h
  diagnostics/profiler.h

  ecs/registry.h)

set(NX_FOUNDATION_SOURCES
  core/foundation.cpp
  core/random.cpp
  core/sha256.cpp
  fibers/context.cpp
  fibers/scheduler.cpp
  fibers/stack.cpp
  fibers/sync.cpp
  threading/cancellation.cpp
  threading/sync.cpp
  threading/thread.cpp
  threading/serial_queue.cpp
  threading/thread_pool.cpp
  platform/filesystem.cpp
  platform/os.cpp
  platform/process.cpp
  platform/shared_library.cpp
  platform/socket.cpp
  vfs/vfs.cpp
  vfs/async_io.cpp
  vfs/pack_manifest.cpp
  vfs/host_device.cpp
  vfs/memory_device.cpp
  vfs/zip_device.cpp
  diagnostics/crash.cpp
  diagnostics/log.cpp
  serialization/json.cpp
  serialization/ini.cpp
  serialization/number_parse.cpp
  serialization/arguments.cpp
  serialization/asset_contract.cpp
  serialization/json_binary.cpp
  serialization/json_document.cpp
  serialization/resource_bundle.cpp
  serialization/content_length.cpp
  reflection/layout.cpp
  reflection/registry.cpp
  serialization/reflect_json.cpp
  serialization/reflect_binary.cpp)

# Enabled here rather than at the root: a root-level ASM_MASM breaks zstd's project(... C ASM).
if(MSVC)
  enable_language(ASM_MASM)
else()
  enable_language(ASM)
  list(APPEND NX_FOUNDATION_SOURCES
    fibers/context_aarch64.S fibers/context_x64_sysv.S)
endif()

if(WIN32)
  list(APPEND NX_FOUNDATION_SOURCES vfs/native_async_io_iocp.cpp)
elseif(APPLE)
  list(APPEND NX_FOUNDATION_SOURCES vfs/native_async_io_dispatch.mm)
elseif(ANDROID)
  # Android seccomp policies may terminate a process which probes io_uring,
  # rather than returning a recoverable error. Provider-backed reads (APK,
  # packages and app storage) use AsyncIoService's bounded VFS worker pool.
  list(APPEND NX_FOUNDATION_SOURCES vfs/native_async_io_fallback.cpp
    platform/android_jni.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND NX_FOUNDATION_SOURCES vfs/native_async_io_uring.cpp)
else()
  list(APPEND NX_FOUNDATION_SOURCES vfs/native_async_io_fallback.cpp)
endif()

if(APPLE)
  set_source_files_properties(vfs/native_async_io_dispatch.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

add_library(nx_foundation STATIC ${NX_FOUNDATION_SOURCES} ${NX_FOUNDATION_HEADERS})
add_library(nx::foundation ALIAS nx_foundation)

if(MSVC)
  # Assembled apart from nx_foundation so ml64 is not handed nx::compile_options' C++ switches.
  add_library(nx_foundation_fiber_asm OBJECT fibers/context_x64_win.asm)
  target_compile_options(nx_foundation_fiber_asm PRIVATE /nologo)
  set_target_properties(nx_foundation_fiber_asm PROPERTIES FOLDER "engine")
  target_sources(nx_foundation PRIVATE $<TARGET_OBJECTS:nx_foundation_fiber_asm>)
endif()

target_include_directories(nx_foundation PUBLIC "${NX_ENGINE_ROOT}/engine")

target_link_libraries(nx_foundation
  PUBLIC nx::mimalloc Tracy::TracyClient
  PRIVATE nx::compile_options nx::minizip)

if(WIN32)
  target_link_libraries(nx_foundation PRIVATE
    Synchronization Psapi ws2_32 Dbghelp Shell32 Ole32 Bcrypt)
elseif(APPLE)
  target_link_libraries(nx_foundation PRIVATE "-framework CoreFoundation")
elseif(ANDROID)
  target_link_libraries(nx_foundation PRIVATE log)
elseif(UNIX)
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  target_link_libraries(nx_foundation PUBLIC Threads::Threads PRIVATE ${CMAKE_DL_LIBS})
endif()

if(MSVC)
  target_sources(nx_foundation PRIVATE nx.natvis)
endif()

set_target_properties(nx_foundation PROPERTIES FOLDER "engine")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}"
  FILES ${NX_FOUNDATION_SOURCES} ${NX_FOUNDATION_HEADERS})
