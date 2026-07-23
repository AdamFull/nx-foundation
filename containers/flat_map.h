#pragma once

/**
 * @file flat_map.h
 * @brief Sorted associative container over contiguous arrays (nx::flat_map).
 */

#include <algorithm>
#include <compare>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "foundation/core/foundation.h"

#include <cassert>

namespace nx {

struct sorted_unique_t {
  explicit sorted_unique_t() = default;
};
inline constexpr sorted_unique_t sorted_unique{};

namespace detail {

template <typename C, typename = void>
struct fm_has_is_transparent : std::false_type {};
template <typename C>
struct fm_has_is_transparent<C, std::void_t<typename C::is_transparent>>
    : std::true_type {};

// operator-> needs to hand back something pointer-like, but the iterator's
// reference is a pair of references that lives nowhere stable. Wrapping it lets
// the arrow chain reach a real address for the duration of the full expression.
template <typename Ref> struct fm_arrow_proxy {
  Ref ref;
  Ref *operator->() noexcept { return std::addressof(ref); }
};

} // namespace detail

/**
 * @brief Ordered key/value map backed by sorted contiguous arrays.
 *
 * Keys and mapped values live in two parallel dense arrays. Lookups are binary
 * searches over the keys, so they are cache-friendly and iteration is ordered.
 * Insert and erase are O(n) because elements shift, making this best for
 * read-heavy maps built once and queried often rather than churn-heavy
 * workloads.
 *
 * @tparam Key     Key type.
 * @tparam T       Mapped value type.
 * @tparam Compare Strict-weak ordering on keys.
 * @tparam Alloc   Allocator type.
 */
template <
    typename Key, typename T, typename Compare = std::less<Key>,
    typename Alloc = Allocator<Key>, typename KeyContainer = vector<Key, Alloc>,
    typename MappedContainer = vector<
        T, typename std::allocator_traits<Alloc>::template rebind_alloc<T>>>
class flat_map {
  static_assert(std::is_same_v<Key, typename KeyContainer::value_type>,
                "flat_map: KeyContainer::value_type must be Key");
  static_assert(std::is_same_v<T, typename MappedContainer::value_type>,
                "flat_map: MappedContainer::value_type must be T");

  static constexpr bool kTransparent =
      detail::fm_has_is_transparent<Compare>::value;

  template <typename A>
  static constexpr bool uses_alloc_v =
      std::uses_allocator_v<KeyContainer, A> &&
      std::uses_allocator_v<MappedContainer, A>;

public:
  using key_type = Key;
  using mapped_type = T;
  using value_type = std::pair<Key, T>;
  using key_compare = Compare;
  using allocator_type = Alloc;
  using reference = std::pair<const Key &, T &>;
  using const_reference = std::pair<const Key &, const T &>;
  using size_type = usize;
  using difference_type = isize;
  using key_container_type = KeyContainer;
  using mapped_container_type = MappedContainer;

  class value_compare {
    friend class flat_map;

  protected:
    key_compare comp_;
    explicit value_compare(key_compare c) : comp_(std::move(c)) {}

  public:
    template <typename L, typename R>
    [[nodiscard]] bool operator()(const L &lhs, const R &rhs) const {
      return comp_(lhs.first, rhs.first);
    }
  };

  struct containers {
    key_container_type keys;
    mapped_container_type values;
  };

