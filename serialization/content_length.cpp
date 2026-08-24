#include "core/foundation/serialization/content_length.h"

namespace nx {
namespace {

constexpr nx::string_view LENGTH = "Content-Length:";
constexpr nx::string_view LINE_END = "\r\n";
constexpr nx::string_view HEADER_END = "\r\n\r\n";

[[nodiscard]] nx::string_view trim_horizontal(nx::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
    value.remove_suffix(1);
  return value;
}

[[nodiscard]] bool parse_length(const nx::string_view text, const usize maximum,
                                usize &out) noexcept {
  const nx::string_view digits = trim_horizontal(text);
  if (digits.empty())
    return false;

  usize value = 0;
  for (const char c : digits) {
    if (c < '0' || c > '9')
      return false;
    const usize digit = static_cast<usize>(c - '0');
    if (digit > maximum || value > (maximum - digit) / 10u)
      return false;
    value = value * 10u + digit;
  }
  out = value;
  return true;
}

} // namespace

ContentLengthFrame content_length_frame(const nx::string_view buffered,
                                        const usize max_body,
                                        const usize max_header) noexcept {
  const usize header_end = buffered.find(HEADER_END);
  if (header_end == nx::string_view::npos) {
    return {buffered.size() > max_header ? ContentLengthStatus::Invalid
                                         : ContentLengthStatus::Incomplete};
  }
  if (header_end > max_header)
    return {ContentLengthStatus::Invalid};

  bool found = false;
  usize body_size = 0;
  usize line_start = 0;
  while (line_start < header_end) {
    usize line_end = buffered.find(LINE_END, line_start);
    if (line_end == nx::string_view::npos || line_end > header_end)
      line_end = header_end;
    const nx::string_view line =
        buffered.substr(line_start, line_end - line_start);
    if (line.starts_with(LENGTH)) {
      if (found ||
          !parse_length(line.substr(LENGTH.size()), max_body, body_size))
        return {ContentLengthStatus::Invalid};
      found = true;
    }
    if (line_end == header_end)
      break;
    line_start = line_end + LINE_END.size();
  }
  if (!found)
    return {ContentLengthStatus::Invalid};

  const usize body_offset = header_end + HEADER_END.size();
  const usize available = buffered.size() - body_offset;
  if (available < body_size)
    return {ContentLengthStatus::Incomplete};
  return {ContentLengthStatus::Ready, body_offset, body_size,
          body_offset + body_size};
}

} // namespace nx
