#pragma once

#include "core/foundation/strings/utf8_string.h"

#include <span>

namespace nx {

/// @p bytes in standard base64 (RFC 4648), padded with '='.
[[nodiscard]] inline string base64_encode(const std::span<const u8> bytes) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  string out;
  out.reserve(((bytes.size() + 2u) / 3u) * 4u);
  for (usize at = 0; at < bytes.size(); at += 3u) {
    const u32 a = bytes[at];
    const u32 b = at + 1u < bytes.size() ? bytes[at + 1u] : 0u;
    const u32 c = at + 2u < bytes.size() ? bytes[at + 2u] : 0u;
    const u32 word = (a << 16u) | (b << 8u) | c;
    out.push_back(alphabet[(word >> 18u) & 63u]);
    out.push_back(alphabet[(word >> 12u) & 63u]);
    out.push_back(at + 1u < bytes.size() ? alphabet[(word >> 6u) & 63u] : '=');
    out.push_back(at + 2u < bytes.size() ? alphabet[word & 63u] : '=');
  }
  return out;
}

} // namespace nx
