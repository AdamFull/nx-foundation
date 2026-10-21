cmake_minimum_required(VERSION 3.21)
project(nx_foundation LANGUAGES C CXX)

# The nx2d engine's standard library, usable on its own: add it with
# add_subdirectory() (after `git submodule update --init --recursive`) and link
# nx::foundation. Its headers are included as "core/foundation/...".
option(NX_FOUNDATION_MIMALLOC "Allocate through mimalloc (off: the system allocator)" ON)
option(NX_FOUNDATION_TRACY "Build Tracy's client, the backend NX_ENABLE_PROFILER instruments with" ON)
option(NX_ENABLE_PROFILER "Compile profiler instrumentation (needs NX_FOUNDATION_TRACY)" OFF)
option(NX_SHIPPING "Build for a sealed release: quieter logs, cooked assets only" OFF)
option(NX_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(NX_FOUNDATION_TESTS "Build the foundation tests and nxtest" ${PROJECT_IS_TOP_LEVEL})

if(NX_ENABLE_PROFILER AND NOT NX_FOUNDATION_TRACY)
  message(FATAL_ERROR "NX_ENABLE_PROFILER needs NX_FOUNDATION_TRACY")
endif()

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)
endif()

# The warning and toolchain policy foundation is built with, for whoever wants
# to build their own code the same way (nx2d's nx::compile_options does).
add_library(nx_foundation_options INTERFACE)
add_library(nx::foundation_options ALIAS nx_foundation_options)

target_compile_features(nx_foundation_options INTERFACE cxx_std_20)
target_compile_definitions(nx_foundation_options INTERFACE
  $<$<PLATFORM_ID:Windows>:NOMINMAX;WIN32_LEAN_AND_MEAN>
  # The CRT headers gate on these regardless of which compiler reads them, so
  # this keys on the ABI - true for clang-cl too - and not on the compiler.
  $<$<BOOL:${MSVC}>:_CRT_SECURE_NO_WARNINGS;_CRT_NONSTDC_NO_WARNINGS>)

# Clang 21 (the NDK's) folded -Wmissing-designated-field-initializers into
# -Wextra. It fires on a designated initializer that leaves out a field with a
# default member initializer, which is how descriptors are written throughout,
# and with NX_WARNINGS_AS_ERRORS it fails the build. Probed by its positive
# spelling: clang accepts an unknown -Wno-<name> quietly until some other
# diagnostic fires, so testing the opt-out itself would pass on compilers that
# do not have it.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wmissing-designated-field-initializers
  NX_HAS_WMISSING_DESIGNATED_FIELD_INITIALIZERS)
set(_nx_warning_optouts "")
if(NX_HAS_WMISSING_DESIGNATED_FIELD_INITIALIZERS)
  set(_nx_warning_optouts -Wno-missing-designated-field-initializers)
endif()

# Three toolchains, not two. clang-cl is Clang behind cl's command line: it
# needs the MSVC-ABI switches but speaks Clang's warning vocabulary, and
# FRONTEND_VARIANT is what separates it from cl.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  target_compile_options(nx_foundation_options INTERFACE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /MP /EHsc
    /volatile:iso /wd4324 /GT)
elseif(MSVC)
  # Without /EHsc clang-cl compiles with exceptions off and every try/catch is
  # an error.
  target_compile_options(nx_foundation_options INTERFACE
    /W4 /utf-8 /EHsc -Wno-unused-parameter ${_nx_warning_optouts})
else()
  target_compile_options(nx_foundation_options INTERFACE
    -Wall -Wextra -Wno-unused-parameter ${_nx_warning_optouts})
endif()
unset(_nx_warning_optouts)

if(NX_WARNINGS_AS_ERRORS)
  target_compile_options(nx_foundation_options INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/WX>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Werror>)
endif()

# A short binary prefix: some upstream object paths are long, and Windows
# limits a full object path to 250 characters.
add_subdirectory(third_party 3p)

set(NX_FOUNDATION_HEADERS
  core/callable.h
  core/enum.h
  core/fixed.h
  core/fixed_step.h
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

  containers/bit_array.h
  containers/blob.h
  containers/btree.h
  containers/concurrent_queue.h
  containers/fixed_vector.h
  containers/flat_map.h
  containers/hash_map.h
  containers/hash_set.h
  containers/map.h
  containers/owned_blob_slice.h
  containers/ring_buffer.h
  containers/slot_map.h
  containers/radix_sort.h
  containers/raw_hash_table.h
  containers/slot_policy.h
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
  serialization/json_member_index.h
  serialization/json_document.h
  serialization/resource_bundle.h
  serialization/json_util.h
  serialization/content_length.h
  serialization/zstd.h
  serialization/nva.h
  serialization/reflect_json.h
  serialization/reflect_binary.h

  simd/algorithm.h
  simd/backend_arm.h
  simd/backend_complex.h
  simd/backend_generic.h
  simd/backend_vector.h
  simd/backend_x86.h
  simd/bit.h
  simd/bit_scalar.h
  simd/complex.h
  simd/detail.h
  simd/interleave.h
  simd/math.h
  simd/pipeline.h
  simd/simd.h
  simd/vec.h

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
  diagnostics/diagnostic.cpp
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
  serialization/zstd.cpp
  reflection/layout.cpp
  reflection/registry.cpp
  serialization/reflect_json.cpp
  serialization/reflect_binary.cpp)

