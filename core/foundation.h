#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>
#include <cassert>

// Platform detection

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

#if defined(NX_PLATFORM_LINUX) || defined(NX_PLATFORM_ANDROID)
#define NX_PLATFORM_POSIX 1
#endif
#if defined(NX_PLATFORM_WINDOWS)
#define NX_HAS_WINDOWS_API 1
#endif
#if defined(NX_PLATFORM_ANDROID)
#define NX_HAS_ANDROID_NDK 1
#endif

#define NX_USE_MIMALLOC 1
#if NX_USE_MIMALLOC
#include <mimalloc.h>
#else
#include <cstdlib>
#include <cstring>
#if defined(NX_PLATFORM_WINDOWS)
#include <malloc.h> // _aligned_malloc, _aligned_realloc, _aligned_free
#elif defined(NX_PLATFORM_MACOS)
#include <malloc/malloc.h> // malloc_size
#else
#include <malloc.h> // malloc_usable_size
#endif
#endif

// Architecture detection

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

// Compiler detection

#if defined(_MSC_VER)
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

// Build config

#if defined(DEBUG) || defined(_DEBUG) || !defined(NDEBUG)
#define NX_DEBUG 1
#define NX_BUILD_CONFIG "Debug"
#else
#define NX_RELEASE 1
#define NX_BUILD_CONFIG "Release"
#endif

// Attributes

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

#if defined(NX_COMPILER_MSVC)
#define NX_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define NX_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

// Marks a parameter whose referenced storage may be reachable through the
// return value, so a caller passing a temporary gets a dangling-reference
// diagnostic instead of a use-after-free at run time. Use it on any parameter
// that a function can hand back, directly or inside a view.
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

#if defined(NX_COMPILER_MSVC)
#if defined(NX_ARCH_X64)
#include <immintrin.h>
#elif defined(NX_ARCH_AARCH64)
#include <intrin.h>
#endif
#endif

// Type aliases

using i8 = int8_t;
using u8 = uint8_t;
using i16 = int16_t;
using u16 = uint16_t;
using i32 = int32_t;
using u32 = uint32_t;
using i64 = int64_t;
using u64 = uint64_t;
using f32 = float;
using f64 = double;
using isize = ptrdiff_t;
using usize = size_t;

// Math utilities