  template <bool Const> class iter_t {
    friend class flat_map;
    template <bool> friend class iter_t;

    using key_it = typename KeyContainer::const_iterator;
    using mapped_it =
        std::conditional_t<Const, typename MappedContainer::const_iterator,
                           typename MappedContainer::iterator>;

    key_it ki_{};
    mapped_it mi_{};

    iter_t(key_it k, mapped_it m) noexcept : ki_(k), mi_(m) {}

  public:
    using iterator_category = std::random_access_iterator_tag;
    using iterator_concept = std::random_access_iterator_tag;
    using value_type = flat_map::value_type;
    using difference_type = isize;
    using reference = std::conditional_t<Const, flat_map::const_reference,
                                         flat_map::reference>;
    using pointer = detail::fm_arrow_proxy<reference>;

    iter_t() = default;

    template <bool C = Const, std::enable_if_t<C, i32> = 0>
    iter_t(const iter_t<false> &o) noexcept : ki_(o.ki_), mi_(o.mi_) {}

    [[nodiscard]] reference operator*() const { return reference{*ki_, *mi_}; }
    [[nodiscard]] pointer operator->() const {
      return pointer{reference{*ki_, *mi_}};
    }
    [[nodiscard]] reference operator[](difference_type n) const {
      return reference{ki_[n], mi_[n]};
    }

    iter_t &operator++() noexcept {
      ++ki_;
      ++mi_;
      return *this;
    }
    iter_t operator++(i32) noexcept {
      iter_t tmp = *this;
      ++*this;
      return tmp;
    }
    iter_t &operator--() noexcept {
      --ki_;
      --mi_;
      return *this;
    }
    iter_t operator--(i32) noexcept {
      iter_t tmp = *this;
      --*this;
      return tmp;
    }
    iter_t &operator+=(difference_type n) noexcept {
      ki_ += n;
      mi_ += n;
      return *this;
    }
    iter_t &operator-=(difference_type n) noexcept {
      ki_ -= n;
      mi_ -= n;
      return *this;
    }

    [[nodiscard]] friend iter_t operator+(iter_t it,
                                          difference_type n) noexcept {
      it += n;
      return it;
    }
    [[nodiscard]] friend iter_t operator+(difference_type n,
                                          iter_t it) noexcept {
      it += n;
      return it;
    }
    [[nodiscard]] friend iter_t operator-(iter_t it,
                                          difference_type n) noexcept {
      it -= n;
      return it;
    }
    [[nodiscard]] friend difference_type operator-(const iter_t &a,
                                                   const iter_t &b) noexcept {
      return a.ki_ - b.ki_;
    }

    [[nodiscard]] friend bool operator==(const iter_t &a,
                                         const iter_t &b) noexcept {
      return a.ki_ == b.ki_;
    }
    [[nodiscard]] friend std::strong_ordering
    operator<=>(const iter_t &a, const iter_t &b) noexcept {
      return (a.ki_ - b.ki_) <=> difference_type{0};
    }
  };

  using iterator = iter_t<false>;
  using const_iterator = iter_t<true>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  flat_map() = default;

  explicit flat_map(const key_compare &comp) : comp_(comp) {}

  flat_map(key_container_type keys, mapped_container_type values,
           const key_compare &comp = key_compare())
      : c_{std::move(keys), std::move(values)}, comp_(comp) {
    assert(c_.keys.size() == c_.values.size());
    sort_and_unique();
  }

  flat_map(sorted_unique_t, key_container_type keys,
           mapped_container_type values,
           const key_compare &comp = key_compare())
      : c_{std::move(keys), std::move(values)}, comp_(comp) {
    assert(c_.keys.size() == c_.values.size());
    assert(is_sorted_unique());
  }

  template <std::input_iterator It>
  flat_map(It first, It last, const key_compare &comp = key_compare())
      : comp_(comp) {
    insert(first, last);
  }

  template <std::input_iterator It>
  flat_map(sorted_unique_t, It first, It last,
           const key_compare &comp = key_compare())
      : comp_(comp) {
    insert(sorted_unique, first, last);
  }

  flat_map(std::initializer_list<value_type> il,
           const key_compare &comp = key_compare())
      : comp_(comp) {
    insert(il.begin(), il.end());
  }

  flat_map(sorted_unique_t, std::initializer_list<value_type> il,
           const key_compare &comp = key_compare())
      : comp_(comp) {
    insert(sorted_unique, il.begin(), il.end());
  }

  template <typename Alloc2, std::enable_if_t<uses_alloc_v<Alloc2>, i32> = 0>
  explicit flat_map(const Alloc2 &a)
      : c_{std::make_obj_using_allocator<key_container_type>(a),
           std::make_obj_using_allocator<mapped_container_type>(a)} {}

  template <typename Alloc2, std::enable_if_t<uses_alloc_v<Alloc2>, i32> = 0>
  flat_map(const key_compare &comp, const Alloc2 &a)
      : c_{std::make_obj_using_allocator<key_container_type>(a),
           std::make_obj_using_allocator<mapped_container_type>(a)},
        comp_(comp) {}

