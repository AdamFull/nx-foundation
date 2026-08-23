#pragma once

#include "core/foundation/core/foundation.h"

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
}

template <typename, usize Size = alignof(std::max_align_t)>
class inplace_function;

/// Similar to std::function but never allocates: the callable is held in a fixed-size internal
/// buffer, so construction from a functor that does not fit (or is over-aligned) fails to compile.
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

    m_invoke = [](void *d, Args... args) -> R {
      return (*nx::cast<FType *>(d))(std::forward<Args>(args)...);
    };
    m_destroy = [](void *d) noexcept { nx::cast<FType *>(d)->~FType(); };

    if constexpr (!std::is_trivially_copyable_v<FType>) {
      m_move = [](void *dst, void *src) noexcept {
        ::new (dst) FType(std::move(*nx::cast<FType *>(src)));
      };
      m_copy = [](void *dst, const void *src) {
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
    return m_invoke(raw(), std::forward<Args>(args)...);
  }

  [[nodiscard]] NX_FORCE_INLINE bool valid() const noexcept {
    return m_invoke != nullptr;
  }
  NX_FORCE_INLINE explicit operator bool() const noexcept { return valid(); }

private:
  alignas(std::max_align_t) mutable std::byte m_storage[Size]{};

  R (*m_invoke)(void *, Args...) noexcept(false) = nullptr;
  void (*m_destroy)(void *) noexcept = nullptr;
  void (*m_move)(void *dst, void *src) noexcept = nullptr;
  void (*m_copy)(void *dst, const void *src) noexcept(false) = nullptr;

  NX_FORCE_INLINE void *raw() const noexcept {
    return nx::cast<void *>(m_storage);
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

template <typename> class function;

/// Prefer nx::inplace_function when allocation must be avoided.
template <typename R, typename... Args> class function<R(Args...)> {
public:
  function() = default;

  template <typename F>
    requires(!std::is_same_v<std::decay_t<F>, function>)
  NX_FORCE_INLINE explicit function(F &&f) {
    using FType = std::decay_t<F>;
    m_invoke = [](void *d, Args... args) -> R {
      return (*nx::cast<FType *>(d))(std::forward<Args>(args)...);
    };
    m_destroy = [](void *d) noexcept { release(nx::cast<FType *>(d)); };
    m_copy = [](void *d) -> void * {
      return allocate<FType>(*nx::cast<const FType *>(d));
    };
    m_data = allocate<FType>(std::forward<F>(f));
  }

  ~function() {
    if (NX_LIKELY(m_data != nullptr)) {
      m_destroy(m_data);
      m_data = nullptr;
      m_invoke = nullptr;
      m_destroy = nullptr;
      m_copy = nullptr;
    }
  }

  NX_FORCE_INLINE function(function &&o) noexcept
      : m_invoke(o.m_invoke), m_destroy(o.m_destroy), m_copy(o.m_copy),
        m_data(o.m_data) {
    o.m_invoke = nullptr;
    o.m_destroy = nullptr;
    o.m_copy = nullptr;
    o.m_data = nullptr;
  }

  NX_FORCE_INLINE function &operator=(function &&o) noexcept {
    if (NX_LIKELY(this != &o)) {
      if (NX_LIKELY(m_data != nullptr))
        m_destroy(m_data);
      m_invoke = o.m_invoke;
      m_destroy = o.m_destroy;
      m_copy = o.m_copy;
      m_data = o.m_data;
      o.m_invoke = nullptr;
      o.m_destroy = nullptr;
      o.m_copy = nullptr;
      o.m_data = nullptr;
    }
    return *this;
  }

  NX_FORCE_INLINE function(const function &o) {
    if (NX_LIKELY(o.m_data != nullptr)) {
      m_invoke = o.m_invoke;
      m_destroy = o.m_destroy;
      m_copy = o.m_copy;
      m_data = m_copy(o.m_data);
    }
  }

  NX_FORCE_INLINE function &operator=(const function &o) {
    if (NX_LIKELY(this != &o)) {
      if (NX_LIKELY(m_data != nullptr))
        m_destroy(m_data);
      m_invoke = nullptr;
      m_destroy = nullptr;
      m_copy = nullptr;
      m_data = nullptr;
      if (NX_LIKELY(o.m_data != nullptr)) {
        m_invoke = o.m_invoke;
        m_destroy = o.m_destroy;
        m_copy = o.m_copy;
        m_data = m_copy(o.m_data);
      }
    }
    return *this;
  }

  NX_FORCE_INLINE R operator()(Args... args) const {
    return m_invoke(m_data, std::forward<Args>(args)...);
  }

  [[nodiscard]] NX_FORCE_INLINE bool valid() const noexcept {
    return m_invoke != nullptr;
  }
  NX_FORCE_INLINE explicit operator bool() const noexcept { return valid(); }

private:
  R (*m_invoke)(void *, Args...) noexcept(false) = nullptr;
  void (*m_destroy)(void *) noexcept = nullptr;
  void *(*m_copy)(void *) noexcept(false) = nullptr;
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
    m_data.obj =
        const_cast<void *>(static_cast<const void *>(std::addressof(f)));
    m_call = [](const storage s, Args... args) -> R {
      return (*static_cast<std::decay_t<F> *>(s.obj))(
          std::forward<Args>(args)...);
    };
  }

  template <typename FR, typename... FArgs>
    requires std::is_invocable_r_v<R, FR (*)(FArgs...), Args...>
  function_ref(FR (*fn)(FArgs...)) noexcept {
    m_data.fun = reinterpret_cast<void (*)()>(fn);
    m_call = [](const storage s, Args... args) -> R {
      return reinterpret_cast<FR (*)(FArgs...)>(s.fun)(
          std::forward<Args>(args)...);
    };
  }

  R operator()(Args... args) const {
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

}
