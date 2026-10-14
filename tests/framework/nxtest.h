#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace nxtest {

using test_fn = void (*)();

struct test_case {
  const char *name;
  const char *file;
  int line;
  test_fn fn;
};

void register_test(const test_case &tc);

struct registrar {
  registrar(const char *name, const char *file, int line, test_fn fn) {
    register_test(test_case{name, file, line, fn});
  }
};

inline constexpr int SKIP_EXIT_CODE = 77;

int run(int argc, char **argv);

struct require_failed {};

struct skipped {};

[[noreturn]] void report_skip(const char *file, int line,
                              const std::string &reason);

void report_failure(const char *file, int line, const char *macro,
                    const char *expr, const std::string &decomposed);
void report_success();

void push_subcase(const char *name);
void pop_subcase();

template <typename T>
concept streamable = requires(std::ostream &os, const T &v) { os << v; };

inline std::string stringify(const char *value) {
  return value ? std::string("\"") + value + "\"" : std::string("nullptr");
}
inline std::string stringify(const std::string &value) {
  return "\"" + value + "\"";
}
inline std::string stringify(std::string_view value) {
  return "\"" + std::string(value) + "\"";
}

template <typename T> std::string stringify(const T &value) {
  if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
    return "nullptr";
  } else if constexpr (std::is_enum_v<T>) {
    std::ostringstream os;
    os << static_cast<long long>(static_cast<std::underlying_type_t<T>>(value));
    return os.str();
  } else if constexpr (std::is_same_v<T, char>) {
    return std::string("'") + value + "'";
  } else if constexpr (std::is_same_v<T, signed char> ||
                       std::is_same_v<T, unsigned char>) {
    return std::to_string(static_cast<int>(value));
  } else if constexpr (std::is_pointer_v<T> &&
                       !std::is_function_v<std::remove_pointer_t<T>>) {
    std::ostringstream os;
    os << static_cast<const void *>(value);
    return os.str();
  } else if constexpr (std::is_pointer_v<T>) {
    return value != nullptr ? "<function>" : "nullptr";
  } else if constexpr (streamable<T>) {
    std::ostringstream os;
    os << value;
    return os.str();
  } else {
    return "{?}";
  }
}

namespace detail {

template <typename L, typename R>
inline constexpr bool mixed_signed_integrals =
    std::is_integral_v<L> && std::is_integral_v<R> &&
    !std::is_same_v<std::remove_cv_t<L>, bool> &&
    !std::is_same_v<std::remove_cv_t<R>, bool> &&
    (std::is_signed_v<L> != std::is_signed_v<R>);

template <typename L, typename R>
[[nodiscard]] constexpr bool equal(const L &lhs, const R &rhs) {
  if constexpr (!mixed_signed_integrals<L, R>) {
    return lhs == rhs;
  } else if constexpr (std::is_signed_v<L>) {
    return lhs >= 0 && static_cast<std::make_unsigned_t<L>>(lhs) == rhs;
  } else {
    return rhs >= 0 && lhs == static_cast<std::make_unsigned_t<R>>(rhs);
  }
}

template <typename L, typename R>
[[nodiscard]] constexpr bool less(const L &lhs, const R &rhs) {
  if constexpr (!mixed_signed_integrals<L, R>) {
    return lhs < rhs;
  } else if constexpr (std::is_signed_v<L>) {
    return lhs < 0 || static_cast<std::make_unsigned_t<L>>(lhs) < rhs;
  } else {
    return rhs >= 0 && lhs < static_cast<std::make_unsigned_t<R>>(rhs);
  }
}

template <typename L, typename R>
[[nodiscard]] constexpr bool not_equal(const L &lhs, const R &rhs) {
  if constexpr (mixed_signed_integrals<L, R>)
    return !equal(lhs, rhs);
  else
    return lhs != rhs;
}

template <typename L, typename R>
[[nodiscard]] constexpr bool less_equal(const L &lhs, const R &rhs) {
  if constexpr (mixed_signed_integrals<L, R>)
    return !less(rhs, lhs);
  else
    return lhs <= rhs;
}

template <typename L, typename R>
[[nodiscard]] constexpr bool greater(const L &lhs, const R &rhs) {
  if constexpr (mixed_signed_integrals<L, R>)
    return less(rhs, lhs);
  else
    return lhs > rhs;
}

template <typename L, typename R>
[[nodiscard]] constexpr bool greater_equal(const L &lhs, const R &rhs) {
  if constexpr (mixed_signed_integrals<L, R>)
    return !less(lhs, rhs);
  else
    return lhs >= rhs;
}

}

