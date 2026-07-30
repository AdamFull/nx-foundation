#pragma once

/**
 * @file slot_map.h
 * @brief Densely packed handle-keyed pool (nx::slot_map).
 */

#include <iterator>
#include <span>
#include <stdexcept>
#include <type_traits>

#include "foundation/core/foundation.h"
#include "foundation/core/handle.h"

#include <cassert>

namespace nx {

/**
 * @brief Handle-keyed container that keeps live elements densely packed.
 *
 * insert() returns a versioned Handle that stays valid across moves; erase()
 * uses swap-and-pop, so element addresses are NOT stable and you should refer
 * to elements by handle, not by pointer. A stale handle (to a recycled slot)
 * reports as absent. Iteration and data() walk a contiguous array, so it is
 * cache-friendly.
 *
 * @tparam T      Element type.
 * @tparam Handle Handle type used as the key (carries index and version).
 * @tparam Alloc  Allocator type.
 */
template <typename T, typename Handle = handle32, typename Alloc = Allocator<T>>
class slot_map {
  using repr = Handle::repr_type;
  using alloc_traits = std::allocator_traits<Alloc>;
  using repr_alloc = alloc_traits::template rebind_alloc<repr>;

  static constexpr repr free_sentinel = Handle::null_index;

  struct sparse_slot {
    repr version;
    repr dense; // index into m_dense when live; free_sentinel when not live
  };
  using sparse_alloc = alloc_traits::template rebind_alloc<sparse_slot>;

public:
  using value_type = T;
  using handle_type = Handle;
  using size_type = usize;
  using difference_type = isize;
  using reference = T &;
  using const_reference = const T &;
  using pointer = T *;
  using const_pointer = const T *;
  using allocator_type = Alloc;

  using iterator = T *;
  using const_iterator = const T *;
  using reverse_iterator = std::reverse_iterator<T *>;
  using const_reverse_iterator = std::reverse_iterator<const T *>;

  slot_map() = default;

  explicit slot_map(const Alloc &alloc)
      : m_dense(alloc), m_dense_to_sparse(repr_alloc(alloc)),
        m_sparse(sparse_alloc(alloc)), m_free(repr_alloc(alloc)) {}

  [[nodiscard]] bool empty() const noexcept { return m_dense.empty(); }
  [[nodiscard]] usize size() const noexcept { return m_dense.size(); }
  [[nodiscard]] usize capacity() const noexcept { return m_dense.capacity(); }
  [[nodiscard]] usize max_size() const noexcept { return Handle::max_index; }

  [[nodiscard]] pointer data() noexcept { return m_dense.data(); }
  [[nodiscard]] const_pointer data() const noexcept { return m_dense.data(); }

  [[nodiscard]] std::span<T> as_span() noexcept {
    return {m_dense.data(), m_dense.size()};
  }
  [[nodiscard]] std::span<const T> as_span() const noexcept {
    return {m_dense.data(), m_dense.size()};
  }

  [[nodiscard]] bool contains(Handle h) const noexcept {
    const repr idx = h.index();
    if (idx >= m_sparse.size())
      return false;
    const sparse_slot &s = m_sparse[idx];
    return s.dense != free_sentinel && s.version == h.version();
  }

  [[nodiscard]] Handle insert(const T &value) { return emplace(value); }
  [[nodiscard]] Handle insert(T &&value) { return emplace(std::move(value)); }

  template <typename... Args> [[nodiscard]] Handle emplace(Args &&...args) {
    const repr idx = acquire_sparse();
    const repr d = nx::cast<repr>(m_dense.size());
    try {
      m_dense_to_sparse.push_back(idx);
    } catch (...) {
      release_sparse(idx);
      throw;
    }
    try {
      m_dense.emplace_back(std::forward<Args>(args)...);
    } catch (...) {
      m_dense_to_sparse.pop_back();
      release_sparse(idx);
      throw;
    }
    m_sparse[idx].dense = d;
    return Handle::from_parts(idx, m_sparse[idx].version);
  }

  bool erase(Handle h) {
    if (!contains(h))
      return false;
    const repr idx = h.index();
    const repr d = m_sparse[idx].dense;
    const repr last = nx::cast<repr>(m_dense.size() - 1);
    if (d != last) {
      m_dense[d] = std::move(m_dense[last]);
      const repr moved = m_dense_to_sparse[last];
      m_dense_to_sparse[d] = moved;
      m_sparse[moved].dense = d;
    }
    m_dense.pop_back();
    m_dense_to_sparse.pop_back();

    sparse_slot &s = m_sparse[idx];
    s.dense = free_sentinel;
    if (s.version < Handle::max_version) {
      ++s.version;
      m_free.push_back(idx);
    }
    return true;
  }