list(TRANSFORM NX_FOUNDATION_HEADERS PREPEND "include/core/foundation/")
list(TRANSFORM NX_FOUNDATION_SOURCES PREPEND "include/core/foundation/")

# Enabled here rather than at the root: a root-level ASM_MASM breaks zstd's
# project(... C ASM).
if(MSVC)
  enable_language(ASM_MASM)
else()
  enable_language(ASM)
  list(APPEND NX_FOUNDATION_SOURCES
    include/core/foundation/fibers/context_aarch64.S
    include/core/foundation/fibers/context_x64_sysv.S)
endif()

if(WIN32)
  list(APPEND NX_FOUNDATION_SOURCES
    include/core/foundation/vfs/native_async_io_iocp.cpp)
elseif(APPLE)
  list(APPEND NX_FOUNDATION_SOURCES
    include/core/foundation/vfs/native_async_io_dispatch.mm)
elseif(ANDROID)
  # Android seccomp policies may terminate a process which probes io_uring,
  # rather than returning a recoverable error. Provider-backed reads (APK,
  # packages and app storage) use AsyncIoService's bounded VFS worker pool.
  list(APPEND NX_FOUNDATION_SOURCES
    include/core/foundation/vfs/native_async_io_fallback.cpp
    include/core/foundation/platform/android_jni.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND NX_FOUNDATION_SOURCES
    include/core/foundation/vfs/native_async_io_uring.cpp)
else()
  list(APPEND NX_FOUNDATION_SOURCES
    include/core/foundation/vfs/native_async_io_fallback.cpp)
endif()

if(APPLE)
  set_source_files_properties(include/core/foundation/vfs/native_async_io_dispatch.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

add_library(nx_foundation STATIC ${NX_FOUNDATION_SOURCES} ${NX_FOUNDATION_HEADERS})
add_library(nx::foundation ALIAS nx_foundation)

if(MSVC)
  # Assembled apart from nx_foundation so ml64 is not handed the C++ switches.
  add_library(nx_foundation_fiber_asm OBJECT
    include/core/foundation/fibers/context_x64_win.asm)
  target_compile_options(nx_foundation_fiber_asm PRIVATE /nologo)
  set_target_properties(nx_foundation_fiber_asm PROPERTIES FOLDER "foundation")
  target_sources(nx_foundation PRIVATE $<TARGET_OBJECTS:nx_foundation_fiber_asm>)
endif()

target_include_directories(nx_foundation PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_features(nx_foundation PUBLIC cxx_std_20)

# The headers read these, so whatever includes them must agree with the
# library.
target_compile_definitions(nx_foundation PUBLIC
  $<$<NOT:$<BOOL:${NX_FOUNDATION_MIMALLOC}>>:NX_USE_MIMALLOC=0>
  $<$<BOOL:${NX_ENABLE_PROFILER}>:NX_PROFILE_ENABLED=1>
  $<$<BOOL:${NX_SHIPPING}>:NX_BUILD_SHIPPING=1>)

if(MSVC)
  # The headers hold UTF-8 text and use __VA_OPT__.
  target_compile_options(nx_foundation PUBLIC
    $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/Zc:preprocessor>)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  # /GT stops the optimizer reusing a thread-local address across a call: a
  # job can resume on another thread after any wait, in the caller's code as
  # much as here. Clang has no such switch, so the scheduler still reads its
  # own thread-locals through non-inlined calls.
  target_compile_options(nx_foundation PUBLIC $<$<COMPILE_LANGUAGE:CXX>:/GT>)
endif()

target_link_libraries(nx_foundation PRIVATE nx::foundation_options nx::minizip
  nx::zstd)
if(NX_FOUNDATION_MIMALLOC)
  target_link_libraries(nx_foundation PUBLIC nx::mimalloc)
endif()
if(NX_FOUNDATION_TRACY)
  target_link_libraries(nx_foundation PUBLIC Tracy::TracyClient)
endif()

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

set_target_properties(nx_foundation PROPERTIES FOLDER "foundation")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include/core/foundation"
  FILES ${NX_FOUNDATION_SOURCES} ${NX_FOUNDATION_HEADERS})

if(NX_FOUNDATION_TESTS)
  if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
  endif()
  add_subdirectory(tests)
endif()