template <typename L> struct lhs_expr {
  const L &lhs;
  bool value = false;
  std::string text;

  explicit lhs_expr(const L &l) : lhs(l) {
    if constexpr (requires { static_cast<bool>(l); })
      value = static_cast<bool>(l);
  }

#define NXTEST_BINARY_OP(op, compare)                                          \
  template <typename R> lhs_expr &operator op(const R &rhs) {                  \
    value = compare;                                                           \
    text = stringify(lhs) + " " #op " " + stringify(rhs);                      \
    return *this;                                                              \
  }

  NXTEST_BINARY_OP(==, detail::equal(lhs, rhs))
  NXTEST_BINARY_OP(!=, detail::not_equal(lhs, rhs))
  NXTEST_BINARY_OP(<, detail::less(lhs, rhs))
  NXTEST_BINARY_OP(<=, detail::less_equal(lhs, rhs))
  NXTEST_BINARY_OP(>, detail::greater(lhs, rhs))
  NXTEST_BINARY_OP(>=, detail::greater_equal(lhs, rhs))

#undef NXTEST_BINARY_OP

  std::string message() const { return text.empty() ? stringify(lhs) : text; }
};

struct decomposer {
  template <typename L> lhs_expr<L> operator<<(const L &lhs) const {
    return lhs_expr<L>(lhs);
  }
};

class Approx {
public:
  explicit Approx(double value) : m_value(value) {}

  Approx &epsilon(double eps) {
    m_epsilon = eps;
    return *this;
  }
  Approx &margin(double m) {
    m_margin = m;
    return *this;
  }

  friend bool operator==(double lhs, const Approx &rhs) {
    const double diff =
        lhs > rhs.m_value ? lhs - rhs.m_value : rhs.m_value - lhs;
    if (diff <= rhs.m_margin)
      return true;
    const double la = lhs < 0 ? -lhs : lhs;
    const double ra = rhs.m_value < 0 ? -rhs.m_value : rhs.m_value;
    return diff <= rhs.m_epsilon * (la > ra ? la : ra);
  }
  friend bool operator==(const Approx &lhs, double rhs) { return rhs == lhs; }
  friend bool operator!=(double lhs, const Approx &rhs) {
    return !(lhs == rhs);
  }
  friend bool operator!=(const Approx &lhs, double rhs) {
    return !(rhs == lhs);
  }

  friend std::ostream &operator<<(std::ostream &os, const Approx &a) {
    return os << "Approx(" << a.m_value << ")";
  }

private:
  double m_value;
  double m_epsilon = 1e-6;
  double m_margin = 0.0;
};

struct subcase_scope {
  explicit subcase_scope(const char *name) { push_subcase(name); }
  ~subcase_scope() { pop_subcase(); }
};

}

#define NXTEST_CONCAT_(a, b) a##b
#define NXTEST_CONCAT(a, b) NXTEST_CONCAT_(a, b)
#define NXTEST_ANON(prefix) NXTEST_CONCAT(prefix, __LINE__)

#define TEST_CASE(name)                                                        \
  static void NXTEST_ANON(nxtest_fn_)();                                       \
  static const ::nxtest::registrar NXTEST_ANON(nxtest_reg_){                   \
      name, __FILE__, __LINE__, &NXTEST_ANON(nxtest_fn_)};                     \
  static void NXTEST_ANON(nxtest_fn_)()

#define SUBCASE(name)                                                          \
  if (const ::nxtest::subcase_scope NXTEST_ANON(nxtest_sub_){name}; true)

