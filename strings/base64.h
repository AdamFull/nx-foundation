#pragma once

#include "core/foundation/strings/utf8_string.h"

#include <span>

namespace nx {

namespace detail {

[[nodiscard]] inline string base64_encode(const std::span<const u8> bytes,
                                          const char (&alphabet)[65],
                                          const bool pad) {
  string out;
  out.reserve(((bytes.size() + 2u) / 3u) * 4u);
  for (usize at = 0; at < bytes.size(); at += 3u) {
    const u32 a = bytes[at];
    const u32 b = at + 1u < bytes.size() ? bytes[at + 1u] : 0u;
    const u32 c = at + 2u < bytes.size() ? bytes[at + 2u] : 0u;
    const u32 word = (a << 16u) | (b << 8u) | c;
    out.push_back(alphabet[(word >> 18u) & 63u]);
    out.push_back(alphabet[(word >> 12u) & 63u]);
    if (at + 1u < bytes.size())
      out.push_back(alphabet[(word >> 6u) & 63u]);
    else if (pad)
      out.push_back('=');
    if (at + 2u < bytes.size())
      out.push_back(alphabet[word & 63u]);
    else if (pad)
      out.push_back('=');
  }
  return out;
}

} // namespace detail

/// @p bytes in standard base64 (RFC 4648), padded with '='.
[[nodiscard]] inline string base64_encode(const std::span<const u8> bytes) {
  return detail::base64_encode(
      bytes, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",
      true);
}

/// @p bytes in URL- and filename-safe base64 (RFC 4648 section 5), without
/// padding, as URLs and PKCE (RFC 7636) want it.
[[nodiscard]] inline string base64url_encode(const std::span<const u8> bytes) {
  return detail::base64_encode(
      bytes, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_",
      false);
}

} // namespace nx
