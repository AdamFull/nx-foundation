#pragma once

#include "core/foundation/core/foundation.h"

#include <functional>
#include <type_traits>
#include <utility>

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

template <typename T, typename E> class result;

namespace detail {

template <typename T> inline constexpr bool is_err = false;
template <typename E> inline constexpr bool is_err<err<E>> = true;

template <typename T> inline constexpr bool is_result = false;
template <typename T, typename E>
inline constexpr bool is_result<result<T, E>> = true;

/// f(args...) as a result<U, E>, where a void f gives result<void, E>.
template <typename E, typename F, typename... Args>
constexpr auto transform_into(F &&f, Args &&...args) {
  using U = std::remove_cv_t<std::invoke_result_t<F, Args...>>;
  if constexpr (std::is_void_v<U>) {
    std::invoke(std::forward<F>(f), std::forward<Args>(args)...);
    return result<void, E>();
  } else {
    return result<U, E>(
        std::invoke(std::forward<F>(f), std::forward<Args>(args)...));
  }
}

template <typename T, typename E> union storage {
  T value;
  E error;

  constexpr storage() {}
  constexpr ~storage() {}
};

template <typename E> union storage<void, E> {
  // Unlike storage<T, E>, there's no "value" member here for the default
  // constructor to activate - GCC's constexpr evaluator (unlike Clang's)
  // treats a union left with no active member as incompletely initialized,
  // rejecting `constexpr result<void, E> ok;` as not a constant expression.
  // This dummy member gives the default constructor something to activate.
  struct empty_t {} none;
  E error;

  constexpr storage() : none() {}
  constexpr ~storage() {}
};

}