#define NXTEST_ASSERT(macro_name, expected, on_fail, ...)                      \
  do {                                                                         \
    auto nxtest_e_ = (::nxtest::decomposer{} << __VA_ARGS__);                  \
    if (nxtest_e_.value == (expected)) {                                       \
      ::nxtest::report_success();                                              \
    } else {                                                                   \
      ::nxtest::report_failure(__FILE__, __LINE__, macro_name, #__VA_ARGS__,   \
                               nxtest_e_.message());                           \
      on_fail;                                                                 \
    }                                                                          \
  } while (false)

#define CHECK(...) NXTEST_ASSERT("CHECK", true, (void)0, __VA_ARGS__)
#define CHECK_FALSE(...)                                                       \
  NXTEST_ASSERT("CHECK_FALSE", false, (void)0, __VA_ARGS__)
#define REQUIRE(...)                                                           \
  NXTEST_ASSERT("REQUIRE", true, throw ::nxtest::require_failed{}, __VA_ARGS__)
#define REQUIRE_FALSE(...)                                                     \
  NXTEST_ASSERT("REQUIRE_FALSE", false, throw ::nxtest::require_failed{},      \
                __VA_ARGS__)

#define CHECK_THROWS(...)                                                      \
  do {                                                                         \
    bool nxtest_threw_ = false;                                                \
    try {                                                                      \
      (void)(__VA_ARGS__);                                                     \
    } catch (const ::nxtest::require_failed &) {                               \
      throw;                                                                   \
    } catch (const ::nxtest::skipped &) {                                      \
      throw;                                                                   \
    } catch (...) {                                                            \
      nxtest_threw_ = true;                                                    \
    }                                                                          \
    if (nxtest_threw_)                                                         \
      ::nxtest::report_success();                                              \
    else                                                                       \
      ::nxtest::report_failure(__FILE__, __LINE__, "CHECK_THROWS",             \
                               #__VA_ARGS__, "did not throw");                 \
  } while (false)

#define CHECK_THROWS_AS(expr, ExceptionType)                                   \
  do {                                                                         \
    int nxtest_state_ = 0; /* 0 none, 1 right type, 2 wrong type */            \
    try {                                                                      \
      (void)(expr);                                                            \
    } catch (const ::nxtest::require_failed &) {                               \
      throw;                                                                   \
    } catch (const ::nxtest::skipped &) {                                      \
      throw;                                                                   \
    } catch (const ExceptionType &) {                                          \
      nxtest_state_ = 1;                                                       \
    } catch (...) {                                                            \
      nxtest_state_ = 2;                                                       \
    }                                                                          \
    if (nxtest_state_ == 1)                                                    \
      ::nxtest::report_success();                                              \
    else                                                                       \
      ::nxtest::report_failure(                                                \
          __FILE__, __LINE__, "CHECK_THROWS_AS", #expr,                        \
          nxtest_state_ == 0 ? "did not throw"                                 \
                             : "threw a different type than " #ExceptionType); \
  } while (false)

#define CHECK_NOTHROW(...)                                                     \
  do {                                                                         \
    try {                                                                      \
      (void)(__VA_ARGS__);                                                     \
      ::nxtest::report_success();                                              \
    } catch (const ::nxtest::require_failed &) {                               \
      throw;                                                                   \
    } catch (const ::nxtest::skipped &) {                                      \
      throw;                                                                   \
    } catch (...) {                                                            \
      ::nxtest::report_failure(__FILE__, __LINE__, "CHECK_NOTHROW",            \
                               #__VA_ARGS__, "threw an exception");            \
    }                                                                          \
  } while (false)

#define FAIL(msg) ::nxtest::report_failure(__FILE__, __LINE__, "FAIL", msg, "")

#define SKIP(reason) ::nxtest::report_skip(__FILE__, __LINE__, reason)

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NXTEST_ASAN 1
#endif
#elif defined(__SANITIZE_ADDRESS__)
#define NXTEST_ASAN 1
#endif
#ifndef NXTEST_ASAN
#define NXTEST_ASAN 0
#endif

#if NXTEST_ASAN && defined(_WIN32)
#define NXTEST_ASAN_UNWIND_BROKEN 1
#else
#define NXTEST_ASAN_UNWIND_BROKEN 0
#endif
