#pragma once

#include "core/foundation/containers/hash_map.h"
#include "core/foundation/strings/utf8_string.h"

namespace nx {

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

template <typename T, typename Alloc = Allocator<std::pair<const string, T>>>
using string_map = flat_hash_map<string, T, string_hash, string_equal, Alloc>;

}
