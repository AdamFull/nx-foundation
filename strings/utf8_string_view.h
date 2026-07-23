#pragma once

/**
 * @file utf8_string_view.h
 * @brief Non-owning UTF-8 string view alias (nx::string_view =
 * std::string_view).
 */

#include <string_view>

namespace nx {
using string_view = std::string_view;

template <typename> inline constexpr bool is_string_view = false;
template <typename C, typename Tr>
inline constexpr bool is_string_view<std::basic_string_view<C, Tr>> = true;
} // namespace nx