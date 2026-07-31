#pragma once

/**
 * @file sparse_set.h
 * @brief Sparse set / handle-keyed component store (nx::sparse_set).
 */

/**
 * @brief Sparse set mapping externally owned handles to dense payloads.
 *
 * Maps a Handle (such as an entity id) to a value with O(1) insert, erase and
 * contains, while keeping the payload contiguous for bandwidth-bound iteration.
 * The sparse side is paged, so a few high handle ids cost one page each rather
 * than a flat array. With T = void it acts as a set of handles. Erase is
 * swap-and-pop, so addresses are not stable; refer to elements by handle.
 *
 * @tparam T        Mapped value type, or void for a plain set.
 * @tparam Handle   Handle type used as the key.
 * @tparam PageSize Elements per sparse page; must be a power of two.
 * @tparam Alloc    Allocator type.
 */

#include <algorithm>
#include <iterator>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "core/foundation/core/foundation.h"
#include "core/foundation/core/handle.h"

#include <cassert>

namespace nx {

namespace detail {

struct sparse_set_empty {};

template <typename Ref> struct ss_arrow_proxy {
  Ref ref;
  Ref *operator->() noexcept { return std::addressof(ref); }
};

} // namespace detail

template <typename T = void, typename Handle = handle32, usize PageSize = 4096,
          typename Alloc = Allocator<Handle>>
class sparse_set {
  static_assert(is_pow2(PageSize), "sparse_set: PageSize must be a power of 2");

  using repr = Handle::repr_type;
  using alloc_traits = std::allocator_traits<Alloc>;

  static constexpr bool has_value = !std::is_void_v<T>;
  static constexpr repr sentinel = Handle::null_index;

  using handle_alloc = alloc_traits::template rebind_alloc<Handle>;
  using repr_alloc = alloc_traits::template rebind_alloc<repr>;
  using page_type = vector<repr, repr_alloc>;
  using page_alloc = alloc_traits::template rebind_alloc<page_type>;

  // value_t stays a real type when T is void so the dependent vector type is
  // always well formed; the storage itself collapses to an empty member.
  using value_t = std::conditional_t<has_value, T, char>;
  using value_alloc = alloc_traits::template rebind_alloc<value_t>;
  using value_storage =
      std::conditional_t<has_value, vector<value_t, value_alloc>,
                         detail::sparse_set_empty>;

  using reference = std::add_lvalue_reference_t<value_t>;
  using const_reference = std::add_lvalue_reference_t<const value_t>;

public:
  using handle_type = Handle;
  using mapped_type = T;
  using value_type = std::conditional_t<has_value, T, Handle>;
  using size_type = usize;
  using difference_type = isize;
  using allocator_type = Alloc;

  using iterator = std::conditional_t<has_value, T *, const Handle *>;
  using const_iterator =
      std::conditional_t<has_value, const T *, const Handle *>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  static constexpr usize page_size = PageSize;

  sparse_set() = default;

  explicit sparse_set(const Alloc &alloc)
      : m_dense(handle_alloc(alloc)), m_values(make_values(alloc)),
        m_pages(page_alloc(alloc)) {}

  [[nodiscard]] bool empty() const noexcept { return m_dense.empty(); }
  [[nodiscard]] size_type size() const noexcept { return m_dense.size(); }
  [[nodiscard]] size_type capacity() const noexcept {
    return m_dense.capacity();
  }
  [[nodiscard]] static constexpr size_type max_size() noexcept {
    return Handle::max_index;
  }

  void reserve(size_type n) {
    m_dense.reserve(n);
    if constexpr (has_value)
      m_values.reserve(n);
  }

  void shrink_to_fit() {
    m_dense.shrink_to_fit();
    if constexpr (has_value)
      m_values.shrink_to_fit();
    while (!m_pages.empty() && m_pages.back().empty())
      m_pages.pop_back();
    m_pages.shrink_to_fit();
  }

  void clear() noexcept {
    m_dense.clear();
    if constexpr (has_value)
      m_values.clear();
    m_pages.clear();
  }

  [[nodiscard]] bool contains(Handle h) const noexcept {
    const repr pos = sparse_lookup(h.index());
    return pos != sentinel && m_dense[pos] == h;
  }

