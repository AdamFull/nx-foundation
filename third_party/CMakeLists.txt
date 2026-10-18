# Vendored dependencies (git submodules). Every upstream build option is pinned
# here so nothing above this directory has to know about them.

set(NX_FOUNDATION_SUBMODULES minizip-ng zlib-ng zstd)
if(NX_FOUNDATION_MIMALLOC)
  list(APPEND NX_FOUNDATION_SUBMODULES mimalloc)
endif()
if(NX_FOUNDATION_TRACY)
  list(APPEND NX_FOUNDATION_SUBMODULES tracy)
endif()

foreach(_submodule ${NX_FOUNDATION_SUBMODULES})
  if(_submodule STREQUAL "zstd")
    set(_submodule_manifest "zstd/build/cmake/CMakeLists.txt")
  else()
    set(_submodule_manifest "${_submodule}/CMakeLists.txt")
  endif()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_submodule_manifest}")
    message(FATAL_ERROR
      "nx-foundation's third_party/${_submodule} is empty. Fetch the "
      "submodules first:\n  git submodule update --init --recursive")
  endif()
endforeach()
unset(_submodule_manifest)

# Directory-scoped: third-party suites should never land in a ctest list.
set(BUILD_TESTING OFF)
# Static, all of them: zlib-ng otherwise builds a DLL too, and minizip-ng's
# fetch path links that one, so every executable needed it beside it.
set(BUILD_SHARED_LIBS OFF)

# mimalloc

if(NX_FOUNDATION_MIMALLOC)
  set(MI_OVERRIDE      OFF CACHE BOOL "" FORCE)
  set(MI_BUILD_SHARED  OFF CACHE BOOL "" FORCE)
  set(MI_BUILD_STATIC  ON  CACHE BOOL "" FORCE)
  set(MI_BUILD_OBJECT  OFF CACHE BOOL "" FORCE)
  set(MI_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
  # Otherwise it forces -march=armv8.1-a, whose atomics SIGILL on ARMv8.0.
  set(MI_NO_OPT_ARCH   ON  CACHE BOOL "" FORCE)
  set(MI_DEBUG_FULL    OFF CACHE BOOL "" FORCE)
  set(MI_SHOW_ERRORS   OFF CACHE BOOL "" FORCE)

  add_subdirectory(mimalloc EXCLUDE_FROM_ALL)

  add_library(nx::mimalloc ALIAS mimalloc-static)

  set_target_properties(mimalloc-static PROPERTIES FOLDER "third_party")

  # mimalloc.h is not our code: keep its warnings out of our builds.
  get_target_property(_mi_includes mimalloc-static INTERFACE_INCLUDE_DIRECTORIES)
  set_target_properties(mimalloc-static PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_mi_includes}")
  unset(_mi_includes)
endif()

# tracy

# TRACY_ENABLE decides whether the client instruments anything. Tracking
# NX_ENABLE_PROFILER means the instrumentation is compiled in for a profiling
# build and stripped everywhere else, where TracyClient builds down to an
# essentially empty object.
if(NX_FOUNDATION_TRACY)
  set(TRACY_STATIC    ON                    CACHE BOOL "" FORCE)
  set(TRACY_ENABLE    ${NX_ENABLE_PROFILER} CACHE BOOL "" FORCE)
  # Capture starts only once a profiler connects, so an instrumented build
  # still runs normally (and at full speed) when nobody is attached.
  set(TRACY_ON_DEMAND ON                    CACHE BOOL "" FORCE)
  # Jobs run on nx::fiber fibers that migrate between threads; Tracy has to
  # follow them or zones spanning a suspension end on the wrong thread.
  set(TRACY_FIBERS    ON                    CACHE BOOL "" FORCE)
  # A profiler connects over loopback or adb forward. Listening on every
  # interface would also have the OS ask, for each new executable, to let it
  # through the firewall.
  set(TRACY_ONLY_LOCALHOST ON               CACHE BOOL "" FORCE)

  # Tracy's standalone build opportunistically installs ccache as a global
  # compile and link launcher. As a dependency it must not rewrite the parent
  # project's toolchain, especially on Windows where cached PDB-producing links
  # can stall. Preserve whatever launcher the project or user selected.
  get_property(_nx_rule_launch_compile GLOBAL PROPERTY RULE_LAUNCH_COMPILE)
  get_property(_nx_rule_launch_link GLOBAL PROPERTY RULE_LAUNCH_LINK)
  add_subdirectory(tracy EXCLUDE_FROM_ALL)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${_nx_rule_launch_compile}")
  set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK "${_nx_rule_launch_link}")
  unset(_nx_rule_launch_compile)
  unset(_nx_rule_launch_link)

  # Tracy already provides Tracy::TracyClient and marks its include dir SYSTEM.
  set_target_properties(TracyClient PROPERTIES FOLDER "third_party")
endif()

# Zstandard

