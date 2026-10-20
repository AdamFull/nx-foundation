#pragma once

#include "core/foundation/containers/btree.h"
#include "core/foundation/core/foundation.h"

#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace nx {

/// An ordered map on a B-tree: elements move between nodes, so inserting
/// and erasing invalidate every iterator, pointer and reference into it.
/// Keys and values must move without throwing.
template <typename Key, typename T, typename Compare = std::less<Key>,
          typename Alloc = Allocator<std::pair<const Key, T>>>
class map
    : public detail::btree<detail::map_slot_policy<Key, T>, Compare, Alloc> {
  using base = detail::btree<detail::map_slot_policy<Key, T>, Compare, Alloc>;
  static_assert(std::is_nothrow_move_constructible_v<Key> &&
                    std::is_nothrow_move_constructible_v<T>,
                "nx::map moves its elements between nodes");

public:
  using mapped_type = T;
  using typename base::const_iterator;
  using typename base::iterator;
  using typename base::key_type;
  using typename base::size_type;
  using typename base::value_type;

  /// With a transparent comparison, a key a Key is made from looks up
  /// without making one, and becomes the Key only when it is inserted.
  template <typename K>
  static constexpr bool heterogeneous_key =
      base::TRANSPARENT && !std::is_same_v<std::remove_cvref_t<K>, Key> &&
      std::is_constructible_v<Key, K &&> &&
      !std::is_convertible_v<K &&, const_iterator> &&
      !std::is_convertible_v<K &&, iterator>;

  map() = default;
  explicit map(const Compare &comp, const Alloc &alloc = Alloc())
      : base(comp, alloc) {}
  explicit map(const Alloc &alloc) : base(alloc) {}
  map(std::initializer_list<value_type> init, const Compare &comp = Compare(),
      const Alloc &alloc = Alloc())
      : base(comp, alloc) {
    insert(init);
  }
  template <typename It>
  map(It first, It last, const Compare &comp = Compare(),
      const Alloc &alloc = Alloc())
      : base(comp, alloc) {
    insert(first, last);
  }
  map &operator=(std::initializer_list<value_type> init) {
    this->clear();
    insert(init);
    return *this;
  }

  std::pair<iterator, bool> insert(const value_type &v) {
    return this->insert_unique(v.first, v);
  }
  std::pair<iterator, bool> insert(value_type &&v) {
    return this->insert_unique(v.first, std::move(v));
  }
  iterator insert(const_iterator hint, const value_type &v) {
    return this->insert_hint_unique(hint, v.first, v).first;
  }
  iterator insert(const_iterator hint, value_type &&v) {
    return this->insert_hint_unique(hint, v.first, std::move(v)).first;
  }
  template <typename It> void insert(It first, It last) {
    for (; first != last; ++first)
      emplace_hint(this->end(), *first);
  }
  void insert(std::initializer_list<value_type> init) {
    insert(init.begin(), init.end());
  }

  template <typename... Args>
  std::pair<iterator, bool> emplace(Args &&...args) {
    std::pair<Key, T> made(std::forward<Args>(args)...);
    return this->insert_unique(made.first, std::move(made.first),
                               std::move(made.second));
  }
  template <typename... Args>
  iterator emplace_hint(const_iterator hint, Args &&...args) {
    std::pair<Key, T> made(std::forward<Args>(args)...);
    return this
        ->insert_hint_unique(hint, made.first, std::move(made.first),
                             std::move(made.second))
        .first;
  }

  template <typename... Args>
  std::pair<iterator, bool> try_emplace(const Key &k, Args &&...args) {
    return this->insert_unique(
        k, std::piecewise_construct, std::forward_as_tuple(k),
        std::forward_as_tuple(std::forward<Args>(args)...));
  }
  template <typename... Args>
  std::pair<iterator, bool> try_emplace(Key &&k, Args &&...args) {
    return this->insert_unique(
        k, std::piecewise_construct, std::forward_as_tuple(std::move(k)),
        std::forward_as_tuple(std::forward<Args>(args)...));
  }
  template <typename K, typename... Args>
    requires heterogeneous_key<K>
  std::pair<iterator, bool> try_emplace(K &&k, Args &&...args) {
    return this->insert_unique(
        k, std::piecewise_construct, std::forward_as_tuple(std::forward<K>(k)),
        std::forward_as_tuple(std::forward<Args>(args)...));
  }
  template <typename... Args>
  iterator try_emplace(const_iterator hint, const Key &k, Args &&...args) {
    return this
        ->insert_hint_unique(hint, k, std::piecewise_construct,
                             std::forward_as_tuple(k),
                             std::forward_as_tuple(std::forward<Args>(args)...))
        .first;
  }
  template <typename... Args>
  iterator try_emplace(const_iterator hint, Key &&k, Args &&...args) {
    return this
        ->insert_hint_unique(hint, k, std::piecewise_construct,
                             std::forward_as_tuple(std::move(k)),
                             std::forward_as_tuple(std::forward<Args>(args)...))
        .first;
  }
  template <typename K, typename... Args>
    requires heterogeneous_key<K>
  iterator try_emplace(const_iterator hint, K &&k, Args &&...args) {
    return this
        ->insert_hint_unique(hint, k, std::piecewise_construct,
                             std::forward_as_tuple(std::forward<K>(k)),
                             std::forward_as_tuple(std::forward<Args>(args)...))
        .first;
  }

  template <typename M>
  std::pair<iterator, bool> insert_or_assign(const Key &k, M &&obj) {
    return assign_found(try_emplace(k, std::forward<M>(obj)),
                        std::forward<M>(obj));
  }
  template <typename M>
  std::pair<iterator, bool> insert_or_assign(Key &&k, M &&obj) {
    return assign_found(try_emplace(std::move(k), std::forward<M>(obj)),
                        std::forward<M>(obj));
  }
  template <typename K, typename M>
    requires heterogeneous_key<K>
  std::pair<iterator, bool> insert_or_assign(K &&k, M &&obj) {
    return assign_found(try_emplace(std::forward<K>(k), std::forward<M>(obj)),
                        std::forward<M>(obj));
  }
  template <typename M>
  iterator insert_or_assign(const_iterator hint, const Key &k, M &&obj) {
    return assign_found(try_emplace_hinted(hint, k, std::forward<M>(obj)),
                        std::forward<M>(obj))
        .first;
  }
  template <typename M>
  iterator insert_or_assign(const_iterator hint, Key &&k, M &&obj) {
    return assign_found(
               try_emplace_hinted(hint, std::move(k), std::forward<M>(obj)),
               std::forward<M>(obj))
        .first;
  }
  template <typename K, typename M>
    requires heterogeneous_key<K>
  iterator insert_or_assign(const_iterator hint, K &&k, M &&obj) {
    return assign_found(try_emplace_hinted(hint, std::forward<K>(k),
                                           std::forward<M>(obj)),
                        std::forward<M>(obj))
        .first;
  }

  T &operator[](const Key &k) { return try_emplace(k).first->second; }
  T &operator[](Key &&k) { return try_emplace(std::move(k)).first->second; }
  template <typename K>
    requires heterogeneous_key<K>
  T &operator[](K &&k) {
    return try_emplace(std::forward<K>(k)).first->second;
  }

  T &at(const Key &k) { return at_impl(*this, k); }
  const T &at(const Key &k) const { return at_impl(*this, k); }
  template <typename K>
    requires heterogeneous_key<const K &>
  T &at(const K &k) {
    return at_impl(*this, k);
  }
  template <typename K>
    requires heterogeneous_key<const K &>
  const T &at(const K &k) const {
    return at_impl(*this, k);
  }

  friend void swap(map &a, map &b) noexcept { a.swap(b); }

private:
  template <typename Self, typename K>
  static auto &at_impl(Self &self, const K &k) {
    const auto found = self.find(k);
    if (found == self.end())
      throw std::out_of_range("nx::map::at");
    return found->second;
  }

  template <typename K, typename M>
  std::pair<iterator, bool> try_emplace_hinted(const_iterator hint, K &&k,
                                               M &&obj) {
    return this->insert_hint_unique(
        hint, k, std::piecewise_construct,
        std::forward_as_tuple(std::forward<K>(k)),
        std::forward_as_tuple(std::forward<M>(obj)));
  }

  template <typename M>
  static std::pair<iterator, bool> assign_found(std::pair<iterator, bool> at,
                                                M &&obj) {
    if (!at.second)
      at.first->second = std::forward<M>(obj);
    return at;
  }
};