  [[nodiscard]] T *try_get(Handle h) noexcept {
    return contains(h) ? &m_dense[m_sparse[h.index()].dense] : nullptr;
  }
  [[nodiscard]] const T *try_get(Handle h) const noexcept {
    return contains(h) ? &m_dense[m_sparse[h.index()].dense] : nullptr;
  }

  [[nodiscard]] reference operator[](Handle h) noexcept {
    assert(contains(h));
    return m_dense[m_sparse[h.index()].dense];
  }
  [[nodiscard]] const_reference operator[](Handle h) const noexcept {
    assert(contains(h));
    return m_dense[m_sparse[h.index()].dense];
  }

  [[nodiscard]] reference at(Handle h) {
    if (!contains(h))
      throw std::out_of_range("slot_map::at");
    return m_dense[m_sparse[h.index()].dense];
  }
  [[nodiscard]] const_reference at(Handle h) const {
    if (!contains(h))
      throw std::out_of_range("slot_map::at");
    return m_dense[m_sparse[h.index()].dense];
  }

  // Recover the handle for the element currently at a dense position. Useful
  // when iterating values and needing to refer back to a stable handle.
  [[nodiscard]] Handle handle_at(usize dense_index) const noexcept {
    assert(dense_index < m_dense.size());
    const repr idx = m_dense_to_sparse[dense_index];
    return Handle::from_parts(idx, m_sparse[idx].version);
  }

  void reserve(usize n) {
    m_dense.reserve(n);
    m_dense_to_sparse.reserve(n);
    m_sparse.reserve(n);
  }

  void clear() noexcept {
    while (!m_dense.empty()) {
      const repr idx = m_dense_to_sparse.back();
      sparse_slot &s = m_sparse[idx];
      s.dense = free_sentinel;
      if (s.version < Handle::max_version) {
        ++s.version;
        m_free.push_back(idx);
      }
      m_dense.pop_back();
      m_dense_to_sparse.pop_back();
    }
  }

  [[nodiscard]] iterator begin() noexcept { return m_dense.data(); }
  [[nodiscard]] iterator end() noexcept {
    return m_dense.data() + m_dense.size();
  }
  [[nodiscard]] const_iterator begin() const noexcept { return m_dense.data(); }
  [[nodiscard]] const_iterator end() const noexcept {
    return m_dense.data() + m_dense.size();
  }
  [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
  [[nodiscard]] const_iterator cend() const noexcept { return end(); }
  [[nodiscard]] reverse_iterator rbegin() noexcept {
    return reverse_iterator{end()};
  }
  [[nodiscard]] reverse_iterator rend() noexcept {
    return reverse_iterator{begin()};
  }
  [[nodiscard]] const_reverse_iterator rbegin() const noexcept {
    return const_reverse_iterator{end()};
  }
  [[nodiscard]] const_reverse_iterator rend() const noexcept {
    return const_reverse_iterator{begin()};
  }
  [[nodiscard]] const_reverse_iterator crbegin() const noexcept {
    return rbegin();
  }
  [[nodiscard]] const_reverse_iterator crend() const noexcept { return rend(); }

  [[nodiscard]] allocator_type get_allocator() const noexcept {
    return m_dense.get_allocator();
  }

private:
  repr acquire_sparse() {
    if (!m_free.empty()) {
      const repr idx = m_free.back();
      m_free.pop_back();
      return idx;
    }
    assert(m_sparse.size() < Handle::null_index &&
           "slot_map: index space exhausted");
    const repr idx = nx::cast<repr>(m_sparse.size());
    m_sparse.push_back(sparse_slot{0, free_sentinel});
    m_free.reserve(m_sparse.size());
    return idx;
  }

  void release_sparse(repr idx) noexcept {
    sparse_slot &s = m_sparse[idx];
    s.dense = free_sentinel;
    if (s.version < Handle::max_version) {
      ++s.version;
      m_free.push_back(idx);
    }
  }

  vector<T, Alloc> m_dense;
  vector<repr, repr_alloc> m_dense_to_sparse;
  vector<sparse_slot, sparse_alloc> m_sparse;
  vector<repr, repr_alloc> m_free;
};

} // namespace nx