/// A value or an error. Discarding one is a warning: an error left unread is
/// a failure nobody handled.
template <typename T, typename E> class [[nodiscard]] result {
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

  /// From anything T is made from, as std::expected takes it: implicit when
  /// the conversion is.
  template <typename U>
    requires(!std::is_same_v<std::remove_cvref_t<U>, T> &&
             !std::is_same_v<std::remove_cvref_t<U>, result> &&
             !detail::is_err<std::remove_cvref_t<U>> &&
             std::is_constructible_v<T, U &&>)
  constexpr explicit(!std::is_convertible_v<U &&, T>) result(U &&v)
      : m_has_value(true) {
    std::construct_at(&m_storage.value, std::forward<U>(v));
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

  /// If a value is held, maps it through f, which may return void; otherwise
  /// forwards the error.
  template <typename F> constexpr auto transform(F &&f) & {
    using Ret = decltype(detail::transform_into<E>(std::forward<F>(f),
                                                   m_storage.value));
    if (NX_LIKELY(m_has_value))
      return detail::transform_into<E>(std::forward<F>(f), m_storage.value);
    else
      return Ret(err<E>(m_storage.error));
  }
  template <typename F> constexpr auto transform(F &&f) const & {
    using Ret = decltype(detail::transform_into<E>(std::forward<F>(f),
                                                   m_storage.value));
    if (NX_LIKELY(m_has_value))
      return detail::transform_into<E>(std::forward<F>(f), m_storage.value);
    else
      return Ret(err<E>(m_storage.error));
  }
  template <typename F> constexpr auto transform(F &&f) && {
    using Ret = decltype(detail::transform_into<E>(std::forward<F>(f),
                                                   std::move(m_storage.value)));
    if (NX_LIKELY(m_has_value))
      return detail::transform_into<E>(std::forward<F>(f),
                                       std::move(m_storage.value));
    else
      return Ret(err<E>(std::move(m_storage.error)));
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

  /// The error, or @p fallback when a value is held.
  template <typename G = E> constexpr E error_or(G &&fallback) const & {
    return NX_UNLIKELY(!m_has_value)
               ? m_storage.error
               : static_cast<E>(std::forward<G>(fallback));
  }
  template <typename G = E> constexpr E error_or(G &&fallback) && {
    return NX_UNLIKELY(!m_has_value)
               ? std::move(m_storage.error)
               : static_cast<E>(std::forward<G>(fallback));
  }

  /// Replaces whatever is held with a value made from @p args.
  template <typename... Args>
    requires std::is_nothrow_constructible_v<T, Args...>
  constexpr T &emplace(Args &&...args) noexcept {
    destroy();
    std::construct_at(&m_storage.value, std::forward<Args>(args)...);
    m_has_value = true;
    return m_storage.value;
  }

  constexpr void swap(result &o) noexcept(std::is_nothrow_swappable_v<T> &&
                                          std::is_nothrow_swappable_v<E>)
    requires(std::is_nothrow_move_constructible_v<T> &&
             std::is_nothrow_move_constructible_v<E> &&
             std::is_swappable_v<T> && std::is_swappable_v<E>)
  {
    using std::swap;
    if (m_has_value && o.m_has_value) {
      swap(m_storage.value, o.m_storage.value);
    } else if (!m_has_value && !o.m_has_value) {
      swap(m_storage.error, o.m_storage.error);
    } else {
      result &valued = m_has_value ? *this : o;
      result &failed = m_has_value ? o : *this;
      E moved(std::move(failed.m_storage.error));
      std::destroy_at(&failed.m_storage.error);
      std::construct_at(&failed.m_storage.value,
                        std::move(valued.m_storage.value));
      std::destroy_at(&valued.m_storage.value);
      std::construct_at(&valued.m_storage.error, std::move(moved));
      failed.m_has_value = true;
      valued.m_has_value = false;
    }
  }
  friend constexpr void swap(result &a, result &b) noexcept(noexcept(a.swap(b)))
    requires requires { a.swap(b); }
  {
    a.swap(b);
  }

  template <typename T2, typename E2>
    requires(!std::is_void_v<T2>)
  friend constexpr bool operator==(const result &a, const result<T2, E2> &b) {
    if (a.has_value() != b.has_value())
      return false;
    return a.has_value() ? *a == *b : a.error() == b.error();
  }
  template <typename U>
    requires(!detail::is_result<U> && !detail::is_err<U>)
  friend constexpr bool operator==(const result &a, const U &v) {
    return a.has_value() && *a == v;
  }
  template <typename G>
  friend constexpr bool operator==(const result &a, const err<G> &e) {
    return !a.has_value() && a.error() == e.error();
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

template <typename E> class [[nodiscard]] result<void, E> {
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
    using Ret = decltype(detail::transform_into<E>(std::forward<F>(f)));
    if (NX_LIKELY(m_has_value))
      return detail::transform_into<E>(std::forward<F>(f));
    else
      return Ret(err<E>(m_storage.error));
  }
  template <typename F> constexpr auto transform(F &&f) const & {
    using Ret = decltype(detail::transform_into<E>(std::forward<F>(f)));
    if (NX_LIKELY(m_has_value))
      return detail::transform_into<E>(std::forward<F>(f));
    else
      return Ret(err<E>(m_storage.error));
  }
  template <typename F> constexpr auto transform(F &&f) && {
    using Ret = decltype(detail::transform_into<E>(std::forward<F>(f)));
    if (NX_LIKELY(m_has_value))
      return detail::transform_into<E>(std::forward<F>(f));
    else
      return Ret(err<E>(std::move(m_storage.error)));
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

  template <typename G = E> constexpr E error_or(G &&fallback) const & {
    return NX_UNLIKELY(!m_has_value)
               ? m_storage.error
               : static_cast<E>(std::forward<G>(fallback));
  }
  template <typename G = E> constexpr E error_or(G &&fallback) && {
    return NX_UNLIKELY(!m_has_value)
               ? std::move(m_storage.error)
               : static_cast<E>(std::forward<G>(fallback));
  }

  /// Clears any error: the result then holds its (void) value.
  constexpr void emplace() noexcept {
    destroy();
    m_has_value = true;
  }

  constexpr void swap(result &o) noexcept(std::is_nothrow_swappable_v<E>)
    requires(std::is_nothrow_move_constructible_v<E> && std::is_swappable_v<E>)
  {
    using std::swap;
    if (!m_has_value && !o.m_has_value) {
      swap(m_storage.error, o.m_storage.error);
    } else if (m_has_value != o.m_has_value) {
      result &valued = m_has_value ? *this : o;
      result &failed = m_has_value ? o : *this;
      std::construct_at(&valued.m_storage.error,
                        std::move(failed.m_storage.error));
      std::destroy_at(&failed.m_storage.error);
      failed.m_has_value = true;
      valued.m_has_value = false;
    }
  }
  friend constexpr void swap(result &a, result &b) noexcept(noexcept(a.swap(b)))
    requires requires { a.swap(b); }
  {
    a.swap(b);
  }

  template <typename E2>
  friend constexpr bool operator==(const result &a, const result<void, E2> &b) {
    if (a.has_value() != b.has_value())
      return false;
    return a.has_value() || a.error() == b.error();
  }
  template <typename G>
  friend constexpr bool operator==(const result &a, const err<G> &e) {
    return !a.has_value() && a.error() == e.error();
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
