#pragma once

#include "core/foundation/core/foundation.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

// Calls `target` with the erased lambda's `args` as a function returning R; a
// void R discards the result. A macro rather than a helper template: every
// wrapped callable would instantiate the helper, and large translation units
// already sit at MSVC's object section limit.
#define NX_DETAIL_INVOKE_R(target)                                             \
  if constexpr (std::is_member_pointer_v<                                      \
                    std::remove_cvref_t<decltype(target)>>)                    \
    return static_cast<R>(std::invoke(target, std::forward<Args>(args)...));   \
  else                                                                         \
    return static_cast<R>(target(std::forward<Args>(args)...))

namespace nx {
namespace detail {

template <typename> struct function_traits;

template <typename C, typename R, typename... Args>
struct function_traits<R (C::*)(Args...) const> {
  using signature = R(Args...);
};

template <typename C, typename R, typename... Args>
struct function_traits<R (C::*)(Args...)> {
  using signature = R(Args...);
};

template <typename F>
using traits_of = function_traits<decltype(&std::decay_t<F>::operator())>;

template <typename F>
inline constexpr usize storage_size_for =
    align_up(sizeof(std::decay_t<F>), alignof(std::max_align_t));

template <typename F, typename Self, typename R, typename... Args>
concept wraps_callable =
    !std::is_same_v<std::remove_cvref_t<F>, Self> &&
    !std::is_same_v<std::remove_cvref_t<F>, std::nullptr_t> &&
    std::is_invocable_r_v<R, std::decay_t<F> &, Args...>;

template <typename T, typename... Args> T *allocate_or_throw(Args &&...args) {
  T *const made = allocate<T>(std::forward<Args>(args)...);
  if (made == nullptr) [[unlikely]]
    throw std::bad_alloc{};
  return made;
}

// A null function or member pointer makes an empty wrapper, as with
// std::function.
template <typename F>
[[nodiscard]] constexpr bool is_null_target(const F &f) noexcept {
  if constexpr (std::is_pointer_v<F> || std::is_member_pointer_v<F>)
    return f == nullptr;
  else
    return false;
}

} // namespace detail

template <typename, usize Size = 2 * sizeof(void *)> class inplace_function;

/// Similar to std::function but never allocates: the callable is held in a
/// fixed-size internal buffer, so construction from a functor that does not fit
/// (or is over-aligned) fails to compile.
template <typename R, typename... Args, usize Size>
class inplace_function<R(Args...), Size> {
public:
  static constexpr usize buffer_size = Size;

  inplace_function() = default;
  inplace_function(std::nullptr_t) noexcept {}

  template <typename F>
    requires detail::wraps_callable<F, inplace_function, R, Args...>
  NX_FORCE_INLINE inplace_function(F &&f) {
    using FType = std::decay_t<F>;

    static_assert(
        sizeof(FType) <= Size,
        "inplace_function: functor does not fit in the inline buffer. "
        "Use make_inplace_function() or specify a larger Size explicitly.");
    static_assert(
        alignof(FType) <= alignof(std::max_align_t),
        "inplace_function: functor alignment exceeds std::max_align_t. "
        "Reduce the capture alignment or specify an explicit Size.");
    static_assert(std::is_copy_constructible_v<FType>,
                  "inplace_function is copyable; hold a move-only callable "
                  "in nx::unique_function");
    static_assert(std::is_nothrow_move_constructible_v<FType>,
                  "inplace_function: the callable's move must not throw");

    if (detail::is_null_target(f))
      return;
    ::new (raw()) FType(std::forward<F>(f));

    m_invoke = [](void *d, Args... args) -> R {
      FType &target = *std::launder(static_cast<FType *>(d));
      NX_DETAIL_INVOKE_R(target);
    };
    m_destroy = [](void *d) noexcept {
      std::launder(static_cast<FType *>(d))->~FType();
    };

    if constexpr (!std::is_trivially_copyable_v<FType>) {
      m_move = [](void *dst, void *src) noexcept {
        ::new (dst) FType(std::move(*std::launder(static_cast<FType *>(src))));
      };
      m_copy = [](void *dst, const void *src) {
        ::new (dst) FType(*std::launder(static_cast<const FType *>(src)));
      };
    }
  }

  ~inplace_function() { reset(); }

  NX_FORCE_INLINE inplace_function(inplace_function &&o) noexcept {
    move_from(std::move(o));
  }

  NX_FORCE_INLINE inplace_function &operator=(inplace_function &&o) noexcept {
    if (NX_LIKELY(this != &o)) {
      reset();
      move_from(std::move(o));
    }
    return *this;
  }

