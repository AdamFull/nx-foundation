#pragma once

/**
 * @file scope_guard.h
 * @brief Run-on-scope-exit helpers: nx::scope_guard, scoped_exit and defer.
 */

#include <type_traits>
#include <utility>

namespace nx {

/**
 * @brief Runs a callable when it leaves scope, unless dismissed first.
 *
 * The action fires at most once. Call dismiss() to cancel it. Move-only: a
 * moved-from guard is already dismissed. Prefer scoped_exit() or the defer
 * macro for the common case.
 *
 * @tparam F Callable type invoked on scope exit.
 */
template <typename F> class [[nodiscard]] scope_guard {
public:
  explicit scope_guard(F fn) noexcept(std::is_nothrow_move_constructible_v<F>)
      : m_fn(std::move(fn)) {}

  scope_guard(scope_guard &&o) noexcept(std::is_nothrow_move_constructible_v<F>)
      : m_fn(std::move(o.m_fn)), m_active(o.m_active) {
    o.m_active = false;
  }

  scope_guard(const scope_guard &) = delete;
  scope_guard &operator=(const scope_guard &) = delete;
  scope_guard &operator=(scope_guard &&) = delete;

  ~scope_guard() {
    if (m_active)
      m_fn();
  }

  void dismiss() noexcept { m_active = false; }

private:
  F m_fn;
  bool m_active = true;
};

template <typename F> scope_guard(F) -> scope_guard<F>;

// Deduces the callable type and returns a guard that runs it on scope exit.
// Bind the result to a variable; discarding it runs the action immediately.
template <typename F>
[[nodiscard]] scope_guard<std::decay_t<F>> scoped_exit(F &&fn) noexcept(
    std::is_nothrow_constructible_v<std::decay_t<F>, F>) {
  return scope_guard<std::decay_t<F>>(std::forward<F>(fn));
}

namespace detail {
struct scope_guard_tag {};
template <typename F>
scope_guard<std::decay_t<F>> operator+(scope_guard_tag, F &&fn) {
  return scope_guard<std::decay_t<F>>(std::forward<F>(fn));
}
} // namespace detail

} // namespace nx

#define NX_SCOPE_CONCAT_(a, b) a##b
#define NX_SCOPE_CONCAT(a, b) NX_SCOPE_CONCAT_(a, b)
#ifdef __COUNTER__
#define NX_SCOPE_ANON(prefix) NX_SCOPE_CONCAT(prefix, __COUNTER__)
#else
#define NX_SCOPE_ANON(prefix) NX_SCOPE_CONCAT(prefix, __LINE__)
#endif

// Go-style deferred block that runs at scope exit, capturing by reference:
//   defer { cgltf_free(data); };
// Use scope_guard or scoped_exit instead when you need to dismiss the action.
#define defer                                                                  \
  const auto NX_SCOPE_ANON(nx_defer_) =                                        \
      ::nx::detail::scope_guard_tag{} + [&]() -> void