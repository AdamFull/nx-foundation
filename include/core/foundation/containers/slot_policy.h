#pragma once

#include <memory>
#include <utility>

namespace nx {
namespace detail {

/// How a container stores an element in a slot it constructs and destroys
/// itself: a map's pair, whose key moves although it is const to callers,
/// and a set's key.
template <typename Key, typename T> struct map_slot_policy {
  using key_type = Key;
  using value_type = std::pair<const Key, T>;
  static constexpr bool constant_elements = false;

  union slot_type {
    value_type value;
    std::pair<Key, T> mutable_value;
    slot_type() noexcept {}
    slot_type(const slot_type &) = delete;
    slot_type &operator=(const slot_type &) = delete;
    ~slot_type() {}
  };

  static const Key &key_of(const value_type &v) noexcept { return v.first; }
  static value_type &element(slot_type &s) noexcept { return s.value; }
  static const value_type &element(const slot_type &s) noexcept {
    return s.value;
  }

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
    construct(alloc, to, std::move(from->mutable_value.first),
              std::move(from->mutable_value.second));
  }

  static bool equal_elements(const value_type &a, const value_type &b) {
    return a.second == b.second;
  }
};

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
} // namespace nx
