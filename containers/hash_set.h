#pragma once

#include "core/foundation/containers/raw_hash_table.h"

#include <memory>
#include <utility>

namespace nx {
namespace detail {

template <typename Key> struct set_slot_policy {
  using key_type = Key;
  using value_type = Key;
  static constexpr bool constant_elements = true;

  union slot_type {
    Key value;
    slot_type() noexcept {}
    slot_type(const slot_type &) = delete;
    slot_type &operator=(const slot_type &) = delete;
    ~slot_type() {}
  };

  static const Key &key_of(const Key &v) noexcept { return v; }
  static Key &element(slot_type &s) noexcept { return s.value; }
  static const Key &element(const slot_type &s) noexcept { return s.value; }

  template <typename A, typename... Args>
  static void construct(A &alloc, slot_type *s, Args &&...args) {
    std::allocator_traits<A>::construct(alloc, std::addressof(s->value),
                                        std::forward<Args>(args)...);
  }
  template <typename A> static void destroy(A &alloc, slot_type *s) noexcept {
    std::allocator_traits<A>::destroy(alloc, std::addressof(s->value));
  }
  template <typename A>
  static void move_construct(A &alloc, slot_type *to, slot_type *from) {
    construct(alloc, to, std::move(from->value));
  }

  static bool equal_elements(const Key &, const Key &) noexcept { return true; }
};

} // namespace detail

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