/// An ordered set on a B-tree, with nx::map's invalidation. Elements are
/// keys, so iterators never give mutable access.
template <typename Key, typename Compare = std::less<Key>,
          typename Alloc = Allocator<Key>>
class set : public detail::btree<detail::set_slot_policy<Key>, Compare, Alloc> {
  using base = detail::btree<detail::set_slot_policy<Key>, Compare, Alloc>;
  static_assert(std::is_nothrow_move_constructible_v<Key>,
                "nx::set moves its elements between nodes");

public:
  using typename base::const_iterator;
  using typename base::iterator;
  using typename base::key_type;
  using typename base::size_type;
  using typename base::value_type;

  set() = default;
  explicit set(const Compare &comp, const Alloc &alloc = Alloc())
      : base(comp, alloc) {}
  explicit set(const Alloc &alloc) : base(alloc) {}
  set(std::initializer_list<Key> init, const Compare &comp = Compare(),
      const Alloc &alloc = Alloc())
      : base(comp, alloc) {
    insert(init);
  }
  template <typename It>
  set(It first, It last, const Compare &comp = Compare(),
      const Alloc &alloc = Alloc())
      : base(comp, alloc) {
    insert(first, last);
  }
  set &operator=(std::initializer_list<Key> init) {
    this->clear();
    insert(init);
    return *this;
  }

  std::pair<iterator, bool> insert(const Key &k) {
    return this->insert_unique(k, k);
  }
  std::pair<iterator, bool> insert(Key &&k) {
    return this->insert_unique(k, std::move(k));
  }
  iterator insert(const_iterator hint, const Key &k) {
    return this->insert_hint_unique(hint, k, k).first;
  }
  iterator insert(const_iterator hint, Key &&k) {
    return this->insert_hint_unique(hint, k, std::move(k)).first;
  }
  template <typename It> void insert(It first, It last) {
    for (; first != last; ++first)
      emplace_hint(this->end(), *first);
  }
  void insert(std::initializer_list<Key> init) {
    insert(init.begin(), init.end());
  }

  template <typename... Args>
  std::pair<iterator, bool> emplace(Args &&...args) {
    Key made(std::forward<Args>(args)...);
    return this->insert_unique(made, std::move(made));
  }
  template <typename... Args>
  iterator emplace_hint(const_iterator hint, Args &&...args) {
    Key made(std::forward<Args>(args)...);
    return this->insert_hint_unique(hint, made, std::move(made)).first;
  }

  friend void swap(set &a, set &b) noexcept { a.swap(b); }
};

} // namespace nx