  template <typename Alloc2, std::enable_if_t<uses_alloc_v<Alloc2>, i32> = 0>
  flat_map(const flat_map &o, const Alloc2 &a)
      : c_{std::make_obj_using_allocator<key_container_type>(a, o.c_.keys),
           std::make_obj_using_allocator<mapped_container_type>(a,
                                                                o.c_.values)},
        comp_(o.comp_) {}

  template <typename Alloc2, std::enable_if_t<uses_alloc_v<Alloc2>, i32> = 0>
  flat_map(flat_map &&o, const Alloc2 &a)
      : c_{std::make_obj_using_allocator<key_container_type>(
               a, std::move(o.c_.keys)),
           std::make_obj_using_allocator<mapped_container_type>(
               a, std::move(o.c_.values))},
        comp_(std::move(o.comp_)) {}

  flat_map(const flat_map &) = default;
  flat_map(flat_map &&) = default;
  flat_map &operator=(const flat_map &) = default;
  flat_map &operator=(flat_map &&) = default;

  flat_map &operator=(std::initializer_list<value_type> il) {
    clear();
    insert(il.begin(), il.end());
    return *this;
  }

  [[nodiscard]] iterator begin() noexcept {
    return iterator{c_.keys.begin(), c_.values.begin()};
  }
  [[nodiscard]] const_iterator begin() const noexcept {
    return const_iterator{c_.keys.begin(), c_.values.begin()};
  }
  [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
  [[nodiscard]] iterator end() noexcept {
    return iterator{c_.keys.end(), c_.values.end()};
  }
  [[nodiscard]] const_iterator end() const noexcept {
    return const_iterator{c_.keys.end(), c_.values.end()};
  }
  [[nodiscard]] const_iterator cend() const noexcept { return end(); }
  [[nodiscard]] reverse_iterator rbegin() noexcept {
    return reverse_iterator{end()};
  }
  [[nodiscard]] const_reverse_iterator rbegin() const noexcept {
    return const_reverse_iterator{end()};
  }
  [[nodiscard]] const_reverse_iterator crbegin() const noexcept {
    return const_reverse_iterator{cend()};
  }
  [[nodiscard]] reverse_iterator rend() noexcept {
    return reverse_iterator{begin()};
  }
  [[nodiscard]] const_reverse_iterator rend() const noexcept {
    return const_reverse_iterator{begin()};
  }
  [[nodiscard]] const_reverse_iterator crend() const noexcept {
    return const_reverse_iterator{cbegin()};
  }

  [[nodiscard]] bool empty() const noexcept { return c_.keys.empty(); }
  [[nodiscard]] size_type size() const noexcept { return c_.keys.size(); }
  [[nodiscard]] size_type max_size() const noexcept {
    return min<size_type>(c_.keys.max_size(), c_.values.max_size());
  }
  [[nodiscard]] size_type capacity() const noexcept {
    return min<size_type>(c_.keys.capacity(), c_.values.capacity());
  }

  void reserve(size_type n) {
    c_.keys.reserve(n);
    c_.values.reserve(n);
  }
  void shrink_to_fit() {
    c_.keys.shrink_to_fit();
    c_.values.shrink_to_fit();
  }

  void clear() noexcept {
    c_.keys.clear();
    c_.values.clear();
  }

  [[nodiscard]] T &operator[](const Key &k) {
    return try_emplace(k).first->second;
  }
  [[nodiscard]] T &operator[](Key &&k) {
    return try_emplace(std::move(k)).first->second;
  }

  [[nodiscard]] T &at(const Key &k) {
    const size_type i = find_index(k);
    if (i == size())
      throw std::out_of_range("flat_map::at");
    return c_.values[i];
  }
  [[nodiscard]] const T &at(const Key &k) const {
    const size_type i = find_index(k);
    if (i == size())
      throw std::out_of_range("flat_map::at");
    return c_.values[i];
  }

  template <typename... Args>
  std::pair<iterator, bool> try_emplace(const Key &k, Args &&...args) {
    const size_type i = lower_bound_index(k);
    if (i < size() && key_equiv(k, c_.keys[i]))
      return {iterator_at(i), false};
    return {insert_at(i, k, std::forward<Args>(args)...), true};
  }
  template <typename... Args>
  std::pair<iterator, bool> try_emplace(Key &&k, Args &&...args) {
    const size_type i = lower_bound_index(k);
    if (i < size() && key_equiv(k, c_.keys[i]))
      return {iterator_at(i), false};
    return {insert_at(i, std::move(k), std::forward<Args>(args)...), true};
  }
  template <typename... Args>
  iterator try_emplace(const_iterator, const Key &k, Args &&...args) {
    return try_emplace(k, std::forward<Args>(args)...).first;
  }
  template <typename... Args>
  iterator try_emplace(const_iterator, Key &&k, Args &&...args) {
    return try_emplace(std::move(k), std::forward<Args>(args)...).first;
  }

  template <typename M>
  std::pair<iterator, bool> insert_or_assign(const Key &k, M &&obj) {
    const size_type i = lower_bound_index(k);
    if (i < size() && key_equiv(k, c_.keys[i])) {
      c_.values[i] = std::forward<M>(obj);
      return {iterator_at(i), false};
    }
    return {insert_at(i, k, std::forward<M>(obj)), true};
  }
  template <typename M>
  std::pair<iterator, bool> insert_or_assign(Key &&k, M &&obj) {
    const size_type i = lower_bound_index(k);
    if (i < size() && key_equiv(k, c_.keys[i])) {
      c_.values[i] = std::forward<M>(obj);
      return {iterator_at(i), false};
    }
    return {insert_at(i, std::move(k), std::forward<M>(obj)), true};
  }
  template <typename M>
  iterator insert_or_assign(const_iterator, const Key &k, M &&obj) {
    return insert_or_assign(k, std::forward<M>(obj)).first;
  }
  template <typename M>
  iterator insert_or_assign(const_iterator, Key &&k, M &&obj) {
    return insert_or_assign(std::move(k), std::forward<M>(obj)).first;
  }

  template <typename... Args>
  std::pair<iterator, bool> emplace(Args &&...args) {
    value_type v(std::forward<Args>(args)...);
    return try_emplace(std::move(v.first), std::move(v.second));
  }
  template <typename... Args>
  iterator emplace_hint(const_iterator, Args &&...args) {
    return emplace(std::forward<Args>(args)...).first;
  }

  std::pair<iterator, bool> insert(const value_type &v) {
    return try_emplace(v.first, v.second);
  }
  std::pair<iterator, bool> insert(value_type &&v) {
    return try_emplace(std::move(v.first), std::move(v.second));
  }
  template <typename P,
            std::enable_if_t<std::is_constructible_v<value_type, P &&> &&
                                 !std::is_same_v<std::decay_t<P>, value_type>,
                             i32> = 0>
  std::pair<iterator, bool> insert(P &&v) {
    return emplace(std::forward<P>(v));
  }
  iterator insert(const_iterator, const value_type &v) {
    return insert(v).first;
  }
  iterator insert(const_iterator, value_type &&v) {
    return insert(std::move(v)).first;
  }

  template <std::input_iterator It> void insert(It first, It last) {
    const size_type before = size();
    clear_guard guard{this};
    for (; first != last; ++first) {
      value_type v(*first);
      push_back_pair(std::move(v.first), std::move(v.second));
    }
    sort_and_unique_keep_first(before);
    guard.disarm();
  }

  template <std::input_iterator It>
  void insert(sorted_unique_t, It first, It last) {
    insert(first, last);
  }

  void insert(std::initializer_list<value_type> il) {
    insert(il.begin(), il.end());
  }
  void insert(sorted_unique_t, std::initializer_list<value_type> il) {
    insert(sorted_unique, il.begin(), il.end());
  }

  [[nodiscard]] iterator find(const Key &k) {
    const size_type i = find_index(k);
    return i == size() ? end() : iterator_at(i);
  }
  [[nodiscard]] const_iterator find(const Key &k) const {
    const size_type i = find_index(k);
    return i == size() ? end() : iterator_at(i);
  }
  template <typename K, bool E = kTransparent, std::enable_if_t<E, i32> = 0>
  [[nodiscard]] iterator find(const K &k) {
    const size_type i = find_index(k);
    return i == size() ? end() : iterator_at(i);
  }
  template <typename K, bool E = kTransparent, std::enable_if_t<E, i32> = 0>
  [[nodiscard]] const_iterator find(const K &k) const {
    const size_type i = find_index(k);
    return i == size() ? end() : iterator_at(i);
  }

  [[nodiscard]] size_type count(const Key &k) const {
    return find_index(k) == size() ? 0 : 1;
  }
  template <typename K, bool E = kTransparent, std::enable_if_t<E, i32> = 0>
  [[nodiscard]] size_type count(const K &k) const {
    return find_index(k) == size() ? 0 : 1;
  }

  [[nodiscard]] bool contains(const Key &k) const {
    return find_index(k) != size();
  }
  template <typename K, bool E = kTransparent, std::enable_if_t<E, i32> = 0>
  [[nodiscard]] bool contains(const K &k) const {
    return find_index(k) != size();
  }

  [[nodiscard]] iterator lower_bound(const Key &k) {
    return iterator_at(lower_bound_index(k));
  }
  [[nodiscard]] const_iterator lower_bound(const Key &k) const {
    return iterator_at(lower_bound_index(k));
  }
  template <typename K, bool E = kTransparent, std::enable_if_t<E, i32> = 0>
  [[nodiscard]] iterator lower_bound(const K &k) {
    return iterator_at(lower_bound_index(k));
  }
  template <typename K, bool E = kTransparent, std::enable_if_t<E, i32> = 0>
  [[nodiscard]] const_iterator lower_bound(const K &k) const {
    return iterator_at(lower_bound_index(k));
  }

  [[nodiscard]] iterator upper_bound(const Key &k) {
    return iterator_at(upper_bound_index(k));
  }
  [[nodiscard]] const_iterator upper_bound(const Key &k) const {
    return iterator_at(upper_bound_index(k));
  }
  template <typename K, bool E = kTransparent, std::enable_if_t<E, i32> = 0>
  [[nodiscard]] iterator upper_bound(const K &k) {
    return iterator_at(upper_bound_index(k));
  }
  template <typename K, bool E = kTransparent, std::enable_if_t<E, i32> = 0>
  [[nodiscard]] const_iterator upper_bound(const K &k) const {
    return iterator_at(upper_bound_index(k));
  }

  [[nodiscard]] std::pair<iterator, iterator> equal_range(const Key &k) {
    return equal_range_impl(*this, k);
  }
  [[nodiscard]] std::pair<const_iterator, const_iterator>
  equal_range(const Key &k) const {
    return equal_range_impl(*this, k);
  }
  template <typename K, bool E = kTransparent, std::enable_if_t<E, i32> = 0>
  [[nodiscard]] std::pair<iterator, iterator> equal_range(const K &k) {
    return equal_range_impl(*this, k);
  }
  template <typename K, bool E = kTransparent, std::enable_if_t<E, i32> = 0>
  [[nodiscard]] std::pair<const_iterator, const_iterator>
  equal_range(const K &k) const {
    return equal_range_impl(*this, k);
  }

  iterator erase(const_iterator pos) {
    const size_type i = index_of(pos);
    c_.keys.erase(c_.keys.begin() + i);
    c_.values.erase(c_.values.begin() + i);
    return iterator_at(i);
  }
  iterator erase(iterator pos) { return erase(const_iterator(pos)); }

  iterator erase(const_iterator first, const_iterator last) {
    const size_type i = index_of(first);
    const size_type j = index_of(last);
    c_.keys.erase(c_.keys.begin() + i, c_.keys.begin() + j);
    c_.values.erase(c_.values.begin() + i, c_.values.begin() + j);
    return iterator_at(i);
  }

  size_type erase(const Key &k) {
    const size_type i = find_index(k);
    if (i == size())
      return 0;
    c_.keys.erase(c_.keys.begin() + i);
    c_.values.erase(c_.values.begin() + i);
    return 1;
  }
  template <typename K, bool E = kTransparent,
            std::enable_if_t<E && !std::is_convertible_v<K, iterator> &&
                                 !std::is_convertible_v<K, const_iterator>,
                             i32> = 0>
  size_type erase(K &&k) {
    const size_type i = find_index(k);
    if (i == size())
      return 0;
    c_.keys.erase(c_.keys.begin() + i);
    c_.values.erase(c_.values.begin() + i);
    return 1;
  }

  [[nodiscard]] containers extract() && {
    containers out = std::move(c_);
    c_.keys.clear();
    c_.values.clear();
    return out;
  }

  void replace(key_container_type &&keys, mapped_container_type &&values) {
    assert(keys.size() == values.size());
    c_.keys = std::move(keys);
    c_.values = std::move(values);
    assert(is_sorted_unique());
  }

  [[nodiscard]] const key_container_type &keys() const noexcept {
    return c_.keys;
  }
  [[nodiscard]] const mapped_container_type &values() const noexcept {
    return c_.values;
  }

  [[nodiscard]] key_compare key_comp() const { return comp_; }
  [[nodiscard]] value_compare value_comp() const {
    return value_compare{comp_};
  }

  void swap(flat_map &o) noexcept(
      std::is_nothrow_swappable_v<key_container_type> &&
      std::is_nothrow_swappable_v<mapped_container_type> &&
      std::is_nothrow_swappable_v<key_compare>) {
    using std::swap;
    swap(c_.keys, o.c_.keys);
    swap(c_.values, o.c_.values);
    swap(comp_, o.comp_);
  }

  friend void swap(flat_map &a, flat_map &b) noexcept(noexcept(a.swap(b))) {
    a.swap(b);
  }

  [[nodiscard]] friend bool operator==(const flat_map &a, const flat_map &b) {
    return a.c_.keys.size() == b.c_.keys.size() &&
           std::equal(a.c_.keys.begin(), a.c_.keys.end(), b.c_.keys.begin()) &&
           std::equal(a.c_.values.begin(), a.c_.values.end(),
                      b.c_.values.begin());
  }

  [[nodiscard]] friend auto operator<=>(const flat_map &a, const flat_map &b)
    requires std::three_way_comparable<Key> && std::three_way_comparable<T>
  {
    const size_type n = min(a.size(), b.size());
    for (size_type i = 0; i < n; ++i) {
      if (auto c = a.c_.keys[i] <=> b.c_.keys[i]; c != 0)
        return c;
      if (auto c = a.c_.values[i] <=> b.c_.values[i]; c != 0)
        return c;
    }
    return a.size() <=> b.size();
  }

private:
  struct clear_guard {
    flat_map *map;
    bool active = true;
    void disarm() noexcept { active = false; }
    ~clear_guard() {
      if (active)
        map->clear();
    }
  };

  [[nodiscard]] iterator iterator_at(size_type i) noexcept {
    return iterator{c_.keys.begin() + i, c_.values.begin() + i};
  }
  [[nodiscard]] const_iterator iterator_at(size_type i) const noexcept {
    return const_iterator{c_.keys.begin() + i, c_.values.begin() + i};
  }

  [[nodiscard]] size_type index_of(const_iterator it) const noexcept {
    return static_cast<size_type>(it.ki_ - c_.keys.cbegin());
  }

  template <typename K>
  [[nodiscard]] size_type lower_bound_index(const K &k) const {
    return static_cast<size_type>(
        std::lower_bound(c_.keys.begin(), c_.keys.end(), k, comp_) -
        c_.keys.begin());
  }
  template <typename K>
  [[nodiscard]] size_type upper_bound_index(const K &k) const {
    return static_cast<size_type>(
        std::upper_bound(c_.keys.begin(), c_.keys.end(), k, comp_) -
        c_.keys.begin());
  }

  template <typename K1, typename K2>
  [[nodiscard]] bool key_equiv(const K1 &a, const K2 &b) const {
    return !comp_(a, b) && !comp_(b, a);
  }

  template <typename K> [[nodiscard]] size_type find_index(const K &k) const {
    const size_type i = lower_bound_index(k);
    if (i < size() && !comp_(k, c_.keys[i]))
      return i;
    return size();
  }

  template <typename Self, typename K>
  static auto equal_range_impl(Self &self, const K &k) {
    const size_type lo = self.lower_bound_index(k);
    const size_type hi =
        (lo < self.size() && !self.comp_(k, self.c_.keys[lo])) ? lo + 1 : lo;
    return std::pair{self.iterator_at(lo), self.iterator_at(hi)};
  }

  template <typename KArg, typename... MArgs>
  iterator insert_at(size_type i, KArg &&key, MArgs &&...margs) {
    c_.keys.insert(c_.keys.begin() + i, std::forward<KArg>(key));
    try {
      c_.values.emplace(c_.values.begin() + i, std::forward<MArgs>(margs)...);
    } catch (...) {
      c_.keys.erase(c_.keys.begin() + i);
      throw;
    }
    return iterator_at(i);
  }

  template <typename KK, typename VV> void push_back_pair(KK &&k, VV &&v) {
    c_.keys.push_back(std::forward<KK>(k));
    try {
      c_.values.push_back(std::forward<VV>(v));
    } catch (...) {
      c_.keys.pop_back();
      throw;
    }
  }

  [[nodiscard]] bool is_sorted_unique() const {
    for (size_type i = 1; i < size(); ++i)
      if (!comp_(c_.keys[i - 1], c_.keys[i]))
        return false;
    return true;
  }

  void sort_and_unique() { sort_and_unique_keep_first(0); }

  // Stable-sort every key and keep the first element of each equivalence class.
  // Indices below from_head were already sorted and unique and carry the lower
  // original positions, so they win over later duplicates appended afterward.
  void sort_and_unique_keep_first(size_type from_head) {
    const size_type n = size();
    if (n - from_head == 0)
      return;
    if (from_head == n)
      return;

    vector<size_type> order(n);
    for (size_type i = 0; i < n; ++i)
      order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [this](size_type a, size_type b) {
                       return comp_(c_.keys[a], c_.keys[b]);
                     });

    vector<size_type> kept;
    kept.reserve(n);
    for (size_type p = 0; p < n; ++p) {
      if (p == 0 || comp_(c_.keys[order[p - 1]], c_.keys[order[p]]))
        kept.push_back(order[p]);
    }

    key_container_type new_keys;
    mapped_container_type new_values;
    new_keys.reserve(kept.size());
    new_values.reserve(kept.size());
    for (size_type idx : kept) {
      new_keys.push_back(std::move(c_.keys[idx]));
      new_values.push_back(std::move(c_.values[idx]));
    }
    c_.keys = std::move(new_keys);
    c_.values = std::move(new_values);
  }

