# nxtest, the test framework every nx2d suite is written in.

add_library(nxtest OBJECT framework/nxtest.cpp)
add_library(nx::test ALIAS nxtest)

target_include_directories(nxtest PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(nxtest PRIVATE nx::foundation_options)
set_target_properties(nxtest PROPERTIES FOLDER "tests")

function(nxtest_discover_tests target)
    cmake_parse_arguments(ARG "" "PREFIX;TIMEOUT;DISCOVERY_TIMEOUT" "LABELS" ${ARGN})
    if (NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 120)
    endif ()
    if (NOT ARG_DISCOVERY_TIMEOUT)
        set(ARG_DISCOVERY_TIMEOUT 30)
    endif ()
    # A cross-compiled binary cannot list its cases here; it still builds, to
    # be run on its target.
    if (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
        return()
    endif ()

    set(ctest_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_tests.cmake")

    add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
            -D "TEST_EXECUTABLE=$<TARGET_FILE:${target}>"
            -D "CTEST_FILE=${ctest_file}"
            -D "TEST_PREFIX=${ARG_PREFIX}"
            -D "TEST_LABELS=${ARG_LABELS}"
            -D "TEST_TIMEOUT=${ARG_TIMEOUT}"
            -D "TEST_DISCOVERY_TIMEOUT=${ARG_DISCOVERY_TIMEOUT}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/framework/nxtest_discover.cmake"
            COMMENT "Discovering nxtest cases in ${target}"
            VERBATIM)

    if (NOT EXISTS "${ctest_file}")
        file(WRITE "${ctest_file}" "")
    endif ()

    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${ctest_file}")
endfunction()


set(NX_FOUNDATION_TEST_SOURCES
  core/test_foundation.cpp
  core/test_hash.cpp
  core/test_result.cpp
  core/test_enum.cpp
  core/test_handle.cpp
  core/test_scope_guard.cpp
  core/test_callable.cpp
  core/test_random.cpp
  core/test_sha256.cpp
  core/test_time.cpp
  core/test_fixed_step.cpp
  core/test_function_ref.cpp
  core/test_no_alloc.cpp

  containers/test_bit_array.cpp
  containers/test_blob.cpp
  containers/test_concurrent_queue.cpp
  containers/test_fixed_vector.cpp
  containers/test_radix_sort.cpp
  containers/test_flat_map.cpp
  containers/test_hash_map.cpp
  containers/test_hash_set.cpp
  containers/test_map.cpp
  containers/test_owned_blob_slice.cpp
  containers/test_ring_buffer.cpp
  containers/test_slot_map.cpp
  containers/test_small_vector.cpp
  containers/test_sparse_set.cpp
  containers/test_stream_compact.cpp
  containers/test_string_map.cpp
  containers/test_stable_pool.cpp
  containers/test_sync_slot_map.cpp

  strings/test_utf8_string.cpp
  strings/test_format.cpp
  strings/test_base64.cpp
  strings/test_id_string.cpp

  reflection/test_aggregate.cpp
  reflection/test_attributes.cpp
  reflection/test_layout.cpp
  reflection/test_type.cpp

  serialization/test_json.cpp
  serialization/test_ini.cpp
  serialization/test_number_parse.cpp
  serialization/test_arguments.cpp
  serialization/test_asset_contract.cpp
  serialization/test_json_binary.cpp
  serialization/test_resource_bundle.cpp
  serialization/test_json_util.cpp
  serialization/test_content_length.cpp
  serialization/test_reflect_binary.cpp

  simd/test_simd_arith.cpp
  simd/test_simd_bit.cpp
  simd/test_simd_complex.cpp
  simd/test_simd_math.cpp
  simd/test_simd_memory.cpp
  simd/test_simd_ops.cpp

  fibers/test_context.cpp
  fibers/test_scheduler.cpp
  fibers/test_stack.cpp
  fibers/test_sync.cpp

  threading/test_cancellation.cpp
  threading/test_sync.cpp
  threading/test_thread.cpp
  threading/test_thread_pool.cpp
  threading/test_serial_queue.cpp
  threading/test_parallel_collect.cpp

  platform/test_filesystem.cpp
  platform/test_shared_library.cpp
  platform/test_os.cpp
  platform/test_process.cpp
  platform/test_socket.cpp

  vfs/test_vfs.cpp
  vfs/test_async_io.cpp
  vfs/test_asset_pipeline.cpp
  vfs/test_vfs_zip.cpp
  vfs/test_pack_manifest.cpp

  diagnostics/test_crash.cpp
  diagnostics/test_log.cpp

  ecs/test_registry.cpp)

add_executable(nx_foundation_tests ${NX_FOUNDATION_TEST_SOURCES})

target_link_libraries(nx_foundation_tests
  PRIVATE nx::test nx::foundation nx::foundation_options)

target_compile_options(nx_foundation_tests PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/wd4127;/wd4018;/wd4389;/bigobj>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-self-move;-Wno-self-assign-overloaded;-Wno-sign-compare>)

set_target_properties(nx_foundation_tests PROPERTIES FOLDER "tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${NX_FOUNDATION_TEST_SOURCES})

nxtest_discover_tests(nx_foundation_tests LABELS foundation TIMEOUT 120)

# The simd tests again on the other backends: plain arrays on every target,
# AVX2 registers on x86-64 and SVE2 on AArch64, when this machine can run them
# or the build is for another one. SVE2 BitPerm is optional in SVE2, so its
# tests build only for a machine known to have it.
set(NX_SIMD_TEST_SOURCES
  simd/test_simd_arith.cpp
  simd/test_simd_bit.cpp
  simd/test_simd_complex.cpp
  simd/test_simd_math.cpp
  simd/test_simd_memory.cpp
  simd/test_simd_ops.cpp)

function(nx_add_simd_tests target prefix)
  add_executable(${target} ${NX_SIMD_TEST_SOURCES})
  target_link_libraries(${target}
    PRIVATE nx::test nx::foundation nx::foundation_options)
  target_compile_options(${target} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/wd4127;/wd4018;/wd4389;/bigobj>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-sign-compare> ${ARGN})
  set_target_properties(${target} PROPERTIES FOLDER "tests")
  nxtest_discover_tests(${target} PREFIX "${prefix}" LABELS foundation TIMEOUT 120)
endfunction()

nx_add_simd_tests(nx_simd_fixed_tests "fixed: " -DNX_SIMD_FORCE_FIXED=1)
# A target device may not run the whole foundation suite; give it the simd
# tests on their own.
if(CMAKE_CROSSCOMPILING)
  nx_add_simd_tests(nx_simd_tests "")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|amd64)$" AND NOT CMAKE_CROSSCOMPILING)
  if(MSVC)
    set(_nx_avx2_flags /arch:AVX2)
  else()
    set(_nx_avx2_flags -mavx2 -mfma)
  endif()
  include(CheckCXXSourceRuns)
  set(CMAKE_REQUIRED_QUIET ON)
  check_cxx_source_runs([[
    #if defined(_MSC_VER)
    #include <intrin.h>
    static bool avx2() {
      int r[4];
      __cpuid(r, 1);
      const bool fma = (r[2] >> 12) & 1, avx = (r[2] >> 28) & 1;
      __cpuidex(r, 7, 0);
      return fma && avx && ((r[1] >> 5) & 1);
    }
    #else
    static bool avx2() {
      return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    }
    #endif
    int main() { return avx2() ? 0 : 1; }
  ]] NX_FOUNDATION_HOST_RUNS_AVX2)
  unset(CMAKE_REQUIRED_QUIET)
  if(NX_FOUNDATION_HOST_RUNS_AVX2)
    nx_add_simd_tests(nx_simd_avx2_tests "avx2: " ${_nx_avx2_flags})
  endif()
  unset(_nx_avx2_flags)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" AND NOT MSVC)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-march=armv9-a+sve2 NX_FOUNDATION_HAS_SVE2_FLAG)
  set(_nx_build_sve2 ${CMAKE_CROSSCOMPILING})
  if(NX_FOUNDATION_HAS_SVE2_FLAG AND NOT CMAKE_CROSSCOMPILING)
    include(CheckCXXSourceRuns)
    set(CMAKE_REQUIRED_QUIET ON)
    check_cxx_source_runs([[
      #include <sys/auxv.h>
      #ifndef HWCAP2_SVE2
      #define HWCAP2_SVE2 (1 << 1)
      #endif
      int main() { return (getauxval(AT_HWCAP2) & HWCAP2_SVE2) ? 0 : 1; }
    ]] NX_FOUNDATION_HOST_RUNS_SVE2)
    set(_nx_build_sve2 ${NX_FOUNDATION_HOST_RUNS_SVE2})
    check_cxx_compiler_flag(-march=armv9-a+sve2+sve2-bitperm
                            NX_FOUNDATION_HAS_SVE2_BITPERM_FLAG)
    if(NX_FOUNDATION_HOST_RUNS_SVE2 AND NX_FOUNDATION_HAS_SVE2_BITPERM_FLAG)
      check_cxx_source_runs([[
        #include <sys/auxv.h>
        #ifndef HWCAP2_SVEBITPERM
        #define HWCAP2_SVEBITPERM (1 << 4)
        #endif
        int main() {
          return (getauxval(AT_HWCAP2) & HWCAP2_SVEBITPERM) ? 0 : 1;
        }
      ]] NX_FOUNDATION_HOST_RUNS_SVE2_BITPERM)
      if(NX_FOUNDATION_HOST_RUNS_SVE2_BITPERM)
        nx_add_simd_tests(nx_simd_sve2_bitperm_tests "sve2 bitperm: "
                          -march=armv9-a+sve2+sve2-bitperm)
      endif()
    endif()
    unset(CMAKE_REQUIRED_QUIET)
  endif()
  if(NX_FOUNDATION_HAS_SVE2_FLAG AND _nx_build_sve2)
    nx_add_simd_tests(nx_simd_sve2_tests "sve2: " -march=armv9-a+sve2)
  endif()
  unset(_nx_build_sve2)
endif()

# Compile the production locale fallback even on hosts whose standard library
# provides floating-point from_chars. This catches platform-only regressions
# without duplicating nx::foundation symbols in the test executable.
add_executable(nx_number_fallback_tests
  serialization/test_number_parse.cpp
  ../include/core/foundation/serialization/number_parse.cpp)
target_include_directories(nx_number_fallback_tests
  PRIVATE $<TARGET_PROPERTY:nx_foundation,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(nx_number_fallback_tests
  PRIVATE nx::test nx::foundation_options)
if(NX_FOUNDATION_MIMALLOC)
  target_link_libraries(nx_number_fallback_tests PRIVATE nx::mimalloc)
endif()
if(NX_FOUNDATION_TRACY)
  target_link_libraries(nx_number_fallback_tests PRIVATE Tracy::TracyClient)
endif()
target_compile_definitions(nx_number_fallback_tests
  PRIVATE NX_FORCE_LOCALE_NUMBER_FALLBACK=1
          $<TARGET_PROPERTY:nx_foundation,INTERFACE_COMPILE_DEFINITIONS>)
target_compile_options(nx_number_fallback_tests
  PRIVATE $<TARGET_PROPERTY:nx_foundation,INTERFACE_COMPILE_OPTIONS>)
set_target_properties(nx_number_fallback_tests PROPERTIES FOLDER "tests")
nxtest_discover_tests(nx_number_fallback_tests
  PREFIX "fallback: " LABELS foundation TIMEOUT 120)

add_library(nx_testlib MODULE platform/testlib/testlib.cpp)
set_target_properties(nx_testlib PROPERTIES
        FOLDER "tests"
        CXX_VISIBILITY_PRESET default
        PREFIX "")
add_dependencies(nx_foundation_tests nx_testlib)
target_compile_definitions(nx_foundation_tests
        PRIVATE NX_TESTLIB_PATH="$<TARGET_FILE:nx_testlib>")
