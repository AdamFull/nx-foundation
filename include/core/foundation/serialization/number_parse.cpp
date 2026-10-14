#include "core/foundation/serialization/number_parse.h"

#if defined(NX_FORCE_LOCALE_NUMBER_FALLBACK) ||                                \
    defined(NX_PLATFORM_ANDROID) || defined(NX_PLATFORM_APPLE) ||              \
    !defined(__cpp_lib_to_chars)
#define NX_NUMBER_PARSE_LOCALE_FALLBACK 1
#endif

#if defined(NX_NUMBER_PARSE_LOCALE_FALLBACK)
#include <cerrno>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

#if defined(NX_PLATFORM_WINDOWS)
#include <locale.h>
#elif defined(NX_PLATFORM_POSIX)
#include <locale.h>
#if defined(NX_PLATFORM_APPLE)
#include <xlocale.h>
#endif
#else
#error                                                                         \
    "Locale-independent floating-point parsing is unsupported on this platform"
#endif
#endif

namespace nx::number {
namespace {

#if defined(NX_NUMBER_PARSE_LOCALE_FALLBACK)

[[nodiscard]] bool is_leading_space(const char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

#if defined(NX_PLATFORM_WINDOWS)

[[nodiscard]] _locale_t c_numeric_locale() noexcept {
  // A single process-lifetime locale avoids static-destruction order hazards.
  static _locale_t locale = _create_locale(LC_NUMERIC, "C");
  return locale;
}

[[nodiscard]] double parse_c_locale(const char *text, char **end,
                                    int &parse_errno) noexcept {
  const _locale_t locale = c_numeric_locale();
  if (locale == nullptr) {
    parse_errno = EINVAL;
    return 0.0;
  }

  errno = 0;
  const double value = _strtod_l(text, end, locale);
  parse_errno = errno;
  return value;
}

#elif defined(NX_PLATFORM_POSIX)

[[nodiscard]] locale_t c_numeric_locale() noexcept {
  // A single process-lifetime locale avoids static-destruction order hazards.
  static locale_t locale = ::newlocale(LC_NUMERIC_MASK, "C", nullptr);
  return locale;
}

class thread_locale_scope {
public:
  explicit thread_locale_scope(const locale_t locale) noexcept
      : m_previous(locale != nullptr ? ::uselocale(locale) : locale_t{}) {}

  ~thread_locale_scope() {
    if (m_previous != locale_t{})
      (void)::uselocale(m_previous);
  }

  [[nodiscard]] bool active() const noexcept {
    return m_previous != locale_t{};
  }

private:
  locale_t m_previous{};
};

[[nodiscard]] double parse_c_locale(const char *text, char **end,
                                    int &parse_errno) noexcept {
  const thread_locale_scope locale_scope(c_numeric_locale());
  if (!locale_scope.active()) {
    parse_errno = EINVAL;
    return 0.0;
  }

  errno = 0;
  const double value = std::strtod(text, end);
  parse_errno = errno;
  return value;
}

#endif

[[nodiscard]] std::from_chars_result
parse_f64_fallback(const char *first, const char *last, f64 &out) noexcept {
  const usize length = nx::cast<usize>(last - first);
  if (length == 0 || is_leading_space(*first) || *first == '+')
    return {first, std::errc::invalid_argument};

  // strtod accepts a 0x prefix in every format, while general-format
  // from_chars stops after the leading zero. Limit that one case explicitly.
  usize parse_length = length;
  const usize prefix = *first == '-' ? 1u : 0u;
  if (length >= prefix + 2u && first[prefix] == '0' &&
      (first[prefix + 1u] == 'x' || first[prefix + 1u] == 'X'))
    parse_length = prefix + 1u;

  if (parse_length == std::numeric_limits<usize>::max())
    return {first, std::errc::not_enough_memory};

  char local[512];
  std::unique_ptr<char[]> allocated;
  char *buffer = local;
  if (parse_length >= sizeof(local)) {
    allocated.reset(new (std::nothrow) char[parse_length + 1u]);
    if (!allocated)
      return {first, std::errc::not_enough_memory};
    buffer = allocated.get();
  }

  std::memcpy(buffer, first, parse_length);
  buffer[parse_length] = '\0';

  char *end = nullptr;
  int parse_errno = 0;
  const double value = parse_c_locale(buffer, &end, parse_errno);
  if (end == nullptr)
    return {first, std::errc::operation_not_supported};
  if (end == buffer)
    return {first, std::errc::invalid_argument};

  out = nx::cast<f64>(value);
  return {first + (end - buffer),
          parse_errno == ERANGE ? std::errc::result_out_of_range : std::errc{}};
}

#endif

} // namespace

std::from_chars_result parse_f64(const char *first, const char *last,
                                 f64 &out) noexcept {
#if defined(NX_NUMBER_PARSE_LOCALE_FALLBACK)
  return parse_f64_fallback(first, last, out);
#else
  return std::from_chars(first, last, out);
#endif
}

} // namespace nx::number
