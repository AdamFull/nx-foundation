#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

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

#ifndef NX_USE_MIMALLOC
#define NX_USE_MIMALLOC 1
#endif
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

// clang-cl is Clang behind cl's driver, and it defines _MSC_VER - so it has to
// be recognised before MSVC or it reports itself as the compiler it emulates.
// Both flags are set on purpose: it targets the MSVC ABI, which makes the
// __declspec attributes and [[msvc::no_unique_address]] below the correct ones
// for it, while Clang's builtins are available as well.
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

// A thread with a deadline can declare that it must not allocate, and be held
// to it in debug builds. The audio mixer's render callback is the case this
// exists for: one allocation there is a dropout, and nothing about the symptom
// points back at the line that caused it.
#if !defined(NX_ALLOC_GUARD)
#define NX_ALLOC_GUARD NX_DEBUG
#endif

#if NX_ALLOC_GUARD
namespace detail {
extern thread_local u32 t_no_alloc_depth;
}

[[nodiscard]] inline bool allocation_allowed() noexcept {
  return detail::t_no_alloc_depth == 0;
}

/// Forbids allocation on this thread for the scope's lifetime. Nests.
class scoped_no_alloc {
public:
  scoped_no_alloc() noexcept { ++detail::t_no_alloc_depth; }
  ~scoped_no_alloc() noexcept { --detail::t_no_alloc_depth; }

  scoped_no_alloc(const scoped_no_alloc &) = delete;
  scoped_no_alloc &operator=(const scoped_no_alloc &) = delete;
};
#else
[[nodiscard]] inline constexpr bool allocation_allowed() noexcept {
  return true;
}

class scoped_no_alloc {
public:
  scoped_no_alloc() noexcept = default;

  scoped_no_alloc(const scoped_no_alloc &) = delete;
  scoped_no_alloc &operator=(const scoped_no_alloc &) = delete;
};
#endif