  NX_FORCE_INLINE inplace_function(const inplace_function &o) { copy_from(o); }

  NX_FORCE_INLINE inplace_function &operator=(const inplace_function &o) {
    if (NX_LIKELY(this != &o)) {
      inplace_function copy(o);
      reset();
      move_from(std::move(copy));
    }
    return *this;
  }

  inplace_function &operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
  }

  NX_FORCE_INLINE R operator()(Args... args) const {
    assert(m_invoke != nullptr && "inplace_function: called while empty");
    return m_invoke(raw(), std::forward<Args>(args)...);
  }

  [[nodiscard]] NX_FORCE_INLINE bool valid() const noexcept {
    return m_invoke != nullptr;
  }
  NX_FORCE_INLINE explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] friend bool operator==(const inplace_function &f,
                                       std::nullptr_t) noexcept {
    return !f.valid();
  }

  NX_FORCE_INLINE void reset() noexcept {
    if (NX_LIKELY(m_invoke != nullptr)) {
      m_destroy(raw());
      m_invoke = nullptr;
      m_destroy = nullptr;
      m_move = nullptr;
      m_copy = nullptr;
    }
  }

private:
  alignas(std::max_align_t) mutable std::byte m_storage[Size]{};

  R (*m_invoke)(void *, Args...) noexcept(false) = nullptr;
  void (*m_destroy)(void *) noexcept = nullptr;
  void (*m_move)(void *dst, void *src) noexcept = nullptr;
  void (*m_copy)(void *dst, const void *src) noexcept(false) = nullptr;

  NX_FORCE_INLINE void *raw() const noexcept {
    return nx::cast<void *>(m_storage);
  }

  NX_FORCE_INLINE void move_from(inplace_function &&o) noexcept {
    if (NX_UNLIKELY(!o.m_invoke))
      return;

    if (o.m_move) {
      o.m_move(raw(), o.raw());
      o.m_destroy(o.raw());
    } else
      std::memcpy(m_storage, o.m_storage, Size);

    m_invoke = o.m_invoke;
    m_destroy = o.m_destroy;
    m_move = o.m_move;
    m_copy = o.m_copy;

    o.m_invoke = nullptr;
    o.m_destroy = nullptr;
    o.m_move = nullptr;
    o.m_copy = nullptr;
  }

  NX_FORCE_INLINE void copy_from(const inplace_function &o) {
    if (NX_UNLIKELY(!o.m_invoke))
      return;

    if (o.m_copy)
      o.m_copy(raw(), o.raw());
    else
      std::memcpy(m_storage, o.m_storage, Size);

    m_invoke = o.m_invoke;
    m_destroy = o.m_destroy;
    m_move = o.m_move;
    m_copy = o.m_copy;
  }
};

template <typename F>
inplace_function(F &&)
    -> inplace_function<typename detail::traits_of<F>::signature,
                        detail::storage_size_for<F>>;

template <typename R, typename... Args>
inplace_function(R (*)(Args...))
    -> inplace_function<R(Args...), detail::storage_size_for<R (*)(Args...)>>;

template <usize Size = 0, typename F>
NX_FORCE_INLINE auto make_inplace_function(F &&f) {
  using FType = std::decay_t<F>;
  using Sig = detail::traits_of<F>::signature;
  constexpr usize ActualSize =
      (Size == 0) ? detail::storage_size_for<FType> : Size;
  return inplace_function<Sig, ActualSize>(std::forward<F>(f));
}

template <usize Size = 0, typename R, typename... Args>
NX_FORCE_INLINE auto make_inplace_function(R (*fn)(Args...)) {
  constexpr usize ActualSize =
      (Size == 0) ? detail::storage_size_for<R (*)(Args...)> : Size;
  return inplace_function<R(Args...), ActualSize>(fn);
}

template <typename, usize InlineSize = 24> class unique_function;

/// Move-only callable with a small inline buffer. Unlike function, the common
/// small-lambda path does not allocate; oversized or over-aligned callables
/// transparently fall back to the engine allocator. This is the ownership
/// shape needed by queues, where copying a pending operation is both wasteful
/// and semantically surprising.
template <typename R, typename... Args, usize InlineSize>
class unique_function<R(Args...), InlineSize> {
  static_assert(InlineSize >= sizeof(void *));

  struct Ops {
    R (*invoke)(void *, Args...);
    void (*destroy)(void *) noexcept;
    void (*move)(void *, void *) noexcept;
  };

  template <typename F> static F *&heap_ptr(void *storage) noexcept {
    return *std::launder(reinterpret_cast<F **>(storage));
  }