# ZIP method 93 (minizip-ng below). The targets are also the one Zstd build
# for whatever else in the parent project needs it.
set(ZSTD_BUILD_PROGRAMS       OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB        OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS          OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED         OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC         ON  CACHE BOOL "" FORCE)
# Packages are written by host tools and read at runtime. minizip-ng keeps both
# directions in one stream translation unit, so retain the small compression
# half in runtime builds too.
set(ZSTD_BUILD_COMPRESSION    ON  CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DECOMPRESSION  ON  CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DICTBUILDER    OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT       OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT  OFF CACHE BOOL "" FORCE)
add_subdirectory(zstd/build/cmake EXCLUDE_FROM_ALL)
add_library(nx::zstd ALIAS libzstd_static)
add_library(zstd::libzstd_static ALIAS libzstd_static)
add_library(zstd::libzstd ALIAS libzstd)
# zstd's build-tree package includes zstdTargets.cmake, but that export is not
# generated until CMake's generate phase. minizip-ng calls find_package(zstd)
# during the configure phase, so point it at a target adapter rather than at
# that incomplete build-tree package.
set(zstd_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/packages/zstd"
    CACHE PATH "Vendored Zstandard package adapter" FORCE)
set_target_properties(libzstd_static PROPERTIES FOLDER "third_party")

# minizip-ng

# ZIP archive backend for the VFS (nx::vfs::ZipDevice); an APK is a ZIP, so the
# same device serves Android assets.
#
# minizip-ng would FetchContent-clone zlib, which needs the network and re-runs
# a flaky git update on every reconfigure. FetchContent honours the first
# declaration of a name, so declaring `zlib` with a local SOURCE_DIR points it
# at our submodule with no git at all.
include(FetchContent)
FetchContent_Declare(zlib
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/zlib-ng"
  EXCLUDE_FROM_ALL)

set(WITH_GTEST        OFF CACHE BOOL   "" FORCE)
# zlib-ng validates this against a fixed list and hard-errors on anything else,
# so an "AUTO" left in the cache by an older zlib-ng would fail configure.
set(WITH_SANITIZER    OFF CACHE STRING "" FORCE)

set(MZ_COMPAT           OFF CACHE BOOL   "" FORCE) # modern mz_* API, no zlib in headers
set(MZ_ZLIB             ON  CACHE BOOL   "" FORCE) # deflate: the APK's default
# Pinned away from "auto": on a host with a system zlib (e.g. ubuntu-latest),
# auto's find_package(ZLIB) would succeed and hand back a target scoped to
# minizip-ng's own directory tree, invisible to a parent project's sibling
# `if(TARGET ZLIB::ZLIB)` check. Forcing the fetch path instead resolves
# through the local FetchContent redirect above (no network) and produces
# zlib-ng::zlibstatic as a real ALIAS of a non-imported target, which - unlike
# an imported find_package() target - is visible project-wide regardless of
# which directory built it. MZ_FETCH_LIBS defaults to ${WIN32}; force it on
# for every platform so this doesn't depend on whether a system zlib happens
# to be findable.
set(MZ_ZLIB_FLAVOR      "zlib-ng" CACHE STRING "" FORCE)
set(MZ_FETCH_LIBS       ON  CACHE BOOL   "" FORCE)
set(MZ_ZSTD             ON  CACHE BOOL   "" FORCE) # method 93 entries
set(MZ_BZIP2            OFF CACHE BOOL   "" FORCE)
set(MZ_LZMA             OFF CACHE BOOL   "" FORCE)
set(MZ_PPMD             OFF CACHE BOOL   "" FORCE)
set(MZ_PKCRYPT          OFF CACHE BOOL   "" FORCE)
set(MZ_WZAES            OFF CACHE BOOL   "" FORCE)
set(MZ_OPENSSL          OFF CACHE BOOL   "" FORCE)
set(MZ_LIBBSD           OFF CACHE BOOL   "" FORCE)
set(MZ_ICONV            OFF CACHE BOOL   "" FORCE)
set(MZ_BUILD_TESTS      OFF CACHE BOOL   "" FORCE)
set(MZ_BUILD_UNIT_TESTS OFF CACHE BOOL   "" FORCE)
set(MZ_LIB_SUFFIX       ""  CACHE STRING "" FORCE) # target name -> "minizip"

add_subdirectory(minizip-ng EXCLUDE_FROM_ALL)
add_library(nx::minizip ALIAS minizip)
set_target_properties(minizip PROPERTIES FOLDER "third_party")

# Upstream leaves a Win32 attribute variable unused in configurations without
# file security metadata. Keep that vendored diagnostic out of clean builds.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
  target_compile_options(minizip PRIVATE -Wno-unused-variable)
endif()

# minizip headers are not our code: keep their warnings out of our builds.
get_target_property(_mz_includes minizip INTERFACE_INCLUDE_DIRECTORIES)
if(_mz_includes)
  set_target_properties(minizip PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_mz_includes}")
endif()
unset(_mz_includes)
