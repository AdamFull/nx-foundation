#pragma once

/**
 * @file blob.h
 * @brief Fixed-size, heap-allocated, owning array (nx::blob).
 */

#include "foundation/core/foundation.h"

#include <cassert>
#include <span>
#include <stdexcept>

namespace nx {

/**
 * @brief Owning array of @p T whose size is fixed at construction.
 *
 * Like a std::vector that never grows or shrinks: it allocates once, owns the
 * elements, and exposes a contiguous range. Use it for buffers whose length is
 * known up front and never changes (file contents, decoded data, scratch
 * space). Provides the usual container access (operator[], at, front, back,
 * data, iterators) plus comparison and span conversion.
 *
 * @tparam T     Element type.
 * @tparam Alloc Allocator type.
 */
template <typename T, typename Alloc = Allocator<T>> class blob {
  using ATraits = std::allocator_traits<Alloc>;
  static_assert(std::is_same_v<typename ATraits::value_type, T>,
                "blob allocator value_type must be T");

public:
  using value_type = T;
  using allocator_type = Alloc;
  using size_type = usize;
  using difference_type = isize;
  using reference = T &;
  using const_reference = const T &;
  using pointer = T *;
  using const_pointer = const T *;
  using iterator = T *;
  using const_iterator = const T *;
  using reverse_iterator = std::reverse_iterator<T *>;
  using const_reverse_iterator = std::reverse_iterator<const T *>;

  blob() noexcept(std::is_nothrow_default_constructible_v<Alloc>) {}
  explicit blob(const Alloc &a) noexcept : m_alloc(a) {}

  explicit blob(usize count, const Alloc &a = Alloc()) : m_alloc(a) {
    m_data = allocate_n(count);
    usize i = 0;
    try {
      for (; i < count; ++i)
        ATraits::construct(m_alloc, m_data + i);
    } catch (...) {
      destroy_range(m_data, i);
      deallocate_n(m_data, count);
      m_data = nullptr;
      throw;
    }
    m_size = count;
  }

  blob(usize count, const T &value, const Alloc &a = Alloc()) : m_alloc(a) {
    m_data = allocate_n(count);
    usize i = 0;
    try {
      for (; i < count; ++i)
        ATraits::construct(m_alloc, m_data + i, value);
    } catch (...) {
      destroy_range(m_data, i);
      deallocate_n(m_data, count);
      m_data = nullptr;
      throw;
    }
    m_size = count;
  }

  template <std::input_iterator It>
  blob(It first, It last, const Alloc &a = Alloc()) : m_alloc(a) {
    if constexpr (std::forward_iterator<It>) {
      const auto n = nx::cast<usize>(std::distance(first, last));
      m_data = allocate_n(n);
      usize i = 0;
      try {
        for (; first != last; ++first, ++i)
          ATraits::construct(m_alloc, m_data + i, *first);
      } catch (...) {
        destroy_range(m_data, i);
        deallocate_n(m_data, n);
        m_data = nullptr;
        throw;
      }
      m_size = n;
    } else {
      // Single-pass source: buffer first, then take a single fixed allocation.
      vector<T, Alloc> tmp(m_alloc);
      for (; first != last; ++first)
        tmp.push_back(*first);
      assign_move_from(tmp.data(), tmp.size());
    }
  }

  blob(std::initializer_list<T> il, const Alloc &a = Alloc())
      : blob(il.begin(), il.end(), a) {}

  explicit blob(std::span<const T> sp, const Alloc &a = Alloc())
      : blob(sp.begin(), sp.end(), a) {}

  blob(const blob &o)
      : m_alloc(ATraits::select_on_container_copy_construction(o.m_alloc)) {
    assign_copy_from(o.m_data, o.m_size);
  }
  blob(const blob &o, const Alloc &a) : m_alloc(a) {
    assign_copy_from(o.m_data, o.m_size);
  }

  blob(blob &&o) noexcept : m_alloc(std::move(o.m_alloc)) { steal(o); }
  blob(blob &&o, const Alloc &a) : m_alloc(a) {
    if (m_alloc == o.m_alloc)
      steal(o);
    else {
      assign_move_from(o.m_data, o.m_size);
      o.free_all();
    }
  }

  ~blob() { free_all(); }

  blob &operator=(const blob &o) {
    if (this == &o)
      return *this;
    if constexpr (ATraits::propagate_on_container_copy_assignment::value) {
      if (m_alloc != o.m_alloc)
        free_all();
      m_alloc = o.m_alloc;
    }
    assign_copy_from(o.m_data, o.m_size);
    return *this;
  }

  blob &operator=(blob &&o) noexcept(
      ATraits::propagate_on_container_move_assignment::value ||
      ATraits::is_always_equal::value) {
    if (this == &o)
      return *this;
    if constexpr (ATraits::propagate_on_container_move_assignment::value) {
      free_all();
      m_alloc = std::move(o.m_alloc);
      steal(o);
    } else if (m_alloc == o.m_alloc) {
      free_all();
      steal(o);
    } else {
      assign_move_from(o.m_data, o.m_size);
      o.free_all();
    }
    return *this;
  }

  /// Takes ownership of an externally allocated, fully constructed buffer.
  // Adopts a buffer of `size` elements that was allocated and constructed with
  // an equal allocator; on destruction it is deallocated with `size`.
  [[nodiscard]] static blob from_owned(T *data, usize size,
                                       const Alloc &a = Alloc()) noexcept {
    blob b(a);
    b.m_data = data;
    b.m_size = size;
    return b;
  }

  struct owned {
    T *data;
    usize size;
  };

  /// Releases ownership of the buffer to the caller, leaving the blob empty.
  [[nodiscard]] owned release() noexcept {
    owned r{m_data, m_size};
    m_data = nullptr;
    m_size = 0;
    return r;
  }

  [[nodiscard]] Alloc get_allocator() const noexcept { return m_alloc; }

  [[nodiscard]] reference at(usize i) {
    if (i >= m_size)
      throw std::out_of_range("nx::blob::at");
    return m_data[i];
  }
  [[nodiscard]] const_reference at(usize i) const {
    if (i >= m_size)
      throw std::out_of_range("nx::blob::at");
    return m_data[i];
  }

  [[nodiscard]] reference operator[](usize i) noexcept {
    assert(i < m_size);
    return m_data[i];
  }
  [[nodiscard]] const_reference operator[](usize i) const noexcept {
    assert(i < m_size);
    return m_data[i];
  }
  [[nodiscard]] reference front() noexcept {
    assert(!empty());
    return m_data[0];
  }
  [[nodiscard]] const_reference front() const noexcept {
    assert(!empty());
    return m_data[0];
  }
  [[nodiscard]] reference back() noexcept {
    assert(!empty());
    return m_data[m_size - 1];
  }
  [[nodiscard]] const_reference back() const noexcept {
    assert(!empty());
    return m_data[m_size - 1];
  }

  [[nodiscard]] pointer data() noexcept { return m_data; }
  [[nodiscard]] const_pointer data() const noexcept { return m_data; }

  [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
  [[nodiscard]] usize size() const noexcept { return m_size; }
  [[nodiscard]] usize bytes_size() const noexcept { return m_size * sizeof(T); }

  [[nodiscard]] iterator begin() noexcept { return m_data; }
  [[nodiscard]] iterator end() noexcept { return m_data + m_size; }
  [[nodiscard]] const_iterator begin() const noexcept { return m_data; }
  [[nodiscard]] const_iterator end() const noexcept { return m_data + m_size; }
  [[nodiscard]] const_iterator cbegin() const noexcept { return m_data; }
  [[nodiscard]] const_iterator cend() const noexcept { return m_data + m_size; }
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
    return const_reverse_iterator{cend()};
  }
  [[nodiscard]] const_reverse_iterator crend() const noexcept {
    return const_reverse_iterator{cbegin()};
  }

  [[nodiscard]] vector<T, Alloc> to_vector() const {
    return vector<T, Alloc>(m_data, m_data + m_size, m_alloc);
  }

  operator std::span<const T>() const noexcept { return {m_data, m_size}; }
  explicit operator std::span<T>() noexcept { return {m_data, m_size}; }

  void swap(blob &o) noexcept {
    using std::swap;
    if constexpr (ATraits::propagate_on_container_swap::value)
      swap(m_alloc, o.m_alloc);
    swap(m_data, o.m_data);
    swap(m_size, o.m_size);
  }

  [[nodiscard]] friend bool operator==(const blob &a, const blob &b) {
    return a.m_size == b.m_size && std::equal(a.begin(), a.end(), b.begin());
  }
  [[nodiscard]] friend auto operator<=>(const blob &a, const blob &b)
    requires std::three_way_comparable<T>
  {
    return std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(),
                                                  b.end());
  }

private:
  NX_NO_UNIQUE_ADDRESS Alloc m_alloc;
  T *m_data = nullptr;
  usize m_size = 0;

