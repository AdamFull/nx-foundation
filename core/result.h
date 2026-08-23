#pragma once

#include "core/foundation/core/foundation.h"

namespace nx {

template <typename E> struct err {
  constexpr explicit err(const E &e) : m_error(e) {}
  constexpr explicit err(E &&e) : m_error(std::move(e)) {}

  constexpr const E &error() const & { return m_error; }
  constexpr E &&error() && { return std::move(m_error); }

private:
  E m_error;
};

template <typename E> err(E) -> err<E>;

namespace detail {

template <typename T, typename E> union storage {
  T value;
  E error;

  constexpr storage() {}
  constexpr ~storage() {}
};

template <typename E> union storage<void, E> {
  E error;

  constexpr storage() {}
  constexpr ~storage() {}
};

}

template <typename T, typename E> class result {
  static_assert(!std::is_reference_v<T>, "T must not be a reference");
  static_assert(!std::is_reference_v<E>, "E must not be a reference");

public:
  using value_type = T;
  using error_type = E;

  constexpr result()
    requires std::default_initializable<T>
      : m_has_value(true) {
    std::construct_at(&m_storage.value);
  }

  constexpr result(const T &v)
    requires std::is_copy_constructible_v<T>
      : m_has_value(true) {
    std::construct_at(&m_storage.value, v);
  }

  constexpr result(T &&v)
    requires std::is_move_constructible_v<T>
      : m_has_value(true) {
    std::construct_at(&m_storage.value, std::move(v));
  }

  template <typename G>
    requires std::is_constructible_v<E, G &&>
  constexpr result(err<G> &&u) : m_has_value(false) {
    std::construct_at(&m_storage.error, std::move(u).error());
  }

  template <typename G>
    requires std::is_constructible_v<E, const G &>
  constexpr result(const err<G> &u) : m_has_value(false) {
    std::construct_at(&m_storage.error, u.error());
  }

  constexpr result(const result &o)
    requires(std::is_copy_constructible_v<T> && std::is_copy_constructible_v<E>)
      : m_has_value(o.m_has_value) {
    if (m_has_value)
      std::construct_at(&m_storage.value, o.m_storage.value);
    else
      std::construct_at(&m_storage.error, o.m_storage.error);
  }

  constexpr result(result &&o) noexcept(
      std::is_nothrow_move_constructible_v<T> &&
      std::is_nothrow_move_constructible_v<E>)
    requires(std::is_move_constructible_v<T> && std::is_move_constructible_v<E>)
      : m_has_value(o.m_has_value) {
    if (m_has_value)
      std::construct_at(&m_storage.value, std::move(o.m_storage.value));
    else
      std::construct_at(&m_storage.error, std::move(o.m_storage.error));
  }

  constexpr result &operator=(const result &o)
    requires(std::is_copy_constructible_v<T> &&
             std::is_copy_constructible_v<E> && std::is_copy_assignable_v<T> &&
             std::is_copy_assignable_v<E> &&
             std::is_nothrow_move_constructible_v<T> &&
             std::is_nothrow_move_constructible_v<E>)
  {
    if (this == &o)
      return *this;
    if (m_has_value == o.m_has_value) {
      if (m_has_value)
        m_storage.value = o.m_storage.value;
      else
        m_storage.error = o.m_storage.error;
    } else if (o.m_has_value) {
      T next(o.m_storage.value);
      std::destroy_at(&m_storage.error);
      std::construct_at(&m_storage.value, std::move(next));
      m_has_value = true;
    } else {
      E next(o.m_storage.error);
      std::destroy_at(&m_storage.value);
      std::construct_at(&m_storage.error, std::move(next));
      m_has_value = false;
    }
    return *this;
  }

  constexpr result &
  operator=(result &&o) noexcept(std::is_nothrow_move_assignable_v<T> &&
                                 std::is_nothrow_move_assignable_v<E>)
    requires(std::is_nothrow_move_constructible_v<T> &&
             std::is_nothrow_move_constructible_v<E> &&
             std::is_move_assignable_v<T> && std::is_move_assignable_v<E>)
  {
    if (this == &o)
      return *this;
    if (m_has_value == o.m_has_value) {
      if (m_has_value)
        m_storage.value = std::move(o.m_storage.value);
      else
        m_storage.error = std::move(o.m_storage.error);
    } else if (o.m_has_value) {
      std::destroy_at(&m_storage.error);
      std::construct_at(&m_storage.value, std::move(o.m_storage.value));
      m_has_value = true;
    } else {
      std::destroy_at(&m_storage.value);
      std::construct_at(&m_storage.error, std::move(o.m_storage.error));
      m_has_value = false;
    }
    return *this;
  }

  constexpr ~result() { destroy(); }

  NX_FORCE_INLINE constexpr explicit operator bool() const noexcept {
    return m_has_value;
  }
  NX_FORCE_INLINE constexpr bool has_value() const noexcept {
    return m_has_value;
  }

  NX_FORCE_INLINE constexpr T &operator*() & { return m_storage.value; }
  NX_FORCE_INLINE constexpr const T &operator*() const & {
    return m_storage.value;
  }
  NX_FORCE_INLINE constexpr T &&operator*() && {
    return std::move(m_storage.value);
  }

  NX_FORCE_INLINE constexpr T *operator->() { return &m_storage.value; }
  NX_FORCE_INLINE constexpr const T *operator->() const {
    return &m_storage.value;
  }

  constexpr T &value() & {
    if (NX_UNLIKELY(!m_has_value))
      abort();
    return m_storage.value;
  }
  constexpr const T &value() const & {
    if (NX_UNLIKELY(!m_has_value))
      abort();
    return m_storage.value;
  }
  constexpr T &&value() && {
    if (NX_UNLIKELY(!m_has_value))
      abort();
    return std::move(m_storage.value);
  }

  constexpr E &error() & {
    if (NX_UNLIKELY(m_has_value))
      abort();
    return m_storage.error;
  }
  constexpr const E &error() const & {
    if (NX_UNLIKELY(m_has_value))
      abort();
    return m_storage.error;
  }
  constexpr E &&error() && {
    if (NX_UNLIKELY(m_has_value))
      abort();
    return std::move(m_storage.error);
  }

  template <typename U> constexpr T value_or(U &&fallback) const & {
    return NX_LIKELY(m_has_value) ? m_storage.value
                                  : nx::cast<T>(std::forward<U>(fallback));
  }
  template <typename U> constexpr T value_or(U &&fallback) && {
    return NX_LIKELY(m_has_value) ? std::move(m_storage.value)
                                  : nx::cast<T>(std::forward<U>(fallback));
  }

  /// If a value is held, returns f(value) (which must itself be a result);
  /// otherwise forwards the current error.
  template <typename F> constexpr auto and_then(F &&f) & {
    using Ret = std::invoke_result_t<F, T &>;
    if (NX_LIKELY(m_has_value))
      return std::forward<F>(f)(m_storage.value);
    else
      return Ret(err<E>(m_storage.error));
  }
  template <typename F> constexpr auto and_then(F &&f) const & {
    using Ret = std::invoke_result_t<F, const T &>;
    if (NX_LIKELY(m_has_value))
      return std::forward<F>(f)(m_storage.value);
    else
      return Ret(err<E>(m_storage.error));
  }
  template <typename F> constexpr auto and_then(F &&f) && {
    using Ret = std::invoke_result_t<F, T &&>;
    if (NX_LIKELY(m_has_value))
      return std::forward<F>(f)(std::move(m_storage.value));
    else
      return Ret(err<E>(std::move(m_storage.error)));
  }

  /// If a value is held, maps it through f and rewraps as result<U, E>;
  /// otherwise forwards the error.
  template <typename F> constexpr auto transform(F &&f) & {
    using U = std::invoke_result_t<F, T &>;
    if (NX_LIKELY(m_has_value))
      return result<U, E>(std::forward<F>(f)(m_storage.value));
    else
      return result<U, E>(err<E>(m_storage.error));
  }
  template <typename F> constexpr auto transform(F &&f) const & {
    using U = std::invoke_result_t<F, const T &>;
    if (NX_LIKELY(m_has_value))
      return result<U, E>(std::forward<F>(f)(m_storage.value));
    else
      return result<U, E>(err<E>(m_storage.error));
  }
  template <typename F> constexpr auto transform(F &&f) && {
    using U = std::invoke_result_t<F, T &&>;
    if (NX_LIKELY(m_has_value))
      return result<U, E>(std::forward<F>(f)(std::move(m_storage.value)));
    else
      return result<U, E>(err<E>(std::move(m_storage.error)));
  }

  /// If an error is held, returns f(error) (which must itself be a result);
  /// otherwise forwards the current value.
  template <typename F> constexpr auto or_else(F &&f) & {
    using Ret = std::invoke_result_t<F, E &>;
    if (NX_UNLIKELY(!m_has_value))
      return std::forward<F>(f)(m_storage.error);
    else
      return Ret(m_storage.value);
  }
  template <typename F> constexpr auto or_else(F &&f) const & {
    using Ret = std::invoke_result_t<F, const E &>;
    if (NX_UNLIKELY(!m_has_value))
      return std::forward<F>(f)(m_storage.error);
    else
      return Ret(m_storage.value);
  }
  template <typename F> constexpr auto or_else(F &&f) && {
    using Ret = std::invoke_result_t<F, E &&>;
    if (NX_UNLIKELY(!m_has_value))
      return std::forward<F>(f)(std::move(m_storage.error));
    else
      return Ret(std::move(m_storage.value));
  }

  /// If an error is held, maps it through f and rewraps as result<T, G>;
  /// otherwise forwards the value.
  template <typename F> constexpr auto transform_error(F &&f) & {
    using G = std::invoke_result_t<F, E &>;
    if (NX_UNLIKELY(!m_has_value))
      return result<T, G>(err<G>(std::forward<F>(f)(m_storage.error)));
    else
      return result<T, G>(m_storage.value);
  }
  template <typename F> constexpr auto transform_error(F &&f) const & {
    using G = std::invoke_result_t<F, const E &>;
    if (NX_UNLIKELY(!m_has_value))
      return result<T, G>(err<G>(std::forward<F>(f)(m_storage.error)));
    else
      return result<T, G>(m_storage.value);
  }
  template <typename F> constexpr auto transform_error(F &&f) && {
    using G = std::invoke_result_t<F, E &&>;
    if (NX_UNLIKELY(!m_has_value))
      return result<T, G>(
          err<G>(std::forward<F>(f)(std::move(m_storage.error))));
    else
      return result<T, G>(std::move(m_storage.value));
  }

private:
  detail::storage<T, E> m_storage;
  bool m_has_value;

  constexpr void destroy() noexcept {
    if (m_has_value)
      std::destroy_at(&m_storage.value);
    else
      std::destroy_at(&m_storage.error);
  }
};

template <typename E> class result<void, E> {
public:
  using value_type = void;
  using error_type = E;

  constexpr result() : m_has_value(true) {}

  template <typename G>
    requires std::is_constructible_v<E, G &&>
  constexpr result(err<G> &&u) : m_has_value(false) {
    std::construct_at(&m_storage.error, std::move(u).error());
  }

  template <typename G>
    requires std::is_constructible_v<E, const G &>
  constexpr result(const err<G> &u) : m_has_value(false) {
    std::construct_at(&m_storage.error, u.error());
  }

  constexpr result(const result &o)
    requires std::is_copy_constructible_v<E>
      : m_has_value(o.m_has_value) {
    if (!m_has_value)
      std::construct_at(&m_storage.error, o.m_storage.error);
  }

  constexpr result(result &&o) noexcept(std::is_nothrow_move_constructible_v<E>)
    requires std::is_move_constructible_v<E>
      : m_has_value(o.m_has_value) {
    if (!m_has_value)
      std::construct_at(&m_storage.error, std::move(o.m_storage.error));
  }

  constexpr result &operator=(const result &o)
    requires(std::is_copy_constructible_v<E> && std::is_copy_assignable_v<E>)
  {
    if (this == &o)
      return *this;
    if (!m_has_value && !o.m_has_value)
      m_storage.error = o.m_storage.error;
    else if (m_has_value && !o.m_has_value) {
      std::construct_at(&m_storage.error, o.m_storage.error);
      m_has_value = false;
    } else if (!m_has_value && o.m_has_value) {
      std::destroy_at(&m_storage.error);
      m_has_value = true;
    }
    return *this;
  }

  constexpr result &
  operator=(result &&o) noexcept(std::is_nothrow_move_constructible_v<E> &&
                                 std::is_nothrow_move_assignable_v<E>)
    requires(std::is_move_constructible_v<E> && std::is_move_assignable_v<E>)
  {
    if (this == &o)
      return *this;
    if (!m_has_value && !o.m_has_value)
      m_storage.error = std::move(o.m_storage.error);
    else if (m_has_value && !o.m_has_value) {
      std::construct_at(&m_storage.error, std::move(o.m_storage.error));
      m_has_value = false;
    } else if (!m_has_value && o.m_has_value) {
      std::destroy_at(&m_storage.error);
      m_has_value = true;
    }
    return *this;
  }

  constexpr ~result() { destroy(); }

  NX_FORCE_INLINE constexpr explicit operator bool() const noexcept {
    return m_has_value;
  }
  NX_FORCE_INLINE constexpr bool has_value() const noexcept {
    return m_has_value;
  }

  constexpr void value() const {
    if (NX_UNLIKELY(!m_has_value))
      abort();
  }
  constexpr void operator*() const noexcept {}

  constexpr E &error() & {
    if (NX_UNLIKELY(m_has_value))
      abort();
    return m_storage.error;
  }
  constexpr const E &error() const & {
    if (NX_UNLIKELY(m_has_value))
      abort();
    return m_storage.error;
  }
  constexpr E &&error() && {
    if (NX_UNLIKELY(m_has_value))
      abort();
    return std::move(m_storage.error);
  }

  template <typename F> constexpr auto and_then(F &&f) & {
    using Ret = std::invoke_result_t<F>;
    if (NX_LIKELY(m_has_value))
      return std::forward<F>(f)();
    else
      return Ret(err<E>(m_storage.error));
  }
  template <typename F> constexpr auto and_then(F &&f) const & {
    using Ret = std::invoke_result_t<F>;
    if (NX_LIKELY(m_has_value))
      return std::forward<F>(f)();
    else
      return Ret(err<E>(m_storage.error));
  }
  template <typename F> constexpr auto and_then(F &&f) && {
    using Ret = std::invoke_result_t<F>;
    if (NX_LIKELY(m_has_value))
      return std::forward<F>(f)();
    else
      return Ret(err<E>(std::move(m_storage.error)));
  }

  template <typename F> constexpr auto transform(F &&f) & {
    using U = std::invoke_result_t<F>;
    if (NX_LIKELY(m_has_value))
      return result<U, E>(std::forward<F>(f)());
    else
      return result<U, E>(err<E>(m_storage.error));
  }
  template <typename F> constexpr auto transform(F &&f) const & {
    using U = std::invoke_result_t<F>;
    if (NX_LIKELY(m_has_value))
      return result<U, E>(std::forward<F>(f)());
    else
      return result<U, E>(err<E>(m_storage.error));
  }
  template <typename F> constexpr auto transform(F &&f) && {
    using U = std::invoke_result_t<F>;
    if (NX_LIKELY(m_has_value))
      return result<U, E>(std::forward<F>(f)());
    else
      return result<U, E>(err<E>(std::move(m_storage.error)));
  }

  template <typename F> constexpr auto or_else(F &&f) & {
    using Ret = std::invoke_result_t<F, E &>;
    if (NX_UNLIKELY(!m_has_value))
      return std::forward<F>(f)(m_storage.error);
    else
      return Ret();
  }
  template <typename F> constexpr auto or_else(F &&f) const & {
    using Ret = std::invoke_result_t<F, const E &>;
    if (NX_UNLIKELY(!m_has_value))
      return std::forward<F>(f)(m_storage.error);
    else
      return Ret();
  }
  template <typename F> constexpr auto or_else(F &&f) && {
    using Ret = std::invoke_result_t<F, E &&>;
    if (NX_UNLIKELY(!m_has_value))
      return std::forward<F>(f)(std::move(m_storage.error));
    else
      return Ret();
  }

  template <typename F> constexpr auto transform_error(F &&f) & {
    using G = std::invoke_result_t<F, E &>;
    if (NX_UNLIKELY(!m_has_value))
      return result<void, G>(err<G>(std::forward<F>(f)(m_storage.error)));
    else
      return result<void, G>();
  }
  template <typename F> constexpr auto transform_error(F &&f) const & {
    using G = std::invoke_result_t<F, const E &>;
    if (NX_UNLIKELY(!m_has_value))
      return result<void, G>(err<G>(std::forward<F>(f)(m_storage.error)));
    else
      return result<void, G>();
  }
  template <typename F> constexpr auto transform_error(F &&f) && {
    using G = std::invoke_result_t<F, E &&>;
    if (NX_UNLIKELY(!m_has_value))
      return result<void, G>(
          err<G>(std::forward<F>(f)(std::move(m_storage.error))));
    else
      return result<void, G>();
  }

private:
  detail::storage<void, E> m_storage;
  bool m_has_value;

  constexpr void destroy() noexcept {
    if (!m_has_value)
      std::destroy_at(&m_storage.error);
  }
};

}