  template <typename F> static const Ops &inline_ops() noexcept {
    static const Ops ops{[](void *storage, Args... args) -> R {
                           F &target =
                               *std::launder(reinterpret_cast<F *>(storage));
                           NX_DETAIL_INVOKE_R(target);
                         },
                         [](void *storage) noexcept {
                           std::launder(reinterpret_cast<F *>(storage))->~F();
                         },
                         [](void *dst, void *src) noexcept {
                           F *const from =
                               std::launder(reinterpret_cast<F *>(src));
                           ::new (dst) F(std::move(*from));
                           from->~F();
                         }};
    return ops;
  }

  template <typename F> static const Ops &heap_ops() noexcept {
    static const Ops ops{[](void *storage, Args... args) -> R {
                           F &target = *heap_ptr<F>(storage);
                           NX_DETAIL_INVOKE_R(target);
                         },
                         [](void *storage) noexcept {
                           if (F *const ptr = heap_ptr<F>(storage)) {
                             release(ptr);
                             heap_ptr<F>(storage) = nullptr;
                           }
                         },
                         [](void *dst, void *src) noexcept {
                           ::new (dst) F *(heap_ptr<F>(src));
                           heap_ptr<F>(src) = nullptr;
                         }};
    return ops;
  }

public:
  unique_function() = default;
  unique_function(std::nullptr_t) noexcept {}

  template <typename F>
    requires detail::wraps_callable<F, unique_function, R, Args...>
  NX_FORCE_INLINE unique_function(F &&fn) {
    using FType = std::decay_t<F>;

    if (detail::is_null_target(fn))
      return;
    constexpr bool fits = sizeof(FType) <= InlineSize &&
                          alignof(FType) <= alignof(std::max_align_t) &&
                          std::is_nothrow_move_constructible_v<FType>;
    if constexpr (fits) {
      ::new (raw()) FType(std::forward<F>(fn));
      m_ops = &inline_ops<FType>();
    } else {
      ::new (raw())
          FType *(detail::allocate_or_throw<FType>(std::forward<F>(fn)));
      m_ops = &heap_ops<FType>();
    }
  }

  ~unique_function() { reset(); }

  unique_function(const unique_function &) = delete;
  unique_function &operator=(const unique_function &) = delete;

  NX_FORCE_INLINE unique_function(unique_function &&other) noexcept {
    move_from(std::move(other));
  }

  NX_FORCE_INLINE
  unique_function &operator=(unique_function &&other) noexcept {
    if (this != &other) {
      reset();
      move_from(std::move(other));
    }
    return *this;
  }

  unique_function &operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
  }

  NX_FORCE_INLINE R operator()(Args... args) const {
    assert(m_ops != nullptr && "unique_function: called while empty");
    return m_ops->invoke(raw(), std::forward<Args>(args)...);
  }

  [[nodiscard]] NX_FORCE_INLINE bool valid() const noexcept {
    return m_ops != nullptr;
  }
  [[nodiscard]] NX_FORCE_INLINE explicit operator bool() const noexcept {
    return valid();
  }
  [[nodiscard]] friend bool operator==(const unique_function &f,
                                       std::nullptr_t) noexcept {
    return !f.valid();
  }

  NX_FORCE_INLINE void reset() noexcept {
    if (m_ops != nullptr) {
      m_ops->destroy(raw());
      m_ops = nullptr;
    }
  }

private:
  NX_FORCE_INLINE void move_from(unique_function &&other) noexcept {
    if (other.m_ops == nullptr)
      return;
    other.m_ops->move(raw(), other.raw());
    m_ops = other.m_ops;
    other.m_ops = nullptr;
  }

  [[nodiscard]] NX_FORCE_INLINE void *raw() const noexcept {
    return const_cast<std::byte *>(m_storage);
  }

  // Storage first keeps unique_function<void()> at 32 bytes with the default
  // 24-byte payload on 64-bit platforms. A pool task plus its group pointer
  // therefore still fits in one 64-byte queue cell.
  alignas(std::max_align_t) mutable std::byte m_storage[InlineSize]{};
  const Ops *m_ops = nullptr;
};

template <typename> class function;

