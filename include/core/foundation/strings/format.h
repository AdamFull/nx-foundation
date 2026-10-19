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
    bool *cut;
    using difference_type = std::ptrdiff_t;
    capped_out_it &operator++() noexcept { return *this; }
    capped_out_it operator++(int) noexcept { return *this; }
    capped_out_it &operator*() noexcept { return *this; }
    capped_out_it &operator=(const char c) noexcept {
      if (*cur < limit)
        *(*cur)++ = c;
      else
        *cut = true;
      return *this;
    }
  };

  char *cur = buf;
  bool cut = false;
  try {
    std::vformat_to(capped_out_it{&cur, buf + N - 1, &cut}, fmt,
                    std::make_format_args(args...));
    if (cut)
      cur = buf +
            utf8::complete_prefix(string_view(buf, nx::cast<usize>(cur - buf)));
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

} // namespace nx
