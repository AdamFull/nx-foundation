#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

#include <format>

namespace nx {
template <usize N, typename... Args>
const char *format_to(char (&buf)[N], string_view fmt,
                      Args &&...args) noexcept {
  static_assert(N > 0);
  struct capped_out_it {
    char **cur;
    char *limit;
    using difference_type = std::ptrdiff_t;
    capped_out_it &operator++() noexcept { return *this; }
    capped_out_it operator++(int) noexcept { return *this; }
    capped_out_it &operator*() noexcept { return *this; }
    capped_out_it &operator=(const char c) noexcept {
      if (*cur < limit)
        *(*cur)++ = c;
      return *this;
    }
  };

  char *cur = buf;
  try {
    std::vformat_to(capped_out_it{&cur, buf + N - 1}, fmt,
                    std::make_format_args(args...));
  } catch (...) {
    cur = buf;
    constexpr string_view fallback = "[format error]";
    for (const char c : fallback)
      if (cur < buf + N - 1)
        *cur++ = c;
  }
  *cur = '\0';
  return buf;
}

template <class... Args>
[[nodiscard]] string format(std::format_string<Args...> fmt, Args &&...args) {
  string out;
  std::vformat_to(std::back_inserter(out), fmt.get(),
                  std::make_format_args(args...));
  return out;
}

} // namespace nx

template <class A, class CharT>
struct std::formatter<nx::basic_string<A>, CharT>
    : std::formatter<std::basic_string_view<CharT>, CharT> {
  template <class FormatContext>
  auto format(const nx::basic_string<A> &s, FormatContext &ctx) const {
    using Base = std::formatter<std::basic_string_view<CharT>, CharT>;
    if constexpr (std::is_same_v<CharT, char>)
      return Base::format(std::string_view(s.c_str(), s.size()), ctx);
    else if constexpr (sizeof(CharT) == 2) {
      const std::u16string tmp = s.u16string();
      return Base::format(
          std::basic_string_view<CharT>(
              reinterpret_cast<const CharT *>(tmp.data()), tmp.size()),
          ctx);
    } else {
      const std::u32string tmp = s.u32string();
      return Base::format(
          std::basic_string_view<CharT>(
              reinterpret_cast<const CharT *>(tmp.data()), tmp.size()),
          ctx);
    }
  }
};
