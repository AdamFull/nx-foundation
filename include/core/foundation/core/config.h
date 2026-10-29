#pragma once

// Platform, architecture, compiler and attribute macros, without any
// library header: what a header needs that should stay cheap to include.

#if defined(_WIN32) || defined(_WIN64) || defined(__WIN32__) ||                \
    defined(__WINDOWS__)
#define NX_PLATFORM_WINDOWS 1
#define NX_PLATFORM_NAME "Windows"
#elif defined(__ANDROID__)
#define NX_PLATFORM_ANDROID 1
#define NX_PLATFORM_NAME "Android"
#elif defined(__linux__) || defined(__gnu_linux__)
#define NX_PLATFORM_LINUX 1
#define NX_PLATFORM_NAME "Linux"
#elif defined(__APPLE__) && defined(__MACH__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
#define NX_PLATFORM_IOS 1
#define NX_PLATFORM_NAME "iOS"
#elif TARGET_OS_MAC
#define NX_PLATFORM_MACOS 1
#define NX_PLATFORM_NAME "macOS"
#endif
#else
#define NX_PLATFORM_UNKNOWN 1
#define NX_PLATFORM_NAME "Unknown"
#warning "Unknown platform detected"
#endif

#if defined(NX_PLATFORM_LINUX) || defined(NX_PLATFORM_ANDROID) ||              \
    defined(NX_PLATFORM_MACOS) || defined(NX_PLATFORM_IOS)
#define NX_PLATFORM_POSIX 1
#endif
#if defined(NX_PLATFORM_MACOS) || defined(NX_PLATFORM_IOS)
#define NX_PLATFORM_APPLE 1
#endif
#if defined(NX_PLATFORM_WINDOWS)
#define NX_HAS_WINDOWS_API 1
#endif
#if defined(NX_PLATFORM_ANDROID)
#define NX_HAS_ANDROID_NDK 1
#endif

#if defined(__x86_64__) || defined(__x86_64) || defined(__amd64__) ||          \
    defined(__amd64) || defined(_M_X64) || defined(_M_AMD64)
#define NX_ARCH_X64 1
#define NX_ARCH_NAME "x64"
#define NX_ARCH_BITS 64
#elif defined(i386) || defined(__i386) || defined(__i386__) ||                 \
    defined(_M_IX86) || defined(_X86_)
#define NX_ARCH_X86 1
#define NX_ARCH_NAME "x86"
#define NX_ARCH_BITS 32
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#define NX_ARCH_AARCH64 1
#define NX_ARCH_NAME "aarch64"
#define NX_ARCH_BITS 64
#elif defined(__arm__) || defined(_ARM) || defined(_M_ARM) || defined(__arm)
#define NX_ARCH_ARM 1
#define NX_ARCH_NAME "arm"
#define NX_ARCH_BITS 32
#else
#define NX_ARCH_UNKNOWN 1
#define NX_ARCH_NAME "Unknown"
#define NX_ARCH_BITS 0
#warning "Unknown architecture detected"
#endif

#if NX_ARCH_BITS == 64
#define NX_64BIT 1
#define NX_POINTER_SIZE 8
#elif NX_ARCH_BITS == 32
#define NX_32BIT 1
#define NX_POINTER_SIZE 4
#endif

// Both flags are set on purpose: it targets the MSVC ABI, which makes the __declspec attributes and
// [[msvc::no_unique_address]] below the correct ones for it, while Clang's builtins are available
// as well.
#if defined(__clang__) && defined(_MSC_VER)
#define NX_COMPILER_MSVC 1
#define NX_COMPILER_CLANG 1
#define NX_COMPILER_CLANG_CL 1
#define NX_COMPILER_NAME "clang-cl"
#define NX_COMPILER_VERSION                                                    \
  (__clang_major__ * 10000 + __clang_minor__ * 100 + __clang_patchlevel__)
#elif defined(_MSC_VER)
#define NX_COMPILER_MSVC 1
#define NX_COMPILER_NAME "MSVC"
#define NX_COMPILER_VERSION _MSC_VER
#elif defined(__clang__)
#define NX_COMPILER_CLANG 1
#define NX_COMPILER_NAME "Clang"
#define NX_COMPILER_VERSION                                                    \
  (__clang_major__ * 10000 + __clang_minor__ * 100 + __clang_patchlevel__)
#elif defined(__GNUC__)
#define NX_COMPILER_GCC 1
#define NX_COMPILER_NAME "GCC"
#define NX_COMPILER_VERSION                                                    \
  (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#else
#define NX_COMPILER_UNKNOWN 1
#define NX_COMPILER_NAME "Unknown"
#define NX_COMPILER_VERSION 0
#endif

#if defined(DEBUG) || defined(_DEBUG) || !defined(NDEBUG)
#define NX_DEBUG 1
#define NX_BUILD_CONFIG "Debug"
#else
#define NX_RELEASE 1
#define NX_BUILD_CONFIG "Release"
#endif

#if defined(NX_COMPILER_MSVC)
#define NX_FORCE_INLINE __forceinline
#define NX_NO_INLINE __declspec(noinline)
#define NX_DEPRECATED __declspec(deprecated)
#define NX_ALIGN(x) __declspec(align(x))
#define NX_UNREACHABLE() __assume(0)
#elif defined(NX_COMPILER_GCC) || defined(NX_COMPILER_CLANG)
#define NX_FORCE_INLINE __attribute__((always_inline)) inline
#define NX_NO_INLINE __attribute__((noinline))
#define NX_DEPRECATED __attribute__((deprecated))
#define NX_ALIGN(x) __attribute__((aligned(x)))
#define NX_UNREACHABLE() __builtin_unreachable()
#else
#define NX_FORCE_INLINE inline
#define NX_NO_INLINE
#define NX_DEPRECATED
#define NX_ALIGN(x)
#define NX_UNREACHABLE()
#endif

/// Prefer this form for public migrations: every supported C++ compiler emits
/// the replacement guidance at the call site instead of a context-free
/// deprecation warning.
#define NX_DEPRECATED_MSG(message) [[deprecated(message)]]

#if defined(NX_PLATFORM_WINDOWS)
#if defined(NX_BUILD_SHARED)
#define NX_API __declspec(dllexport)
#elif defined(NX_USE_SHARED)
#define NX_API __declspec(dllimport)
#else
#define NX_API
#endif
#elif defined(NX_COMPILER_GCC) || defined(NX_COMPILER_CLANG)
#define NX_API __attribute__((visibility("default")))
#else
#define NX_API
#endif

#if defined(NX_COMPILER_GCC) || defined(NX_COMPILER_CLANG)
#define NX_LIKELY(x) __builtin_expect(!!(x), 1)
#define NX_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define NX_LIKELY(x) (x)
#define NX_UNLIKELY(x) (x)
#endif

#if defined(__SANITIZE_ADDRESS__)
#define NX_ASAN_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NX_ASAN_ENABLED 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define NX_TSAN_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define NX_TSAN_ENABLED 1
#endif
#endif

#if defined(NX_COMPILER_MSVC)
#define NX_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define NX_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

// Marks a parameter whose referenced storage may be reachable through the return value, so a caller
// passing a temporary gets a dangling-reference diagnostic instead of a use-after-free at run time.
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(msvc::lifetimebound)
#define NX_LIFETIMEBOUND [[msvc::lifetimebound]]
#elif __has_cpp_attribute(clang::lifetimebound)
#define NX_LIFETIMEBOUND [[clang::lifetimebound]]
#else
#define NX_LIFETIMEBOUND
#endif
#else
#define NX_LIFETIMEBOUND
#endif
