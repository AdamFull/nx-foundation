#pragma once

#include <string_view>

namespace nx {
using string_view = std::string_view;

template <typename> inline constexpr bool is_string_view = false;
template <typename C, typename Tr>
inline constexpr bool is_string_view<std::basic_string_view<C, Tr>> = true;
}