  NX_NO_UNIQUE_ADDRESS containers c_{};
  NX_NO_UNIQUE_ADDRESS key_compare comp_{};
};

template <typename Key, typename T, typename Compare, typename KC, typename MC,
          typename Pred>
typename flat_map<Key, T, Compare, KC, MC>::size_type
erase_if(flat_map<Key, T, Compare, KC, MC> &m, Pred pred) {
  using size_type = typename flat_map<Key, T, Compare, KC, MC>::size_type;
  const size_type before = m.size();

  // Compact survivors forward through both arrays in lockstep, then trim. The
  // extract/replace round trip keeps the public const-key invariant intact
  // while still letting us move elements out of the underlying containers.
  auto cont = std::move(m).extract();
  size_type out = 0;
  const size_type n = cont.keys.size();
  for (size_type i = 0; i < n; ++i) {
    typename flat_map<Key, T, Compare, KC, MC>::reference ref{cont.keys[i],
                                                              cont.values[i]};
    if (pred(ref))
      continue;
    if (out != i) {
      cont.keys[out] = std::move(cont.keys[i]);
      cont.values[out] = std::move(cont.values[i]);
    }
    ++out;
  }
  cont.keys.erase(cont.keys.begin() + out, cont.keys.end());
  cont.values.erase(cont.values.begin() + out, cont.values.end());
  m.replace(std::move(cont.keys), std::move(cont.values));
  return before - m.size();
}

} // namespace nx