#pragma once

/**
 * @file string_map.h
 * @brief hash_map keyed by string, without the lookup allocation
 * (nx::string_map).
 */

#include "foundation/containers/hash_map.h"
#include "foundation/strings/utf8_string.h"

namespace nx {

/// Transparent, which is the point: hash_map's default hasher is std::hash and
/// is not, so `map.find(some_view)` on a string-keyed map builds a whole string
/// to probe with and throws it away.
struct string_hash {
  using is_transparent = void;
  [[nodiscard]] usize operator()(const string_view s) const noexcept {
    return cast<usize>(hash_fnv1a64(s.data(), s.size()));
  }
};

struct string_equal {
  using is_transparent = void;
  [[nodiscard]] bool operator()(const string_view a,
                                const string_view b) const noexcept {
    return a == b;
  }
};

/// Lookups take a string_view, a string or a literal and allocate for none of
/// them. Insertion still needs an owned key.
template <typename T, typename Alloc = Allocator<std::pair<const string, T>>>
using string_map = flat_hash_map<string, T, string_hash, string_equal, Alloc>;

} // namespace nx
