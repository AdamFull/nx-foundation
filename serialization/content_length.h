#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string_view.h"

namespace nx {

inline constexpr usize CONTENT_LENGTH_MAX_HEADER = 8u * 1024u;

enum class ContentLengthStatus : u8 { Incomplete, Ready, Invalid };

struct ContentLengthFrame {
  ContentLengthStatus status = ContentLengthStatus::Incomplete;
  usize body_offset = 0;
  usize body_size = 0;
  usize consumed = 0;
};

[[nodiscard]] ContentLengthFrame
content_length_frame(nx::string_view buffered, usize max_body,
                     usize max_header = CONTENT_LENGTH_MAX_HEADER) noexcept;

} // namespace nx