[[nodiscard]] inline void *
mem_alloc(usize size, usize align = alignof(max_align_t)) noexcept {
  assert(allocation_allowed() && "allocation inside a scoped_no_alloc");
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
  assert(allocation_allowed() && "reallocation inside a scoped_no_alloc");
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

// Freeing is guarded too: the allocator's free path is no more real-time than
// its allocate path, and a deadline thread that frees is a deadline thread
// that was handed ownership of something it should only have borrowed.
inline void mem_free(void *ptr) noexcept {
  assert(allocation_allowed() && "free inside a scoped_no_alloc");
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

namespace detail {

struct SharedHeader {
  void (*destroy)(void *object) noexcept;
  std::atomic<u32> refs;
  u32 back;
};

struct alignas(std::max_align_t) SharedChunk {
  u8 raw[alignof(std::max_align_t)];
};

template <typename T, typename Alloc> struct SharedBlock {
  using chunks =
      typename std::allocator_traits<Alloc>::template rebind_alloc<SharedChunk>;
  using traits = std::allocator_traits<chunks>;

  static constexpr bool holds_allocator = !std::is_empty_v<chunks>;

  static_assert(holds_allocator || std::is_default_constructible_v<chunks>,
                "a stateless allocator is rebuilt rather than stored, so it "
                "has to be default-constructible");
  static_assert(!holds_allocator || alignof(chunks) <= alignof(SharedChunk),
                "an over-aligned allocator cannot be stored in the block");

  static constexpr usize align = max(alignof(T), alignof(SharedHeader));
  static constexpr usize prefix =
      align_up(holds_allocator ? sizeof(chunks) : usize(0),
               alignof(SharedHeader)) +
      sizeof(SharedHeader);
  static constexpr usize slack = align > alignof(SharedChunk) ? align
                                                              : usize(0);
  static constexpr usize count =
      (prefix + slack + sizeof(T) + sizeof(SharedChunk) - 1) /
      sizeof(SharedChunk);
};

[[nodiscard]] NX_FORCE_INLINE SharedHeader *
shared_header(const void *object) noexcept {
  return const_cast<SharedHeader *>(static_cast<const SharedHeader *>(object) -
                                    1);
}

NX_FORCE_INLINE void shared_retain(const void *object) noexcept {
  if (object != nullptr)
    shared_header(object)->refs.fetch_add(1, std::memory_order_relaxed);
}

NX_FORCE_INLINE void shared_release(const void *object) noexcept {
  if (object == nullptr)
    return;
  SharedHeader *const head = shared_header(object);
  if (head->refs.fetch_sub(1, std::memory_order_release) != 1)
    return;
  std::atomic_thread_fence(std::memory_order_acquire);
  head->destroy(const_cast<void *>(object));
}

template <typename T, typename Alloc>
void shared_destroy(void *object) noexcept {
  using U = std::remove_cv_t<T>;
  using Block = SharedBlock<U, Alloc>;
  using Held = typename Block::chunks;

  u8 *const base = static_cast<u8 *>(object) - shared_header(object)->back;
  static_cast<U *>(object)->~U();

  // The allocator has to survive the deallocation of the block it lives in.
  if constexpr (Block::holds_allocator) {
    Held *const stored = reinterpret_cast<Held *>(base);
    Held source(std::move(*stored));
    stored->~Held();
    Block::traits::deallocate(source, reinterpret_cast<SharedChunk *>(base),
                              Block::count);
  } else {
    Held source;
    Block::traits::deallocate(source, reinterpret_cast<SharedChunk *>(base),
                              Block::count);
  }
}

template <typename From, typename To>
concept SharedRebindable =
    std::is_convertible_v<From *, To *> &&
    (std::is_void_v<std::remove_cv_t<To>> ||
     std::is_same_v<std::remove_cv_t<From>, std::remove_cv_t<To>>);

struct AdoptShared {};

} // namespace detail

template <typename T> class shared_ptr {
public:
  using element_type = T;

  constexpr shared_ptr() noexcept = default;
  constexpr shared_ptr(std::nullptr_t) noexcept {}
  shared_ptr(detail::AdoptShared, T *object) noexcept : m_ptr(object) {}

  shared_ptr(const shared_ptr &other) noexcept : m_ptr(other.m_ptr) {
    detail::shared_retain(m_ptr);
  }

  shared_ptr(shared_ptr &&other) noexcept : m_ptr(other.m_ptr) {
    other.m_ptr = nullptr;
  }

  template <typename U>
    requires detail::SharedRebindable<U, T>
  shared_ptr(const shared_ptr<U> &other) noexcept : m_ptr(other.m_ptr) {
    detail::shared_retain(m_ptr);
  }

  template <typename U>
    requires detail::SharedRebindable<U, T>
  shared_ptr(shared_ptr<U> &&other) noexcept : m_ptr(other.m_ptr) {
    other.m_ptr = nullptr;
  }

  ~shared_ptr() { detail::shared_release(m_ptr); }

  shared_ptr &operator=(const shared_ptr &other) noexcept {
    if (this == &other)
      return *this;
    return assign(other.m_ptr);
  }

  template <typename U>
    requires detail::SharedRebindable<U, T>
  shared_ptr &operator=(const shared_ptr<U> &other) noexcept {
    return assign(other.m_ptr);
  }

  shared_ptr &operator=(shared_ptr &&other) noexcept {
    if (this != &other) {
      detail::shared_release(m_ptr);
      m_ptr = other.m_ptr;
      other.m_ptr = nullptr;
    }
    return *this;
  }

  template <typename U>
    requires detail::SharedRebindable<U, T>
  shared_ptr &operator=(shared_ptr<U> &&other) noexcept {
    detail::shared_release(m_ptr);
    m_ptr = other.m_ptr;
    other.m_ptr = nullptr;
    return *this;
  }

  shared_ptr &operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
  }

  void reset() noexcept {
    detail::shared_release(m_ptr);
    m_ptr = nullptr;
  }

  void swap(shared_ptr &other) noexcept {
    T *const held = m_ptr;
    m_ptr = other.m_ptr;
    other.m_ptr = held;
  }

  [[nodiscard]] T *get() const noexcept { return m_ptr; }
  [[nodiscard]] T *operator->() const noexcept { return m_ptr; }
  [[nodiscard]] std::add_lvalue_reference_t<T> operator*() const noexcept
    requires(!std::is_void_v<T>)
  {
    return *m_ptr;
  }
  [[nodiscard]] explicit operator bool() const noexcept {
    return m_ptr != nullptr;
  }

  [[nodiscard]] u32 use_count() const noexcept {
    return m_ptr == nullptr ? 0u
                            : detail::shared_header(m_ptr)->refs.load(
                                  std::memory_order_acquire);
  }

private:
  template <typename U> friend class shared_ptr;

  shared_ptr &assign(T *object) noexcept {
    if (m_ptr == object)
      return *this;
    detail::shared_retain(object);
    detail::shared_release(m_ptr);
    m_ptr = object;
    return *this;
  }

  T *m_ptr = nullptr;
};

template <typename T, typename U>
[[nodiscard]] bool operator==(const shared_ptr<T> &a,
                              const shared_ptr<U> &b) noexcept {
  return a.get() == b.get();
}

template <typename T>
[[nodiscard]] bool operator==(const shared_ptr<T> &a, std::nullptr_t) noexcept {
  return a.get() == nullptr;
}

template <typename T> void swap(shared_ptr<T> &a, shared_ptr<T> &b) noexcept {
  a.swap(b);
}

/// The object, the header and - when it has state worth keeping - a copy of
/// @p alloc, in one allocation taken from @p alloc itself.
template <typename T, typename Alloc, typename... Args>
  requires(!std::is_array_v<T>)
[[nodiscard]] shared_ptr<T> allocate_shared(const Alloc &alloc,
                                            Args &&...args) {
  using Block = detail::SharedBlock<T, Alloc>;
  using Held = typename Block::chunks;
  assert(allocation_allowed() && "allocation inside a scoped_no_alloc");

  Held source(alloc);
  detail::SharedChunk *const chunk =
      Block::traits::allocate(source, Block::count);
  if (chunk == nullptr) [[unlikely]]
    return {};

  u8 *const base = reinterpret_cast<u8 *>(chunk);
  T *const object = reinterpret_cast<T *>(
      align_up(reinterpret_cast<uintptr_t>(base + Block::prefix),
               uintptr_t(Block::align)));

  detail::SharedHeader *const head =
      ::new (static_cast<void *>(detail::shared_header(object)))
          detail::SharedHeader{};
  head->destroy = &detail::shared_destroy<T, Alloc>;
  head->refs.store(1, std::memory_order_relaxed);
  head->back = cast<u32>(reinterpret_cast<u8 *>(object) - base);

  if constexpr (Block::holds_allocator)
    ::new (static_cast<void *>(base)) Held(source);

  try {
    ::new (static_cast<void *>(object)) T(std::forward<Args>(args)...);
  } catch (...) {
    if constexpr (Block::holds_allocator)
      reinterpret_cast<Held *>(base)->~Held();
    Block::traits::deallocate(source, chunk, Block::count);
    throw;
  }
  return shared_ptr<T>(detail::AdoptShared{}, object);
}

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
[[nodiscard]] shared_ptr<T> make_shared(Args &&...args) {
  // Qualified: an unqualified call drags in std::allocate_shared by ADL as
  // soon as T is a std type, and the two overloads are then ambiguous.
  return nx::allocate_shared<T>(Allocator<T>{}, std::forward<Args>(args)...);
}

class life_watch {
public:
  life_watch() = default;

  [[nodiscard]] bool alive() const noexcept {
    return m_flag && m_flag->load(std::memory_order_acquire);
  }
  [[nodiscard]] explicit operator bool() const noexcept { return alive(); }

private:
  friend class life_token;

  explicit life_watch(shared_ptr<std::atomic<bool>> flag) noexcept
      : m_flag(std::move(flag)) {}

  shared_ptr<std::atomic<bool>> m_flag;
};

/// Held by the object whose life is being watched, as its last member so that
/// it falls after everything a watcher might reach through.
class life_token {
public:
  life_token() : m_flag(make_shared<std::atomic<bool>>(true)) {}
  ~life_token() {
    if (m_flag)
      m_flag->store(false, std::memory_order_release);
  }

  life_token(const life_token &) = delete;
  life_token &operator=(const life_token &) = delete;

  [[nodiscard]] life_watch watch() const noexcept { return life_watch(m_flag); }

private:
  shared_ptr<std::atomic<bool>> m_flag;
};

template <typename T> struct AllocDeleter {
  void operator()(T *p) const noexcept {
    if (p == nullptr)
      return;
    using traits = std::allocator_traits<Allocator<T>>;
    Allocator<T> alloc;
    traits::destroy(alloc, p);
    traits::deallocate(alloc, p, 1);
  }
};

template <typename T, typename Deleter = AllocDeleter<T>>
using unique_ptr = std::unique_ptr<T, Deleter>;

template <typename T, typename... Args>
[[nodiscard]] unique_ptr<T> make_unique(Args &&...args) {
  using traits = std::allocator_traits<Allocator<T>>;
  Allocator<T> alloc;
  T *p = traits::allocate(alloc, 1);
  try {
    traits::construct(alloc, p, std::forward<Args>(args)...);
  } catch (...) {
    traits::deallocate(alloc, p, 1);
    throw;
  }
  return unique_ptr<T>(p);
}

} // namespace nx
