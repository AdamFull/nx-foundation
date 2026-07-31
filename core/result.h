#pragma once

/**
 * @file result.h
 * @brief Error-or-value type nx::result and its error wrapper nx::err.
 */
#include "core/foundation/core/foundation.h"

namespace nx {

/**
 * @brief Wraps a value as the error alternative when building a result.
 *
 * Returning err{e} from a function whose return type is result<T, E> constructs
 * the error state. The class template argument is deduced from the argument.
 *
 * @tparam E Error type being carried.
 */
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

} // namespace detail

/**
 * @brief Holds either a value of type @p T or an error of type @p E.
 *
 * A discriminated union for error handling without exceptions. Construct from a
 * T for the value state or from nx::err for the error state; test with
 * operator bool or has_value(). Access the value with operator*, operator->,
 * value() or value_or(), and the error with error(). Offers the monadic
 * combinators and_then, transform, or_else and transform_error for chaining.
 *
 * @tparam T Value type.
 * @tparam E Error type.
 */
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

  constexpr result(const T &v) : m_has_value(true) {
    std::construct_at(&m_storage.value, v);
  }

  constexpr result(T &&v) : m_has_value(true) {
    std::construct_at(&m_storage.value, std::move(v));
  }

  template <typename G> constexpr result(err<G> &&u) : m_has_value(false) {
    std::construct_at(&m_storage.error, std::move(u).error());
  }

  template <typename G> constexpr result(const err<G> &u) : m_has_value(false) {
    std::construct_at(&m_storage.error, u.error());
  }

  constexpr result(const result &o) : m_has_value(o.m_has_value) {
    if (m_has_value)
      std::construct_at(&m_storage.value, o.m_storage.value);
    else
      std::construct_at(&m_storage.error, o.m_storage.error);
  }

  constexpr result(result &&o) noexcept : m_has_value(o.m_has_value) {
    if (m_has_value)
      std::construct_at(&m_storage.value, std::move(o.m_storage.value));
    else
      std::construct_at(&m_storage.error, std::move(o.m_storage.error));
  }

  constexpr result &operator=(const result &o) {
    if (this == &o)
      return *this;
    destroy();
    m_has_value = o.m_has_value;
    if (m_has_value)
      std::construct_at(&m_storage.value, o.m_storage.value);
    else
      std::construct_at(&m_storage.error, o.m_storage.error);
    return *this;
  }

  constexpr result &operator=(result &&o) noexcept {
    if (this == &o)
      return *this;
    destroy();
    m_has_value = o.m_has_value;
    if (m_has_value)
      std::construct_at(&m_storage.value, std::move(o.m_storage.value));
    else
      std::construct_at(&m_storage.error, std::move(o.m_storage.error));
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

/**
 * @brief result specialization carrying no value, only success or an error.
 *
 * Models an operation that either succeeds or fails with an error of type @p E.
 *
 * @tparam E Error type.
 */
template <typename E> class result<void, E> {
public:
  using value_type = void;
  using error_type = E;

  constexpr result() : m_has_value(true) {}

  template <typename G> constexpr result(err<G> &&u) : m_has_value(false) {
    std::construct_at(&m_storage.error, std::move(u).error());
  }

  template <typename G> constexpr result(const err<G> &u) : m_has_value(false) {
    std::construct_at(&m_storage.error, u.error());
  }

  constexpr result(const result &o) : m_has_value(o.m_has_value) {
    if (!m_has_value)
      std::construct_at(&m_storage.error, o.m_storage.error);
  }

  constexpr result(result &&o) noexcept : m_has_value(o.m_has_value) {
    if (!m_has_value)
      std::construct_at(&m_storage.error, std::move(o.m_storage.error));
  }

  constexpr result &operator=(const result &o) {
    if (this == &o)
      return *this;
    destroy();
    m_has_value = o.m_has_value;
    if (!m_has_value)
      std::construct_at(&m_storage.error, o.m_storage.error);
    return *this;
  }

  constexpr result &operator=(result &&o) noexcept {
    if (this == &o)
      return *this;
    destroy();
    m_has_value = o.m_has_value;
    if (!m_has_value)
      std::construct_at(&m_storage.error, std::move(o.m_storage.error));
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

} // namespace nx