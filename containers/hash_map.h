#pragma once

#include "core/foundation/containers/raw_hash_table.h"

#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace nx {
namespace detail {

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

} // namespace detail

/// Element order is unspecified and not stable across rehash.
template <typename Key, typename T, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Alloc = Allocator<std::pair<const Key, T>>>
class flat_hash_map
    : public detail::raw_hash_table<detail::map_slot_policy<Key, T>, Hash,
                                    KeyEqual, Alloc> {
  using base = detail::raw_hash_table<detail::map_slot_policy<Key, T>, Hash,
                                      KeyEqual, Alloc>;
  using policy = detail::map_slot_policy<Key, T>;

public:
  using mapped_type = T;
  using typename base::const_iterator;
  using typename base::iterator;

  using base::base;

  template <typename... Args>
  std::pair<iterator, bool> try_emplace(const Key &k, Args &&...args) {
    return try_emplace_impl(k, std::forward<Args>(args)...);
  }
  template <typename... Args>
  std::pair<iterator, bool> try_emplace(Key &&k, Args &&...args) {
    return try_emplace_impl(std::move(k), std::forward<Args>(args)...);
  }

  template <typename M>
  std::pair<iterator, bool> insert_or_assign(const Key &k, M &&obj) {
    return insert_or_assign_impl(k, std::forward<M>(obj));
  }
  template <typename M>
  std::pair<iterator, bool> insert_or_assign(Key &&k, M &&obj) {
    return insert_or_assign_impl(std::move(k), std::forward<M>(obj));
  }

  T &operator[](const Key &k) { return try_emplace(k).first->second; }
  T &operator[](Key &&k) { return try_emplace(std::move(k)).first->second; }

  T &at(const Key &k) {
    const usize idx = this->find_index(k);
    if (idx == base::NPOS)
      throw std::out_of_range("flat_hash_map::at");
    return policy::element(*this->slot_at(idx)).second;
  }
  const T &at(const Key &k) const {
    const usize idx = this->find_index(k);
    if (idx == base::NPOS)
      throw std::out_of_range("flat_hash_map::at");
    return policy::element(*this->slot_at(idx)).second;
  }

private:
  template <typename K, typename... Args>
  std::pair<iterator, bool> try_emplace_impl(K &&k, Args &&...args) {
    auto [idx, inserted] = this->find_or_prepare_insert(k);
    if (inserted)
      this->construct(this->slot_at(idx), std::piecewise_construct,
                      std::forward_as_tuple(std::forward<K>(k)),
                      std::forward_as_tuple(std::forward<Args>(args)...));
    return {this->iterator_at(idx), inserted};
  }

  template <typename K, typename M>
  std::pair<iterator, bool> insert_or_assign_impl(K &&k, M &&obj) {
    auto [idx, inserted] = this->find_or_prepare_insert(k);
    if (inserted)
      this->construct(this->slot_at(idx), std::piecewise_construct,
                      std::forward_as_tuple(std::forward<K>(k)),
                      std::forward_as_tuple(std::forward<M>(obj)));
    else
      policy::element(*this->slot_at(idx)).second = std::forward<M>(obj);
    return {this->iterator_at(idx), inserted};
  }
};

template <typename Key, typename T, typename Hasher = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Alloc = Allocator<std::pair<const Key, T>>>
using hash_map = flat_hash_map<Key, T, Hasher, KeyEqual, Alloc>;

} // namespace nx