  T *allocate_n(usize n) { return n ? ATraits::allocate(m_alloc, n) : nullptr; }
  void deallocate_n(T *p, usize n) noexcept {
    if (p)
      ATraits::deallocate(m_alloc, p, n);
  }
  void destroy_range(T *p, usize n) noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>)
      for (usize i = 0; i < n; ++i)
        ATraits::destroy(m_alloc, p + i);
  }

  void free_all() noexcept {
    destroy_range(m_data, m_size);
    deallocate_n(m_data, m_size);
    m_data = nullptr;
    m_size = 0;
  }

  void steal(blob &o) noexcept {
    m_data = o.m_data;
    m_size = o.m_size;
    o.m_data = nullptr;
    o.m_size = 0;
  }

  void assign_copy_from(const T *src, usize n) {
    free_all();
    if (n == 0)
      return;
    T *nd = allocate_n(n);
    usize i = 0;
    try {
      for (; i < n; ++i)
        ATraits::construct(m_alloc, nd + i, src[i]);
    } catch (...) {
      destroy_range(nd, i);
      deallocate_n(nd, n);
      throw;
    }
    m_data = nd;
    m_size = n;
  }

  void assign_move_from(T *src, usize n) {
    free_all();
    if (n == 0)
      return;
    T *nd = allocate_n(n);
    usize i = 0;
    try {
      for (; i < n; ++i)
        ATraits::construct(m_alloc, nd + i, std::move(src[i]));
    } catch (...) {
      destroy_range(nd, i);
      deallocate_n(nd, n);
      throw;
    }
    m_data = nd;
    m_size = n;
  }

  template <typename, typename> friend class blob;
};

template <typename T, typename Alloc>
void swap(blob<T, Alloc> &a, blob<T, Alloc> &b) noexcept {
  a.swap(b);
}

} // namespace nx