  // Dense position of a live handle. Precondition: contains(h).
  [[nodiscard]] size_type index_of(Handle h) const noexcept {
    assert(contains(h));
    return sparse_lookup(h.index());
  }

  // Handle currently occupying a dense position; pairs with begin()/values().
  [[nodiscard]] Handle handle_at(size_type dense_index) const noexcept {
    assert(dense_index < m_dense.size());
    return m_dense[dense_index];
  }

  template <typename... Args>
  reference emplace(Handle h, Args &&...args)
    requires has_value
  {
    assert(h.valid() && "sparse_set::emplace: null handle");
    assert(!contains(h) && "sparse_set::emplace: handle already present");
    const repr pos = nx::cast<repr>(m_dense.size());
    m_dense.push_back(h);
    try {
      m_values.emplace_back(std::forward<Args>(args)...);
    } catch (...) {
      m_dense.pop_back();
      throw;
    }
    try {
      sparse_assign(h.index(), pos);
    } catch (...) {
      m_values.pop_back();
      m_dense.pop_back();
      throw;
    }
    return m_values.back();
  }

  template <typename V>
    requires has_value && std::is_constructible_v<T, V &&>
  std::pair<reference, bool> insert(Handle h, V &&value) {
    if (const repr pos = sparse_lookup(h.index());
        pos != sentinel && m_dense[pos] == h)
      return {m_values[pos], false};
    return {emplace(h, std::forward<V>(value)), true};
  }

  template <typename M>
  reference insert_or_assign(Handle h, M &&value)
    requires has_value
  {
    if (const repr pos = sparse_lookup(h.index());
        pos != sentinel && m_dense[pos] == h) {
      m_values[pos] = std::forward<M>(value);
      return m_values[pos];
    }
    return emplace(h, std::forward<M>(value));
  }

  template <typename... Args>
  reference get_or_emplace(Handle h, Args &&...args)
    requires has_value
  {
    if (const repr pos = sparse_lookup(h.index());
        pos != sentinel && m_dense[pos] == h)
      return m_values[pos];
    return emplace(h, std::forward<Args>(args)...);
  }

  bool insert(Handle h)
    requires(!has_value)
  {
    assert(h.valid() && "sparse_set::insert: null handle");
    if (contains(h))
      return false;
    const repr pos = nx::cast<repr>(m_dense.size());
    m_dense.push_back(h);
    try {
      sparse_assign(h.index(), pos);
    } catch (...) {
      m_dense.pop_back();
      throw;
    }
    return true;
  }

  bool erase(Handle h) {
    const repr i = h.index();
    const repr pos = sparse_lookup(i);
    if (pos == sentinel || m_dense[pos] != h)
      return false;

    const repr last = nx::cast<repr>(m_dense.size() - 1);
    if (pos != last) {
      m_dense[pos] = m_dense[last];
      if constexpr (has_value)
        m_values[pos] = std::move(m_values[last]);
      sparse_assign(m_dense[pos].index(), pos);
    }
    m_dense.pop_back();
    if constexpr (has_value)
      m_values.pop_back();
    sparse_clear(i);
    return true;
  }

  [[nodiscard]] reference operator[](Handle h) noexcept
    requires has_value
  {
    assert(contains(h));
    return m_values[sparse_lookup(h.index())];
  }
  [[nodiscard]] const_reference operator[](Handle h) const noexcept
    requires has_value
  {
    assert(contains(h));
    return m_values[sparse_lookup(h.index())];
  }

  [[nodiscard]] reference at(Handle h)
    requires has_value
  {
    const repr pos = sparse_lookup(h.index());
    if (pos == sentinel || m_dense[pos] != h)
      throw std::out_of_range("sparse_set::at");
    return m_values[pos];
  }
  [[nodiscard]] const_reference at(Handle h) const
    requires has_value
  {
    const repr pos = sparse_lookup(h.index());
    if (pos == sentinel || m_dense[pos] != h)
      throw std::out_of_range("sparse_set::at");
    return m_values[pos];
  }

  [[nodiscard]] T *try_get(Handle h) noexcept
    requires has_value
  {
    const repr pos = sparse_lookup(h.index());
    return (pos != sentinel && m_dense[pos] == h) ? &m_values[pos] : nullptr;
  }
  [[nodiscard]] const T *try_get(Handle h) const noexcept
    requires has_value
  {
    const repr pos = sparse_lookup(h.index());
    return (pos != sentinel && m_dense[pos] == h) ? &m_values[pos] : nullptr;
  }

