#pragma once

#include "core/foundation/containers/raw_hash_table.h"
#include "core/foundation/containers/slot_policy.h"

#include <memory>
#include <utility>

namespace nx {

/// Element order is unspecified and not stable across rehash. Elements are
/// keys, so iterators never give mutable access.
template <typename Key, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Alloc = Allocator<Key>>
class flat_hash_set
    : public detail::raw_hash_table<detail::set_slot_policy<Key>, Hash,
                                    KeyEqual, Alloc> {
  using base = detail::raw_hash_table<detail::set_slot_policy<Key>, Hash,
                                      KeyEqual, Alloc>;

public:
  using base::base;
};

template <typename Key, typename Hasher = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Alloc = Allocator<Key>>
using hash_set = flat_hash_set<Key, Hasher, KeyEqual, Alloc>;

} // namespace nx
