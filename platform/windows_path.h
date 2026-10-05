#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string_view.h"

#if defined(NX_PLATFORM_WINDOWS)

namespace nx::fs::detail {

/// A path at least this long, once resolved, is handed to Win32 in its
/// extended-length form. CreateDirectoryW refuses MAX_PATH less 12 (room
/// for an 8.3 name), the tightest of the plain-form limits.
inline constexpr usize WIN32_LONG_PATH = 260 - 12;

/// @p path as Win32 file APIs take it: UTF-16 and null-terminated, and, when
/// its full path is WIN32_LONG_PATH or longer, made absolute in the
/// extended-length form (`\\?\C:\...`, `\\?\UNC\server\share\...`), which
/// reaches 32767 characters. Empty when @p path is empty or not UTF-8.
[[nodiscard]] vector<wchar_t> win32_path(string_view path);

/// @p path, UTF-16 and null-terminated, in the extended-length form into
/// @p out when its full path needs it; false, leaving @p out alone, when the
/// plain form serves or it already has a prefix.
[[nodiscard]] bool extend_win32_path(const wchar_t *path, vector<wchar_t> &out);

} // namespace nx::fs::detail

#endif