  [[nodiscard]] iterator find(Handle h) noexcept {
    const repr pos = sparse_lookup(h.index());
    return (pos != sentinel && m_dense[pos] == h) ? begin() + pos : end();
  }
  [[nodiscard]] const_iterator find(Handle h) const noexcept {
    const repr pos = sparse_lookup(h.index());
    return (pos != sentinel && m_dense[pos] == h) ? begin() + pos : end();
  }

  [[nodiscard]] std::span<const Handle> entities() const noexcept {
    return {m_dense.data(), m_dense.size()};
  }

  [[nodiscard]] T *data() noexcept
    requires has_value
  {
    return m_values.data();
  }
  [[nodiscard]] const T *data() const noexcept
    requires has_value
  {
    return m_values.data();
  }
  [[nodiscard]] std::span<T> values() noexcept
    requires has_value
  {
    return {m_values.data(), m_values.size()};
  }
  [[nodiscard]] std::span<const T> values() const noexcept
    requires has_value
  {
    return {m_values.data(), m_values.size()};
  }

  [[nodiscard]] iterator begin() noexcept { return dense_begin(); }
  [[nodiscard]] const_iterator begin() const noexcept { return dense_begin(); }
  [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
  [[nodiscard]] iterator end() noexcept { return dense_begin() + size(); }
  [[nodiscard]] const_iterator end() const noexcept {
    return dense_begin() + size();
  }
  [[nodiscard]] const_iterator cend() const noexcept { return end(); }
  [[nodiscard]] reverse_iterator rbegin() noexcept {
    return reverse_iterator{end()};
  }
  [[nodiscard]] const_reverse_iterator rbegin() const noexcept {
    return const_reverse_iterator{end()};
  }
  [[nodiscard]] const_reverse_iterator crbegin() const noexcept {
    return rbegin();
  }
  [[nodiscard]] reverse_iterator rend() noexcept {
    return reverse_iterator{begin()};
  }
  [[nodiscard]] const_reverse_iterator rend() const noexcept {
    return const_reverse_iterator{begin()};
  }
  [[nodiscard]] const_reverse_iterator crend() const noexcept { return rend(); }

  template <bool Const> class each_iterator {
    friend class sparse_set;
    using val_ptr = std::conditional_t<Const, const T *, T *>;
    const Handle *m_e = nullptr;
    val_ptr m_v = nullptr;
    each_iterator(const Handle *e, val_ptr v) noexcept : m_e(e), m_v(v) {}

  public:
    using value_type = std::pair<Handle, T>;
    using reference =
        std::pair<Handle, std::conditional_t<Const, const T &, T &>>;
    using pointer = detail::ss_arrow_proxy<reference>;
    using difference_type = isize;
    using iterator_category = std::forward_iterator_tag;

    each_iterator() = default;
    template <bool C = Const, std::enable_if_t<C, i32> = 0>
    each_iterator(const each_iterator<false> &o) noexcept
        : m_e(o.m_e), m_v(o.m_v) {}

    [[nodiscard]] reference operator*() const { return reference{*m_e, *m_v}; }
    [[nodiscard]] pointer operator->() const {
      return pointer{reference{*m_e, *m_v}};
    }
    each_iterator &operator++() noexcept {
      ++m_e;
      ++m_v;
      return *this;
    }
    each_iterator operator++(i32) noexcept {
      each_iterator tmp = *this;
      ++*this;
      return tmp;
    }
    [[nodiscard]] friend bool operator==(const each_iterator &a,
                                         const each_iterator &b) noexcept {
      return a.m_e == b.m_e;
    }
    [[nodiscard]] friend bool operator!=(const each_iterator &a,
                                         const each_iterator &b) noexcept {
      return a.m_e != b.m_e;
    }
  };

  template <bool Const> class each_range {
    friend class sparse_set;
    using val_ptr = std::conditional_t<Const, const T *, T *>;
    const Handle *m_e;
    val_ptr m_v;
    usize m_n;
    each_range(const Handle *e, val_ptr v, usize n) noexcept
        : m_e(e), m_v(v), m_n(n) {}

  public:
    [[nodiscard]] each_iterator<Const> begin() const noexcept {
      return each_iterator<Const>{m_e, m_v};
    }
    [[nodiscard]] each_iterator<Const> end() const noexcept {
      return each_iterator<Const>{m_e + m_n, m_v + m_n};
    }
  };

  // Iterate (handle, value&) pairs in dense order, e.g.
  //   for (auto [e, pos] : positions.each()) pos.x += 1;
  [[nodiscard]] each_range<false> each() noexcept
    requires has_value
  {
    return each_range<false>{m_dense.data(), m_values.data(), m_dense.size()};
  }
  [[nodiscard]] each_range<true> each() const noexcept
    requires has_value
  {
    return each_range<true>{m_dense.data(), m_values.data(), m_dense.size()};
  }

  // Reorder the dense arrays by value, keeping the sparse mapping consistent.
  // Use after bulk changes to restore locality or to impose a draw/update
  // order.
  template <typename Compare>
  void sort(Compare comp)
    requires has_value
  {
    apply_permutation(make_order(
        [&](usize a, usize b) { return comp(m_values[a], m_values[b]); }));
  }

  template <typename Compare> void sort_handles(Compare comp) {
    apply_permutation(make_order(
        [&](usize a, usize b) { return comp(m_dense[a], m_dense[b]); }));
  }

  void swap(sparse_set &o) noexcept {
    using std::swap;
    swap(m_dense, o.m_dense);
    if constexpr (has_value)
      swap(m_values, o.m_values);
    swap(m_pages, o.m_pages);
  }

  friend void swap(sparse_set &a, sparse_set &b) noexcept { a.swap(b); }

  [[nodiscard]] allocator_type get_allocator() const noexcept {
    return allocator_type(m_dense.get_allocator());
  }

private:
  static value_storage make_values(const Alloc &alloc) {
    if constexpr (has_value)
      return value_storage(value_alloc(alloc));
    else {
      (void)alloc;
      return value_storage{};
    }
  }

  iterator dense_begin() noexcept {
    if constexpr (has_value)
      return m_values.data();
    else
      return m_dense.data();
  }
  const_iterator dense_begin() const noexcept {
    if constexpr (has_value)
      return m_values.data();
    else
      return m_dense.data();
  }

  [[nodiscard]] repr sparse_lookup(repr index) const noexcept {
    const usize page = index / PageSize;
    if (page >= m_pages.size() || m_pages[page].empty())
      return sentinel;
    return m_pages[page][index % PageSize];
  }

  void sparse_assign(repr index, repr pos) {
    const usize page = index / PageSize;
    if (page >= m_pages.size())
      m_pages.resize(page + 1);
    if (m_pages[page].empty())
      m_pages[page].assign(PageSize, sentinel);
    m_pages[page][index % PageSize] = pos;
  }

  void sparse_clear(repr index) noexcept {
    const usize page = index / PageSize;
    m_pages[page][index % PageSize] = sentinel;
  }

  template <typename Less> vector<usize> make_order(Less less) const {
    const usize n = m_dense.size();
    vector<usize> order(n);
    for (usize i = 0; i < n; ++i)
      order[i] = i;
    std::stable_sort(order.begin(), order.end(), less);
    return order;
  }

  void apply_permutation(const vector<usize> &order) {
    const usize n = order.size();
    vector<Handle, handle_alloc> new_dense(m_dense.get_allocator());
    new_dense.reserve(n);
    for (usize i = 0; i < n; ++i)
      new_dense.push_back(m_dense[order[i]]);

    if constexpr (has_value) {
      value_storage new_values(m_values.get_allocator());
      new_values.reserve(n);
      for (usize i = 0; i < n; ++i)
        new_values.push_back(std::move(m_values[order[i]]));
      m_values = std::move(new_values);
    }
    m_dense = std::move(new_dense);

    for (usize i = 0; i < n; ++i)
      sparse_assign(m_dense[i].index(), nx::cast<repr>(i));
  }

  vector<Handle, handle_alloc> m_dense;
  NX_NO_UNIQUE_ADDRESS value_storage m_values;
  vector<page_type, page_alloc> m_pages;
};

} // namespace nx