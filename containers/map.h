#pragma once

#include "core/foundation/core/foundation.h"

#include <functional>
#include <map>
#include <set>

namespace nx {

template <typename Key, typename T, typename Compare = std::less<Key>,
          typename Alloc = Allocator<std::pair<const Key, T>>>
using map = std::map<Key, T, Compare, Alloc>;

template <typename Key, typename Compare = std::less<Key>,
          typename Alloc = Allocator<Key>>
using set = std::set<Key, Compare, Alloc>;

} // namespace nx
