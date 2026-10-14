#pragma once

#include "core/foundation/containers/slot_policy.h"
#include "core/foundation/core/foundation.h"

#include <algorithm>
#include <cassert>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>

namespace nx {
namespace detail {

template <typename C, typename = void>
struct btree_transparent : std::false_type {};
template <typename C>
struct btree_transparent<C, std::void_t<typename C::is_transparent>>
    : std::true_type {};

/// An ordered B-tree of unique keys, its values in every node, each node
/// about NODE_BYTES. Inserting and erasing move elements between nodes, so
/// they invalidate every iterator, pointer and reference into the tree.
template <typename Policy, typename Compare, typename Alloc> class btree {
protected:
  using slot_type = typename Policy::slot_type;

public:
  using key_type = typename Policy::key_type;
  using value_type = typename Policy::value_type;
  using size_type = usize;
  using difference_type = isize;
  using key_compare = Compare;
  using allocator_type = Alloc;
  using reference = std::conditional_t<Policy::constant_elements,
                                       const value_type &, value_type &>;
  using const_reference = const value_type &;

  static constexpr usize NODE_BYTES = 512;

protected:
  struct leaf_node;
  struct internal_node;

  struct node_header {
    internal_node *parent = nullptr;
    u8 position = 0;
    u8 count = 0;
    bool leaf = true;
  };

public:
  /// Elements a node holds: as many as fit NODE_BYTES, at least three.
  static constexpr u32 SLOTS = static_cast<u32>(std::clamp<usize>(
      (NODE_BYTES - sizeof(node_header)) / sizeof(slot_type), 3, 255));

protected:
  /// Below this a node other than the root borrows or merges after an erase.
  static constexpr u32 MIN_SLOTS = SLOTS / 2;

  struct leaf_node : node_header {
    slot_type slots[SLOTS];
  };
  struct internal_node : leaf_node {
    leaf_node *children[SLOTS + 1];
  };

  using leaf_alloc =
      typename std::allocator_traits<Alloc>::template rebind_alloc<leaf_node>;
  using internal_alloc = typename std::allocator_traits<
      Alloc>::template rebind_alloc<internal_node>;

  static constexpr bool TRANSPARENT = btree_transparent<Compare>::value;

  static leaf_node *&child(leaf_node *n, const u32 i) noexcept {
    return static_cast<internal_node *>(n)->children[i];
  }
  static const key_type &key_at(const leaf_node *n, const u32 i) noexcept {
    return Policy::key_of(Policy::element(n->slots[i]));
  }

public:
  template <bool Const> class iterator_t {
    static constexpr bool CONSTANT = Const || Policy::constant_elements;

  public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = typename Policy::value_type;
    using difference_type = isize;
    using reference =
        std::conditional_t<CONSTANT, const value_type &, value_type &>;
    using pointer =
        std::conditional_t<CONSTANT, const value_type *, value_type *>;

    iterator_t() = default;
    template <bool C = Const, typename = std::enable_if_t<C>>
    iterator_t(const iterator_t<false> &o) noexcept
        : m_node(o.m_node), m_pos(o.m_pos) {}

    reference operator*() const noexcept {
      return Policy::element(m_node->slots[m_pos]);
    }
    pointer operator->() const noexcept { return std::addressof(**this); }

    iterator_t &operator++() noexcept {
      if (!m_node->leaf) {
        m_node = child(m_node, m_pos + 1);
        while (!m_node->leaf)
          m_node = child(m_node, 0);
        m_pos = 0;
        return *this;
      }
      if (++m_pos < m_node->count)
        return *this;
      // Up to the first ancestor this subtree lies left of; past the last
      // element there is none, and the iterator stays where it is, at end.
      leaf_node *n = m_node;
      u32 pos = m_pos;
      while (pos == n->count && n->parent != nullptr) {
        pos = n->position;
        n = n->parent;
      }
      if (pos < n->count) {
        m_node = n;
        m_pos = pos;
      }
      return *this;
    }
    iterator_t operator++(int) noexcept {
      iterator_t old = *this;
      ++*this;
      return old;
    }
    iterator_t &operator--() noexcept {
      if (!m_node->leaf) {
        m_node = child(m_node, m_pos);
        while (!m_node->leaf)
          m_node = child(m_node, m_node->count);
        m_pos = m_node->count - 1u;
        return *this;
      }
      if (m_pos > 0) {
        --m_pos;
        return *this;
      }
      leaf_node *n = m_node;
      while (n->parent != nullptr && n->position == 0)
        n = n->parent;
      m_pos = n->position - 1u;
      m_node = n->parent;
      return *this;
    }
    iterator_t operator--(int) noexcept {
      iterator_t old = *this;
      --*this;
      return old;
    }

    friend bool operator==(const iterator_t &a, const iterator_t &b) noexcept {
      return a.m_node == b.m_node && a.m_pos == b.m_pos;
    }

  private:
    friend class btree;
    template <bool> friend class iterator_t;
    iterator_t(leaf_node *n, const u32 pos) noexcept : m_node(n), m_pos(pos) {}

    leaf_node *m_node = nullptr;
    u32 m_pos = 0;
  };

  using iterator = iterator_t<false>;
  using const_iterator = iterator_t<true>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  btree() = default;
  explicit btree(const Compare &comp, const Alloc &alloc = Alloc())
      : m_comp(comp), m_alloc(alloc) {}
  explicit btree(const Alloc &alloc) : m_alloc(alloc) {}
  btree(const btree &other)
      : m_comp(other.m_comp),
        m_alloc(
            std::allocator_traits<Alloc>::select_on_container_copy_construction(
                other.m_alloc)) {
    copy_from(other);
  }
  btree(btree &&other) noexcept
      : m_root(std::exchange(other.m_root, nullptr)),
        m_leftmost(std::exchange(other.m_leftmost, nullptr)),
        m_rightmost(std::exchange(other.m_rightmost, nullptr)),
        m_size(std::exchange(other.m_size, 0)), m_comp(other.m_comp),
        m_alloc(std::move(other.m_alloc)) {}
  ~btree() { clear(); }

  btree &operator=(const btree &other) {
    if (this != &other) {
      clear();
      m_comp = other.m_comp;
      if constexpr (std::allocator_traits<
                        Alloc>::propagate_on_container_copy_assignment::value)
        m_alloc = other.m_alloc;
      copy_from(other);
    }
    return *this;
  }
  btree &operator=(btree &&other) noexcept {
    if (this != &other) {
      clear();
      swap(other);
    }
    return *this;
  }

  [[nodiscard]] iterator begin() noexcept {
    return m_root == nullptr ? end() : iterator(m_leftmost, 0);
  }
  [[nodiscard]] const_iterator begin() const noexcept {
    return const_cast<btree *>(this)->begin();
  }
  [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
  [[nodiscard]] iterator end() noexcept {
    return m_root == nullptr ? iterator()
                             : iterator(m_rightmost, m_rightmost->count);
  }
  [[nodiscard]] const_iterator end() const noexcept {
    return const_cast<btree *>(this)->end();
  }
  [[nodiscard]] const_iterator cend() const noexcept { return end(); }
  [[nodiscard]] reverse_iterator rbegin() noexcept {
    return reverse_iterator(end());
  }
  [[nodiscard]] const_reverse_iterator rbegin() const noexcept {
    return const_reverse_iterator(end());
  }
  [[nodiscard]] reverse_iterator rend() noexcept {
    return reverse_iterator(begin());
  }
  [[nodiscard]] const_reverse_iterator rend() const noexcept {
    return const_reverse_iterator(begin());
  }

  [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
  [[nodiscard]] size_type size() const noexcept { return m_size; }
  [[nodiscard]] key_compare key_comp() const { return m_comp; }
  [[nodiscard]] allocator_type get_allocator() const { return m_alloc; }

  void clear() noexcept {
    if (m_root != nullptr)
      destroy_subtree(m_root);
    m_root = m_leftmost = m_rightmost = nullptr;
    m_size = 0;
  }

  void swap(btree &other) noexcept {
    using std::swap;
    swap(m_root, other.m_root);
    swap(m_leftmost, other.m_leftmost);
    swap(m_rightmost, other.m_rightmost);
    swap(m_size, other.m_size);
    swap(m_comp, other.m_comp);
    if constexpr (std::allocator_traits<
                      Alloc>::propagate_on_container_swap::value)
      swap(m_alloc, other.m_alloc);
  }

  [[nodiscard]] iterator find(const key_type &key) { return find_of(key); }
  [[nodiscard]] const_iterator find(const key_type &key) const {
    return const_cast<btree *>(this)->find_of(key);
  }
  [[nodiscard]] bool contains(const key_type &key) const {
    return locate(key).second;
  }
  [[nodiscard]] size_type count(const key_type &key) const {
    return contains(key) ? 1 : 0;
  }
  [[nodiscard]] iterator lower_bound(const key_type &key) {
    return lower_of(key);
  }
  [[nodiscard]] const_iterator lower_bound(const key_type &key) const {
    return const_cast<btree *>(this)->lower_of(key);
  }
  [[nodiscard]] iterator upper_bound(const key_type &key) {
    return upper_of(key);
  }
  [[nodiscard]] const_iterator upper_bound(const key_type &key) const {
    return const_cast<btree *>(this)->upper_of(key);
  }
  [[nodiscard]] std::pair<iterator, iterator> equal_range(const key_type &key) {
    return range_of(key);
  }
  [[nodiscard]] std::pair<const_iterator, const_iterator>
  equal_range(const key_type &key) const {
    return const_cast<btree *>(this)->range_of(key);
  }

  // The same for a key of another type, where the comparison takes one.
  template <typename K>
    requires TRANSPARENT
  [[nodiscard]] iterator find(const K &key) {
    return find_of(key);
  }
  template <typename K>
    requires TRANSPARENT
  [[nodiscard]] const_iterator find(const K &key) const {
    return const_cast<btree *>(this)->find_of(key);
  }
  template <typename K>
    requires TRANSPARENT
  [[nodiscard]] bool contains(const K &key) const {
    return locate(key).second;
  }
  template <typename K>
    requires TRANSPARENT
  [[nodiscard]] size_type count(const K &key) const {
    return contains(key) ? 1 : 0;
  }
  template <typename K>
    requires TRANSPARENT
  [[nodiscard]] iterator lower_bound(const K &key) {
    return lower_of(key);
  }
  template <typename K>
    requires TRANSPARENT
  [[nodiscard]] const_iterator lower_bound(const K &key) const {
    return const_cast<btree *>(this)->lower_of(key);
  }
  template <typename K>
    requires TRANSPARENT
  [[nodiscard]] iterator upper_bound(const K &key) {
    return upper_of(key);
  }
  template <typename K>
    requires TRANSPARENT
  [[nodiscard]] const_iterator upper_bound(const K &key) const {
    return const_cast<btree *>(this)->upper_of(key);
  }
  template <typename K>
    requires TRANSPARENT
  [[nodiscard]] std::pair<iterator, iterator> equal_range(const K &key) {
    return range_of(key);
  }
  template <typename K>
    requires TRANSPARENT
  [[nodiscard]] std::pair<const_iterator, const_iterator>
  equal_range(const K &key) const {
    return const_cast<btree *>(this)->range_of(key);
  }

  /// The element after the one erased, or end.
  iterator erase(const_iterator at) noexcept {
    leaf_node *n = at.m_node;
    u32 i = at.m_pos;
    // An element of an internal node is replaced by its predecessor, always
    // last in a leaf, and that leaf loses it instead.
    const bool internal = !n->leaf;
    if (internal) {
      leaf_node *leaf = child(n, i);
      while (!leaf->leaf)
        leaf = child(leaf, leaf->count);
      destroy_slot(&n->slots[i]);
      transfer(&n->slots[i], &leaf->slots[leaf->count - 1u]);
      n = leaf;
      i = --leaf->count;
    } else {
      destroy_slot(&n->slots[i]);
      shift(n, i + 1u, n->count, n, i);
      --n->count;
    }
    --m_size;
    if (m_size == 0) {
      clear();
      return end();
    }
    // Where the erased element was, carried through the rebalance: the
    // element after it, or the end of its leaf when that comes before it.
    iterator next(n, i);
    rebalance(n, next);
    next = normalize(next);
    // After an internal erase that is the predecessor, now where it was.
    if (internal)
      ++next;
    return next;
  }
  iterator erase(iterator at) noexcept { return erase(const_iterator(at)); }
  iterator erase(const_iterator first, const_iterator last) noexcept {
    // Each erase invalidates last, so count it out instead.
    usize n = static_cast<usize>(std::distance(first, last));
    iterator at(first.m_node, first.m_pos);
    while (n-- > 0)
      at = erase(at);
    return at;
  }
  size_type erase(const key_type &key) { return erase_of(key); }
  template <typename K>
    requires(TRANSPARENT && !std::is_convertible_v<K, const_iterator> &&
             !std::is_convertible_v<K, iterator>)
  size_type erase(const K &key) {
    return erase_of(key);
  }

  template <typename Pred> friend size_type erase_if(btree &tree, Pred pred) {
    const size_type before = tree.size();
    for (iterator at = tree.begin(); at != tree.end();)
      at = pred(*at) ? tree.erase(at) : std::next(at);
    return before - tree.size();
  }

  friend bool operator==(const btree &a, const btree &b) {
    if (a.size() != b.size())
      return false;
    auto other = b.begin();
    for (const value_type &v : a) {
      const value_type &w = *other++;
      if (a.m_comp(Policy::key_of(v), Policy::key_of(w)) ||
          a.m_comp(Policy::key_of(w), Policy::key_of(v)) ||
          !Policy::equal_elements(v, w))
        return false;
    }
    return true;
  }

protected:
  template <typename K> [[nodiscard]] iterator find_of(const K &key) {
    const auto [at, found] = locate(key);
    return found ? at : end();
  }
  template <typename K> [[nodiscard]] iterator lower_of(const K &key) {
    const auto [at, found] = locate(key);
    return found ? at : normalize(at);
  }
  template <typename K> [[nodiscard]] iterator upper_of(const K &key) {
    if (m_root == nullptr)
      return end();
    leaf_node *n = m_root;
    for (;;) {
      const u32 i = upper_in(n, key);
      if (n->leaf)
        return normalize(iterator(n, i));
      n = child(n, i);
    }
  }
  template <typename K>
  [[nodiscard]] std::pair<iterator, iterator> range_of(const K &key) {
    const auto [at, found] = locate(key);
    if (!found)
      return {normalize(at), normalize(at)};
    return {at, std::next(at)};
  }
  template <typename K> size_type erase_of(const K &key) {
    const auto [at, found] = locate(key);
    if (!found)
      return 0;
    erase(at);
    return 1;
  }

  /// The element of @p key, or the leaf position it would be inserted at.
  template <typename K>
  [[nodiscard]] std::pair<iterator, bool> locate(const K &key) const {
    if (m_root == nullptr)
      return {iterator(), false};
    leaf_node *n = m_root;
    for (;;) {
      const u32 i = lower_in(n, key);
      if (i < n->count && !m_comp(key, key_at(n, i)))
        return {iterator(n, i), true};
      if (n->leaf)
        return {iterator(n, i), false};
      n = child(n, i);
    }
  }

  template <typename K>
  [[nodiscard]] u32 lower_in(const leaf_node *n, const K &key) const {
    u32 lo = 0;
    u32 hi = n->count;
    while (lo < hi) {
      const u32 mid = (lo + hi) / 2;
      if (m_comp(key_at(n, mid), key))
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  }
  template <typename K>
  [[nodiscard]] u32 upper_in(const leaf_node *n, const K &key) const {
    u32 lo = 0;
    u32 hi = n->count;
    while (lo < hi) {
      const u32 mid = (lo + hi) / 2;
      if (m_comp(key, key_at(n, mid)))
        hi = mid;
      else
        lo = mid + 1;
    }
    return lo;
  }

  /// @p at, a position before an element or at the end of a node, as the
  /// element it comes before, or end.
  [[nodiscard]] iterator normalize(iterator at) noexcept {
    if (at.m_node == nullptr)
      return end();
    leaf_node *n = at.m_node;
    u32 pos = at.m_pos;
    while (pos == n->count && n->parent != nullptr) {
      pos = n->position;
      n = n->parent;
    }
    return pos == n->count ? end() : iterator(n, pos);
  }

  template <typename K, typename... Args>
  std::pair<iterator, bool> insert_unique(const K &key, Args &&...args) {
    if (m_root == nullptr) {
      m_root = m_leftmost = m_rightmost = new_leaf();
      return {emplace_at(m_root, 0, std::forward<Args>(args)...), true};
    }
    const auto [at, found] = locate(key);
    if (found)
      return {at, false};
    return {emplace_at(at.m_node, at.m_pos, std::forward<Args>(args)...), true};
  }

  /// Inserts before @p hint when @p key belongs there, otherwise where it
  /// belongs; a run inserted in order at the end costs no search.
  template <typename K, typename... Args>
  std::pair<iterator, bool> insert_hint_unique(const_iterator hint,
                                               const K &key, Args &&...args) {
    if (m_root != nullptr) {
      if (hint == end() || m_comp(key, Policy::key_of(*hint))) {
        if (hint == begin() || m_comp(Policy::key_of(*std::prev(hint)), key)) {
          leaf_node *n = hint.m_node;
          u32 pos = hint.m_pos;
          if (!n->leaf) {
            n = child(n, pos);
            while (!n->leaf)
              n = child(n, n->count);
            pos = n->count;
          }
          return {emplace_at(n, pos, std::forward<Args>(args)...), true};
        }
      } else if (!m_comp(Policy::key_of(*hint), key)) {
        return {iterator(hint.m_node, hint.m_pos), false};
      }
    }
    return insert_unique(key, std::forward<Args>(args)...);
  }

  [[nodiscard]] slot_type *slot_of(const const_iterator at) const noexcept {
    return &at.m_node->slots[at.m_pos];
  }

private:
  template <typename... Args>
  iterator emplace_at(leaf_node *n, u32 i, Args &&...args) {
    // Made aside first, so that a throwing constructor leaves the tree as
    // it was, and a throwing allocation leaves it whole.
    slot_type made;
    Policy::construct(m_alloc, &made, std::forward<Args>(args)...);
    try {
      make_room(n, i);
    } catch (...) {
      destroy_slot(&made);
      throw;
    }
    shift(n, i, n->count, n, i + 1u);
    transfer(&n->slots[i], &made);
    ++n->count;
    ++m_size;
    return iterator(n, i);
  }

  /// Splits @p n if it is full, its parent first, so that an element (and
  /// in an internal node, the child after it) fits at @p i, moving @p n and
  /// @p i to where that now is.
  void make_room(leaf_node *&n, u32 &i) {
    if (n->count < SLOTS)
      return;
    leaf_node *dest = n->leaf ? new_leaf() : new_internal();
    try {
      if (n == m_root) {
        internal_node *root = new_internal();
        set_child(root, 0, n);
        m_root = root;
      }
    } catch (...) {
      free_node(dest);
      throw;
    }
    leaf_node *parent = n->parent;
    u32 at = n->position;
    try {
      make_room(parent, at);
    } catch (...) {
      free_node(dest);
      throw;
    }
    // Appending keeps the left node full, so a run inserted in order fills
    // its nodes; otherwise both halves are kept about equal.
    const u32 keep = i == SLOTS ? SLOTS - 1u : SLOTS / 2u;
    dest->count = static_cast<u8>(SLOTS - keep - 1u);
    shift(n, keep + 1u, SLOTS, dest, 0);
    if (!n->leaf)
      for (u32 c = 0; c <= dest->count; ++c)
        set_child(dest, c, child(n, keep + 1u + c));
    shift(parent, at, parent->count, parent, at + 1u);
    for (u32 c = parent->count + 1u; c > at + 1u; --c)
      set_child(parent, c, child(parent, c - 1u));
    transfer(&parent->slots[at], &n->slots[keep]);
    set_child(parent, at + 1u, dest);
    ++parent->count;
    n->count = static_cast<u8>(keep);
    if (n == m_rightmost)
      m_rightmost = dest;
    if (i > keep) {
      n = dest;
      i -= keep + 1u;
    }
  }

  /// Merges or refills @p n, short of elements after an erase, and its
  /// ancestors as merges empty them, carrying @p track, a position in the
  /// leaf erased from, along: only merging that leaf into its left sibling
  /// and borrowing from that sibling move it.
  void rebalance(leaf_node *n, iterator &track) noexcept {
    for (;;) {
      if (n == m_root) {
        if (n->count == 0 && !n->leaf) {
          m_root = child(n, 0);
          m_root->parent = nullptr;
          m_root->position = 0;
          free_node(n);
        }
        return;
      }
      if (n->count >= MIN_SLOTS)
        return;
      leaf_node *parent = n->parent;
      const u32 at = n->position;
      if (at > 0 && child(parent, at - 1u)->count + n->count + 1u <= SLOTS) {
        merge(parent, at - 1u, track);
      } else if (at < parent->count &&
                 n->count + child(parent, at + 1u)->count + 1u <= SLOTS) {
        merge(parent, at, track);
      } else {
        // No merge fits, so a sibling holds more than it needs.
        if (at < parent->count)
          borrow_right(parent, at);
        else
          borrow_left(parent, at, track);
        return;
      }
      n = parent;
    }
  }

  /// Child at + 1 of @p parent appended to child @p at, after their
  /// separator.
  void merge(leaf_node *parent, const u32 at, iterator &track) noexcept {
    leaf_node *left = child(parent, at);
    leaf_node *right = child(parent, at + 1u);
    const u32 base = left->count;
    transfer(&left->slots[base], &parent->slots[at]);
    shift(right, 0, right->count, left, base + 1u);
    if (!left->leaf)
      for (u32 c = 0; c <= right->count; ++c)
        set_child(left, base + 1u + c, child(right, c));
    left->count = static_cast<u8>(base + 1u + right->count);
    shift(parent, at + 1u, parent->count, parent, at);
    for (u32 c = at + 1u; c < parent->count; ++c)
      set_child(parent, c, child(parent, c + 1u));
    --parent->count;
    if (track.m_node == right)
      track = iterator(left, base + 1u + track.m_pos);
    if (right == m_rightmost)
      m_rightmost = left;
    free_node(right);
  }

  void borrow_right(leaf_node *parent, const u32 at) noexcept {
    leaf_node *n = child(parent, at);
    leaf_node *right = child(parent, at + 1u);
    const u32 moved = (right->count - n->count + 1u) / 2u;
    const u32 base = n->count;
    transfer(&n->slots[base], &parent->slots[at]);
    shift(right, 0, moved - 1u, n, base + 1u);
    transfer(&parent->slots[at], &right->slots[moved - 1u]);
    shift(right, moved, right->count, right, 0);
    if (!n->leaf) {
      for (u32 c = 0; c < moved; ++c)
        set_child(n, base + 1u + c, child(right, c));
      for (u32 c = 0; c + moved <= right->count; ++c)
        set_child(right, c, child(right, c + moved));
    }
    n->count = static_cast<u8>(base + moved);
    right->count = static_cast<u8>(right->count - moved);
  }

  void borrow_left(leaf_node *parent, const u32 at, iterator &track) noexcept {
    leaf_node *left = child(parent, at - 1u);
    leaf_node *n = child(parent, at);
    const u32 moved = (left->count - n->count + 1u) / 2u;
    const u32 from = left->count - moved;
    shift(n, 0, n->count, n, moved);
    transfer(&n->slots[moved - 1u], &parent->slots[at - 1u]);
    shift(left, from + 1u, left->count, n, 0);
    transfer(&parent->slots[at - 1u], &left->slots[from]);
    if (!n->leaf) {
      for (u32 c = n->count + 1u; c-- > 0;)
        set_child(n, c + moved, child(n, c));
      for (u32 c = 0; c < moved; ++c)
        set_child(n, c, child(left, from + 1u + c));
    }
    n->count = static_cast<u8>(n->count + moved);
    left->count = static_cast<u8>(from);
    if (track.m_node == n)
      track.m_pos += moved;
  }

  /// Moves elements [@p first, @p last) of @p from to @p to at @p dest,
  /// which may overlap them.
  void shift(leaf_node *from, const u32 first, const u32 last, leaf_node *to,
             const u32 dest) noexcept {
    if (from == to && dest > first) {
      for (u32 k = last; k-- > first;)
        transfer(&to->slots[dest + (k - first)], &from->slots[k]);
    } else {
      for (u32 k = first; k < last; ++k)
        transfer(&to->slots[dest + (k - first)], &from->slots[k]);
    }
  }

  static void set_child(leaf_node *n, const u32 i, leaf_node *c) noexcept {
    child(n, i) = c;
    c->parent = static_cast<internal_node *>(n);
    c->position = static_cast<u8>(i);
  }

  void transfer(slot_type *to, slot_type *from) noexcept {
    Policy::move_construct(m_alloc, to, from);
    Policy::destroy(m_alloc, from);
  }
  void destroy_slot(slot_type *s) noexcept { Policy::destroy(m_alloc, s); }

  [[nodiscard]] leaf_node *new_leaf() {
    leaf_alloc alloc(m_alloc);
    leaf_node *n = std::allocator_traits<leaf_alloc>::allocate(alloc, 1);
    return ::new (static_cast<void *>(n)) leaf_node{};
  }
  [[nodiscard]] internal_node *new_internal() {
    internal_alloc alloc(m_alloc);
    internal_node *n =
        std::allocator_traits<internal_alloc>::allocate(alloc, 1);
    ::new (static_cast<void *>(n)) internal_node{};
    n->leaf = false;
    return n;
  }
  void free_node(leaf_node *n) noexcept {
    if (n->leaf) {
      leaf_alloc alloc(m_alloc);
      std::allocator_traits<leaf_alloc>::deallocate(alloc, n, 1);
    } else {
      internal_alloc alloc(m_alloc);
      std::allocator_traits<internal_alloc>::deallocate(
          alloc, static_cast<internal_node *>(n), 1);
    }
  }
  void destroy_subtree(leaf_node *n) noexcept {
    for (u32 i = 0; i < n->count; ++i)
      destroy_slot(&n->slots[i]);
    if (!n->leaf)
      for (u32 c = 0; c <= n->count; ++c)
        destroy_subtree(child(n, c));
    free_node(n);
  }

  void copy_from(const btree &other) {
    for (const value_type &v : other)
      insert_hint_unique(end(), Policy::key_of(v), v);
  }

protected:
  leaf_node *m_root = nullptr;
  leaf_node *m_leftmost = nullptr;
  leaf_node *m_rightmost = nullptr;
  usize m_size = 0;
  NX_NO_UNIQUE_ADDRESS Compare m_comp{};
  NX_NO_UNIQUE_ADDRESS Alloc m_alloc{};
};

} // namespace detail
} // namespace nx