namespace nx {

namespace detail {

/// Hints the CPU that the caller is in a spin-wait loop (x86 PAUSE / ARM
/// YIELD). Lowers power draw and frees pipeline resources for the sibling
/// hyper-thread.
NX_FORCE_INLINE void cpu_relax() noexcept {
#if defined(NX_ARCH_X64)
#if defined(NX_COMPILER_MSVC)
  _mm_pause();
#else
  __builtin_ia32_pause();
#endif
#elif defined(NX_ARCH_AARCH64)
#if defined(NX_COMPILER_MSVC)
  __yield();
#else
  __asm__ volatile("yield" ::: "memory");
#endif
#endif
}
} // namespace detail

template <typename T>
concept UnsignedArithmetic = std::is_unsigned_v<T> && std::is_arithmetic_v<T>;
template <typename T>
concept Arithmetic = std::is_arithmetic_v<T>;

template <UnsignedArithmetic T> constexpr bool is_pow2(T x) noexcept {
  return x != 0 && (x & (x - 1)) == 0;
}

template <UnsignedArithmetic T> constexpr T next_pow2(T x) noexcept {
  if (x == 0)
    return 1;
  --x;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  if constexpr (sizeof(T) == 8)
    x |= x >> 32;
  return x + 1;
}

template <UnsignedArithmetic T> constexpr T align_up(T x, T align) noexcept {
  return (x + align - 1) & ~(align - 1);
}

template <UnsignedArithmetic T> constexpr T align_down(T x, T align) noexcept {
  return x & ~(align - 1);
}

template <Arithmetic T> constexpr T min(T a, T b) noexcept {
  return a < b ? a : b;
}
template <Arithmetic T> constexpr T max(T a, T b) noexcept {
  return a > b ? a : b;
}
template <Arithmetic T> constexpr T clamp(T x, T lo, T hi) noexcept {
  return x < lo ? lo : (x > hi ? hi : x);
}

template <typename T, usize N>
[[nodiscard]] constexpr usize array_size(T (&)[N]) noexcept {
  return N;
}

// Bounded string copy (strlcpy-like): writes at most dst_size-1 characters
// from src into dst and always NUL-terminates (unless dst_size == 0). Returns
// the number of characters written, excluding the terminator.
inline usize str_copy(char *dst, usize dst_size, const char *src) noexcept {
  if (dst_size == 0)
    return 0;
  usize i = 0;
  for (; src[i] != '\0' && i + 1 < dst_size; ++i)
    dst[i] = src[i];
  dst[i] = '\0';
  return i;
}

template <typename To, typename From>
[[nodiscard]] NX_FORCE_INLINE constexpr To cast(From &&from) noexcept {
  return static_cast<To>(std::forward<From>(from));
}

template <typename To, typename From>
  requires std::is_pointer_v<To>
[[nodiscard]] NX_FORCE_INLINE To safe_cast(From *from) noexcept {
#if NX_DEBUG
  if constexpr (std::is_polymorphic_v<From>) {
    if (from == nullptr)
      return nullptr;
    To result = dynamic_cast<To>(from);
    assert(result != nullptr && "safe_cast: invalid downcast");
    return result;
  } else {
    return static_cast<To>(from);
  }
#else
  return static_cast<To>(from);
#endif
}

template <typename To, typename From>
  requires std::is_reference_v<To>
[[nodiscard]] NX_FORCE_INLINE To safe_cast(From &from) {
#if NX_DEBUG
  if constexpr (std::is_polymorphic_v<From>)
    return dynamic_cast<To>(from);
  else
    return static_cast<To>(from);
#else
  return static_cast<To>(from);
#endif
}

[[nodiscard]] inline void *
mem_alloc(usize size, usize align = alignof(max_align_t)) noexcept {
#if NX_USE_MIMALLOC
  return mi_aligned_alloc(align, size);
#elif defined(NX_PLATFORM_WINDOWS)
  return _aligned_malloc(size, align);
#else
  void *p = nullptr;
  posix_memalign(&p, align, size);
  return p;
#endif
}

[[nodiscard]] inline void *
mem_realloc(void *ptr, usize newsize,
            usize align = alignof(max_align_t)) noexcept {
#if NX_USE_MIMALLOC
  return mi_realloc_aligned(ptr, newsize, align);
#elif defined(NX_PLATFORM_WINDOWS)
  return _aligned_realloc(ptr, newsize, align);
#else

  // POSIX has no aligned_realloc; get old size, alloc, copy, free.
  usize old_size = 0;
  if (ptr) {
#if defined(NX_PLATFORM_MACOS)
    old_size = malloc_size(ptr);
#else
    old_size = malloc_usable_size(ptr);
#endif
  }
  void *p = nullptr;
  posix_memalign(&p, align, newsize);
  if (!p)
    return nullptr;
  if (ptr) {
    memcpy(p, ptr, old_size < newsize ? old_size : newsize);
    free(ptr);
  }
  return p;
#endif
}

inline void mem_free(void *ptr) noexcept {
#if NX_USE_MIMALLOC
  mi_free(ptr);
#elif defined(NX_PLATFORM_WINDOWS)
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

// Typed allocation: allocates aligned memory and constructs T in place.
template <typename T, usize Align = alignof(T), typename... Args>
  requires(!std::is_array_v<T> && Align >= alignof(T))
[[nodiscard]] T *
allocate(Args &&...args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
  void *raw = mem_alloc(sizeof(T), Align);
  if (!raw) [[unlikely]]
    return nullptr;
  if constexpr (std::is_trivially_constructible_v<T, Args...> &&
                sizeof...(Args) == 0)
    return static_cast<T *>(raw);
  else
    return ::new (raw) T(std::forward<Args>(args)...);
}

// Typed release: destructs T then frees its memory.
template <typename T>
  requires(!std::is_array_v<T>)
void release(T *ptr) noexcept {
  if (!ptr) [[unlikely]]
    return;
  if constexpr (!std::is_trivially_destructible_v<T>)
    ptr->~T();
  mem_free(ptr);
}

// Typed array allocation: allocates n elements, default-constructs non-trivial
// types.
template <typename T, usize Align = alignof(T)>
  requires(!std::is_array_v<T> && Align >= alignof(T))
[[nodiscard]] T *
allocate_array(usize n) noexcept(std::is_nothrow_default_constructible_v<T>) {
  if (n == 0) [[unlikely]]
    return nullptr;
  void *raw = mem_alloc(n * sizeof(T), Align);
  if (!raw) [[unlikely]]
    return nullptr;
  T *arr = static_cast<T *>(raw);
  if constexpr (!std::is_trivially_default_constructible_v<T>) {
    for (usize i = 0; i < n; ++i)
      ::new (arr + i) T();
  }
  return arr;
}

// Typed array release: destructs n elements then frees the allocation.
template <typename T>
  requires(!std::is_array_v<T>)
void release_array(T *ptr, usize n) noexcept {
  if (!ptr) [[unlikely]]
    return;
  if constexpr (!std::is_trivially_destructible_v<T>) {
    for (usize i = n; i-- > 0;)
      ptr[i].~T();
  }
  mem_free(ptr);
}

template <typename T> struct DefaultAllocator {
  using value_type = T;

  DefaultAllocator() = default;
  template <typename U>
  DefaultAllocator(const DefaultAllocator<U> &) noexcept {}

  [[nodiscard]] T *allocate(const std::size_t n) {
    void *p = mem_alloc(n * sizeof(T), alignof(T));
    if (!p) [[unlikely]]
      throw std::bad_alloc{};
    return static_cast<T *>(p);
  }

  void deallocate(T *p, std::size_t) noexcept { mem_free(p); }

  template <typename U>
  bool operator==(const DefaultAllocator<U> &) const noexcept {
    return true;
  }
};

#if NX_USE_MIMALLOC
template <typename T> using Allocator = mi_stl_allocator<T>;
#else
template <typename T> using Allocator = DefaultAllocator<T>;
#endif

template <typename T, typename Alloc = Allocator<T>>
using vector = std::vector<T, Alloc>;

template <typename T> using shared_ptr = std::shared_ptr<T>;

template <typename T> using weak_ptr = std::weak_ptr<T>;

template <typename T, typename... Args>
[[nodiscard]] shared_ptr<T> make_shared(Args &&...args) {
  return std::allocate_shared<T>(Allocator<T>{}, std::forward<Args>(args)...);
}

} // namespace nx