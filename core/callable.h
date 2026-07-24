#pragma once

/**
 * @file callable.h
 * @brief Type-erased callables: nx::function and nx::inplace_function.
 */

#include "foundation/core/foundation.h"

#include <cstring>
#include <type_traits>
#include <utility>

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
} // namespace detail

template <typename, usize Size = alignof(std::max_align_t)>
class inplace_function;

/**
 * @brief Type-erased callable that stores its target in an inline buffer.
 *
 * Similar to std::function but never allocates: the callable is held in a
 * fixed-size internal buffer, so construction from a functor that does not fit
 * (or is over-aligned) fails to compile. Copyable and movable.
 *
 * @tparam R    Return type of the call signature.
 * @tparam Args Argument types of the call signature.
 * @tparam Size Size in bytes of the inline storage buffer.
 */
template <typename R, typename... Args, usize Size>
class inplace_function<R(Args...), Size> {
public:
  static constexpr usize buffer_size = Size;

  inplace_function() = default;

  template <typename F>
    requires(!std::is_same_v<std::decay_t<F>, inplace_function>)
  NX_FORCE_INLINE explicit inplace_function(F &&f) {
    using FType = std::decay_t<F>;

    static_assert(
        sizeof(FType) <= Size,
        "inplace_function: functor does not fit in the inline buffer. "
        "Use make_inplace_function() or specify a larger Size explicitly.");
    static_assert(
        alignof(FType) <= alignof(std::max_align_t),
        "inplace_function: functor alignment exceeds std::max_align_t. "
        "Reduce the capture alignment or specify an explicit Size.");

    ::new (raw()) FType(std::forward<F>(f));

    invoke_ = [](void *d, Args... args) -> R {
      return (*nx::cast<FType *>(d))(std::forward<Args>(args)...);
    };
    destroy_ = [](void *d) noexcept { nx::cast<FType *>(d)->~FType(); };

    if constexpr (!std::is_trivially_copyable_v<FType>) {
      move_ = [](void *dst, void *src) noexcept {
        ::new (dst) FType(std::move(*nx::cast<FType *>(src)));
      };
      copy_ = [](void *dst, const void *src) {
        ::new (dst) FType(*nx::cast<const FType *>(src));
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
      reset();
      copy_from(o);
    }
    return *this;
  }

  NX_FORCE_INLINE R operator()(Args... args) const {
    return invoke_(raw(), std::forward<Args>(args)...);
  }

  [[nodiscard]] NX_FORCE_INLINE bool valid() const noexcept {
    return invoke_ != nullptr;
  }
  NX_FORCE_INLINE explicit operator bool() const noexcept { return valid(); }

private:
  alignas(std::max_align_t) mutable std::byte storage_[Size]{};

  R (*invoke_)(void *, Args...) noexcept(false) = nullptr;
  void (*destroy_)(void *) noexcept = nullptr;
  void (*move_)(void *dst, void *src) noexcept = nullptr;
  void (*copy_)(void *dst, const void *src) noexcept(false) = nullptr;

  NX_FORCE_INLINE void *raw() const noexcept {
    return nx::cast<void *>(storage_);
  }

  NX_FORCE_INLINE void reset() noexcept {
    if (NX_LIKELY(invoke_ != nullptr)) {
      destroy_(raw());
      invoke_ = nullptr;
      destroy_ = nullptr;
      move_ = nullptr;
      copy_ = nullptr;
    }
  }

  NX_FORCE_INLINE void move_from(inplace_function &&o) noexcept {
    if (NX_UNLIKELY(!o.invoke_))
      return;

    if (o.move_) {
      o.move_(raw(), o.raw());
      o.destroy_(o.raw());
    } else
      std::memcpy(storage_, o.storage_, Size);

    invoke_ = o.invoke_;
    destroy_ = o.destroy_;
    move_ = o.move_;
    copy_ = o.copy_;

    o.invoke_ = nullptr;
    o.destroy_ = nullptr;
    o.move_ = nullptr;
    o.copy_ = nullptr;
  }

  NX_FORCE_INLINE void copy_from(const inplace_function &o) {
    if (NX_UNLIKELY(!o.invoke_))
      return;

    if (o.copy_)
      o.copy_(raw(), o.raw());
    else
      std::memcpy(storage_, o.storage_, Size);

    invoke_ = o.invoke_;
    destroy_ = o.destroy_;
    move_ = o.move_;
    copy_ = o.copy_;
  }
};

template <typename F>
inplace_function(F &&)
    -> inplace_function<typename detail::traits_of<F>::signature,
                        detail::storage_size_for<F>>;

template <typename R, typename... Args>
inplace_function(R (*)(Args...))
    -> inplace_function<R(Args...), detail::storage_size_for<R (*)(Args...)>>;

/// Builds an nx::inplace_function, deducing the signature and buffer size from
/// @p f.
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

template <typename> class function;

/**
 * @brief Type-erased, owning callable wrapper (heap-backed).
 *
 * Equivalent in spirit to std::function: holds any callable matching the
 * signature R(Args...) on the heap. Copyable and movable. Prefer
 * nx::inplace_function when allocation must be avoided.
 *
 * @tparam R    Return type of the call signature.
 * @tparam Args Argument types of the call signature.
 */
template <typename R, typename... Args> class function<R(Args...)> {
public:
  function() = default;

  template <typename F>
    requires(!std::is_same_v<std::decay_t<F>, function>)
  NX_FORCE_INLINE explicit function(F &&f) {
    using FType = std::decay_t<F>;
    invoke_ = [](void *d, Args... args) -> R {
      return (*nx::cast<FType *>(d))(std::forward<Args>(args)...);
    };
    destroy_ = [](void *d) noexcept { release(nx::cast<FType *>(d)); };
    copy_ = [](void *d) -> void * {
      return allocate<FType>(*nx::cast<const FType *>(d));
    };
    data_ = allocate<FType>(std::forward<F>(f));
  }

  ~function() {
    if (NX_LIKELY(data_ != nullptr)) {
      destroy_(data_);
      data_ = nullptr;
      invoke_ = nullptr;
      destroy_ = nullptr;
      copy_ = nullptr;
    }
  }

  NX_FORCE_INLINE function(function &&o) noexcept
      : invoke_(o.invoke_), destroy_(o.destroy_), copy_(o.copy_),
        data_(o.data_) {
    o.invoke_ = nullptr;
    o.destroy_ = nullptr;
    o.copy_ = nullptr;
    o.data_ = nullptr;
  }

  NX_FORCE_INLINE function &operator=(function &&o) noexcept {
    if (NX_LIKELY(this != &o)) {
      if (NX_LIKELY(data_ != nullptr))
        destroy_(data_);
      invoke_ = o.invoke_;
      destroy_ = o.destroy_;
      copy_ = o.copy_;
      data_ = o.data_;
      o.invoke_ = nullptr;
      o.destroy_ = nullptr;
      o.copy_ = nullptr;
      o.data_ = nullptr;
    }
    return *this;
  }

  NX_FORCE_INLINE function(const function &o) {
    if (NX_LIKELY(o.data_ != nullptr)) {
      invoke_ = o.invoke_;
      destroy_ = o.destroy_;
      copy_ = o.copy_;
      data_ = copy_(o.data_);
    }
  }

  NX_FORCE_INLINE function &operator=(const function &o) {
    if (NX_LIKELY(this != &o)) {
      if (NX_LIKELY(data_ != nullptr))
        destroy_(data_);
      invoke_ = nullptr;
      destroy_ = nullptr;
      copy_ = nullptr;
      data_ = nullptr;
      if (NX_LIKELY(o.data_ != nullptr)) {
        invoke_ = o.invoke_;
        destroy_ = o.destroy_;
        copy_ = o.copy_;
        data_ = copy_(o.data_);
      }
    }
    return *this;
  }

  NX_FORCE_INLINE R operator()(Args... args) const {
    return invoke_(data_, std::forward<Args>(args)...);
  }

  [[nodiscard]] NX_FORCE_INLINE bool valid() const noexcept {
    return invoke_ != nullptr;
  }
  NX_FORCE_INLINE explicit operator bool() const noexcept { return valid(); }

private:
  R (*invoke_)(void *, Args...) noexcept(false) = nullptr;
  void (*destroy_)(void *) noexcept = nullptr;
  void *(*copy_)(void *) noexcept(false) = nullptr;
  void *data_ = nullptr;
};

/// Builds an nx::function, deducing the call signature from @p f.
template <typename F> auto make_function(F &&f) {
  using FType = std::decay_t<F>;
  using Traits = decltype([]<typename C, typename Ret, typename... A>(
                              Ret (C::*)(A...) const) {
    return std::type_identity<function<Ret(A...)>>{};
  }(&FType::operator()));
  return typename Traits::type(std::forward<F>(f));
}

template <typename R, typename... Args> auto make_function(R (*fn)(Args...)) {
  return function<R(Args...)>(fn);
}

} // namespace nx