/// Prefer nx::inplace_function when allocation must be avoided.
template <typename R, typename... Args> class function<R(Args...)> {
public:
  function() = default;
  function(std::nullptr_t) noexcept {}

  template <typename F>
    requires detail::wraps_callable<F, function, R, Args...>
  NX_FORCE_INLINE function(F &&f) {
    using FType = std::decay_t<F>;
    static_assert(std::is_copy_constructible_v<FType>,
                  "nx::function is copyable; hold a move-only callable in "
                  "nx::unique_function");

    if (detail::is_null_target(f))
      return;
    m_data = detail::allocate_or_throw<FType>(std::forward<F>(f));
    m_invoke = [](void *d, Args... args) -> R {
      FType &target = *static_cast<FType *>(d);
      NX_DETAIL_INVOKE_R(target);
    };
    m_destroy = [](void *d) noexcept { release(static_cast<FType *>(d)); };
    m_copy = [](const void *d) -> void * {
      return detail::allocate_or_throw<FType>(*static_cast<const FType *>(d));
    };
  }

  ~function() { reset(); }

  NX_FORCE_INLINE function(function &&o) noexcept
      : m_invoke(o.m_invoke), m_destroy(o.m_destroy), m_copy(o.m_copy),
        m_data(o.m_data) {
    o.forget();
  }

  NX_FORCE_INLINE function &operator=(function &&o) noexcept {
    if (NX_LIKELY(this != &o)) {
      reset();
      m_invoke = o.m_invoke;
      m_destroy = o.m_destroy;
      m_copy = o.m_copy;
      m_data = o.m_data;
      o.forget();
    }
    return *this;
  }

  NX_FORCE_INLINE function(const function &o) {
    if (o.m_data != nullptr) {
      m_data = o.m_copy(o.m_data);
      m_invoke = o.m_invoke;
      m_destroy = o.m_destroy;
      m_copy = o.m_copy;
    }
  }

  NX_FORCE_INLINE function &operator=(const function &o) {
    if (NX_LIKELY(this != &o))
      *this = function(o);
    return *this;
  }

  function &operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
  }

  NX_FORCE_INLINE R operator()(Args... args) const {
    assert(m_data != nullptr && "function: called while empty");
    return m_invoke(m_data, std::forward<Args>(args)...);
  }

  [[nodiscard]] NX_FORCE_INLINE bool valid() const noexcept {
    return m_data != nullptr;
  }
  NX_FORCE_INLINE explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] friend bool operator==(const function &f,
                                       std::nullptr_t) noexcept {
    return !f.valid();
  }

  void reset() noexcept {
    if (m_data != nullptr)
      m_destroy(m_data);
    forget();
  }

private:
  void forget() noexcept {
    m_invoke = nullptr;
    m_destroy = nullptr;
    m_copy = nullptr;
    m_data = nullptr;
  }

  R (*m_invoke)(void *, Args...) noexcept(false) = nullptr;
  void (*m_destroy)(void *) noexcept = nullptr;
  void *(*m_copy)(const void *) noexcept(false) = nullptr;
  void *m_data = nullptr;
};

template <typename> class function_ref;

template <typename R, typename... Args> class function_ref<R(Args...)> {
  union storage {
    void *obj;
    void (*fun)();
  };

public:
  constexpr function_ref() noexcept = default;

  /// From a callable object. Refers to it, so it must outlive this.
  template <typename F>
    requires(!std::is_same_v<std::decay_t<F>, function_ref> &&
             !std::is_function_v<std::remove_pointer_t<std::decay_t<F>>> &&
             std::is_invocable_r_v<R, F &, Args...>)
  function_ref(F &&f NX_LIFETIMEBOUND) noexcept {
    using Target = std::remove_reference_t<F>;
    m_data.obj =
        const_cast<void *>(static_cast<const void *>(std::addressof(f)));
    m_call = [](const storage s, Args... args) -> R {
      Target &target = *static_cast<Target *>(s.obj);
      NX_DETAIL_INVOKE_R(target);
    };
  }

  template <typename FR, typename... FArgs>
    requires std::is_invocable_r_v<R, FR (*)(FArgs...), Args...>
  function_ref(FR (*fn)(FArgs...)) noexcept {
    if (fn == nullptr)
      return;
    m_data.fun = reinterpret_cast<void (*)()>(fn);
    m_call = [](const storage s, Args... args) -> R {
      FR (*const target)(FArgs...) = reinterpret_cast<FR (*)(FArgs...)>(s.fun);
      NX_DETAIL_INVOKE_R(target);
    };
  }

  R operator()(Args... args) const {
    assert(m_call != nullptr && "function_ref: called while empty");
    return m_call(m_data, std::forward<Args>(args)...);
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return m_call != nullptr;
  }

private:
  storage m_data{nullptr};
  R (*m_call)(storage, Args...) = nullptr;
};

template <typename F> auto make_function(F &&f) {
  return function<typename detail::traits_of<F>::signature>(std::forward<F>(f));
}

template <typename R, typename... Args> auto make_function(R (*fn)(Args...)) {
  return function<R(Args...)>(fn);
}

} // namespace nx

#undef NX_DETAIL_INVOKE_R
