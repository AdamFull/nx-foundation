#pragma once

/**
 * @file small_vector.h
 * @brief Vector with small-buffer optimization (nx::small_vector).
 */

#include <algorithm>
#include <compare>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>

#include "foundation/core/foundation.h"

#include <cassert>

namespace nx {

/**
 * @brief Dynamic vector that keeps its first @p N elements inline.
 *
 * Behaves like std::vector but avoids the heap while the size stays within @p
 * N; it spills to an allocation only when it grows beyond the inline buffer.
 * Good for collections that are usually small but occasionally large.
 *
 * @tparam T     Element type.
 * @tparam N     Number of elements kept inline before spilling to the heap.
 * @tparam Alloc Allocator used once the inline buffer is exceeded.
 */
template <typename T, usize N, typename Alloc = Allocator<T>>
class small_vector {
  static_assert(N > 0, "small_vector: N must be > 0");
  using alloc_traits = std::allocator_traits<Alloc>;

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

  small_vector() noexcept(std::is_nothrow_default_constructible_v<Alloc>) {}

  explicit small_vector(const Alloc &alloc) noexcept : m_alloc(alloc) {}

  explicit small_vector(const usize count, const Alloc &alloc = Alloc())
      : m_alloc(alloc) {
    resize(count);
  }

  small_vector(const usize count, const T &value, const Alloc &alloc = Alloc())
      : m_alloc(alloc) {
    assign(count, value);
  }

  template <std::input_iterator It>
  small_vector(It first, It last, const Alloc &alloc = Alloc())
      : m_alloc(alloc) {
    assign(first, last);
  }

  small_vector(std::initializer_list<T> il, const Alloc &alloc = Alloc())
      : m_alloc(alloc) {
    assign(il.begin(), il.end());
  }

  explicit small_vector(std::span<const T> sp, const Alloc &alloc = Alloc())
      : m_alloc(alloc) {
    assign(sp.begin(), sp.end());
  }

  small_vector(const small_vector &other)
      : m_alloc(alloc_traits::select_on_container_copy_construction(
            other.m_alloc)) {
    reserve(other.m_size);
    for (usize i = 0; i < other.m_size; ++i)
      alloc_traits::construct(m_alloc, m_data + i, other.m_data[i]);
    m_size = other.m_size;
  }

  small_vector(const small_vector &other, const Alloc &alloc) : m_alloc(alloc) {
    reserve(other.m_size);
    for (usize i = 0; i < other.m_size; ++i)
      alloc_traits::construct(m_alloc, m_data + i, other.m_data[i]);
    m_size = other.m_size;
  }

  small_vector(small_vector &&other) noexcept(
      std::is_nothrow_move_constructible_v<T>)
      : m_alloc(std::move(other.m_alloc)) {
    steal_or_move(other);
  }

  small_vector(small_vector &&other, const Alloc &alloc) noexcept(
      std::is_nothrow_move_constructible_v<T>)
      : m_alloc(alloc) {
    if (m_alloc == other.m_alloc)
      steal_or_move(other);
    else {
      reserve(other.m_size);
      for (usize i = 0; i < other.m_size; ++i) {
        alloc_traits::construct(m_alloc, m_data + i,
                                std::move(other.m_data[i]));
        alloc_traits::destroy(m_alloc, other.m_data + i);
      }
      m_size = other.m_size;
      other.m_size = 0;
    }
  }

  ~small_vector() noexcept { release(); }

  small_vector &operator=(const small_vector &other) {
    if (this == &other)
      return *this;

    if constexpr (alloc_traits::propagate_on_container_copy_assignment::value) {
      if (m_alloc != other.m_alloc) {
        release();
        m_alloc = other.m_alloc;
      }
    }
    assign(other.begin(), other.end());
    return *this;
  }

  small_vector &operator=(small_vector &&other) noexcept(
      alloc_traits::propagate_on_container_move_assignment::value &&
      std::is_nothrow_move_constructible_v<T>) {
    if (this == &other)
      return *this;

    release();

    if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
      m_alloc = std::move(other.m_alloc);
      steal_or_move(other);
    } else {
      if (m_alloc == other.m_alloc)
        steal_or_move(other);
      else {
        if (other.m_size > m_capacity)
          reallocate(other.m_size);
        for (usize i = 0; i < other.m_size; ++i) {
          alloc_traits::construct(m_alloc, m_data + i,
                                  std::move(other.m_data[i]));
          alloc_traits::destroy(m_alloc, other.m_data + i);
        }
        m_size = other.m_size;
        other.m_size = 0;
      }
    }
    return *this;
  }

  small_vector &operator=(std::initializer_list<T> il) {
    assign(il.begin(), il.end());
    return *this;
  }

  void assign(const usize count, const T &value) {
    clear();
    if (count > m_capacity)
      reallocate(count);

    for (usize i = 0; i < count; ++i)
      alloc_traits::construct(m_alloc, m_data + i, value);
    m_size = count;
  }

  template <std::input_iterator It> void assign(It first, It last) {
    clear();
    if constexpr (std::random_access_iterator<It>) {
      if (usize count = cast<usize>(std::distance(first, last));
          count > m_capacity)
        reallocate(count);
    }
    for (; first != last; ++first)
      emplace_back(*first);
  }

  void assign(std::initializer_list<T> il) { assign(il.begin(), il.end()); }

  [[nodiscard]] reference at(usize i) {
    if (i >= m_size)
      throw std::out_of_range("small_vector::at");
    return m_data[i];
  }
  [[nodiscard]] const_reference at(usize i) const {
    if (i >= m_size)
      throw std::out_of_range("small_vector::at");
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

  [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
  [[nodiscard]] usize size() const noexcept { return m_size; }
  [[nodiscard]] usize max_size() const noexcept {
    return alloc_traits::max_size(m_alloc);
  }
  [[nodiscard]] usize capacity() const noexcept { return m_capacity; }

  void reserve(usize n) {
    if (n > m_capacity)
      reallocate(n);
  }

  void shrink_to_fit() {
    if (is_inline())
      return;

    if (m_size <= N) {
      T *old_data = m_data;
      usize old_cap = m_capacity;
      m_data = inline_ptr();
      m_capacity = N;
      for (usize i = 0; i < m_size; ++i) {
        alloc_traits::construct(m_alloc, m_data + i, std::move(old_data[i]));
        alloc_traits::destroy(m_alloc, old_data + i);
      }
      alloc_traits::deallocate(m_alloc, old_data, old_cap);
    } else if (m_size < m_capacity)
      reallocate(m_size);
  }

  void clear() noexcept {
    for (usize i = 0; i < m_size; ++i)
      alloc_traits::destroy(m_alloc, m_data + i);
    m_size = 0;
  }

  void push_back(const T &v) { emplace_back(v); }
  void push_back(T &&v) { emplace_back(std::move(v)); }

  template <typename... Args> reference emplace_back(Args &&...args) {
    if (m_size == m_capacity)
      grow();
    alloc_traits::construct(m_alloc, m_data + m_size,
                            std::forward<Args>(args)...);
    return m_data[m_size++];
  }

  void pop_back() noexcept {
    assert(!empty());
    alloc_traits::destroy(m_alloc, m_data + --m_size);
  }

  template <typename... Args>
  iterator emplace(const_iterator pos, Args &&...args) {
    usize off = nx::cast<usize>(pos - m_data);
    ensure_space(1);
    T *p = m_data + off;
    shift_right(p, 1);
    alloc_traits::construct(m_alloc, p, std::forward<Args>(args)...);
    ++m_size;
    return p;
  }

  iterator insert(const_iterator pos, const T &v) { return emplace(pos, v); }
  iterator insert(const_iterator pos, T &&v) {
    return emplace(pos, std::move(v));
  }

  iterator insert(const_iterator pos, usize count, const T &v) {
    if (count == 0)
      return const_cast<iterator>(pos);

    usize off = nx::cast<usize>(pos - m_data);
    ensure_space(count);
    T *p = m_data + off;
    shift_right(p, count);
    for (usize i = 0; i < count; ++i)
      alloc_traits::construct(m_alloc, p + i, v);

    m_size += count;
    return p;
  }

  template <std::input_iterator It>
  iterator insert(const_iterator pos, It first, It last) {
    usize off = nx::cast<usize>(pos - m_data);
    if constexpr (std::random_access_iterator<It>) {
      const usize count = nx::cast<usize>(std::distance(first, last));
      if (count == 0)
        return m_data + off;

      ensure_space(count);
      T *p = m_data + off;
      shift_right(p, count);
      for (usize i = 0; first != last; ++first, ++i)
        alloc_traits::construct(m_alloc, p + i, *first);

      m_size += count;
      return p;
    } else {
      usize tail = m_size;
      for (; first != last; ++first)
        emplace_back(*first);
      std::rotate(m_data + off, m_data + tail, m_data + m_size);
      return m_data + off;
    }
  }

  iterator insert(const const_iterator pos, std::initializer_list<T> il) {
    return insert(pos, il.begin(), il.end());
  }

  iterator erase(const_iterator pos) { return erase(pos, pos + 1); }

  iterator erase(const_iterator first, const_iterator last) {
    T *f = const_cast<T *>(first);
    T *l = const_cast<T *>(last);
    const auto n = nx::cast<usize>(l - f);
    if (n == 0)
      return f;

    T *new_end = std::move(l, m_data + m_size, f);
    for (T *p = new_end; p != m_data + m_size; ++p)
      alloc_traits::destroy(m_alloc, p);
    m_size -= n;
    return f;
  }

  void resize(const usize count) {
    if (count < m_size) {
      for (usize i = count; i < m_size; ++i)
        alloc_traits::destroy(m_alloc, m_data + i);
    } else if (count > m_size) {
      if (count > m_capacity)
        reallocate(count);
      for (usize i = m_size; i < count; ++i)
        alloc_traits::construct(m_alloc, m_data + i);
    }
    m_size = count;
  }

  void resize(const usize count, const T &value) {
    if (count < m_size) {
      for (usize i = count; i < m_size; ++i)
        alloc_traits::destroy(m_alloc, m_data + i);
    } else if (count > m_size) {
      if (count > m_capacity)
        reallocate(count);
      for (usize i = m_size; i < count; ++i)
        alloc_traits::construct(m_alloc, m_data + i, value);
    }
    m_size = count;
  }

  void swap(small_vector &other) noexcept {
    if (this == &other)
      return;

    if (!is_inline() && !other.is_inline()) {
      if constexpr (alloc_traits::propagate_on_container_swap::value) {
        using std::swap;
        swap(m_alloc, other.m_alloc);
      } else {
        assert(m_alloc == other.m_alloc &&
               "small_vector::swap: non-propagating unequal allocators");
      }
      std::swap(m_data, other.m_data);
      std::swap(m_size, other.m_size);
      std::swap(m_capacity, other.m_capacity);
    } else {
      small_vector tmp(std::move(*this));
      *this = std::move(other);
      other = std::move(tmp);
    }
  }

  operator std::span<const T>() const noexcept { return {m_data, m_size}; }
  explicit operator std::span<T>() noexcept { return {m_data, m_size}; }

  [[nodiscard]] friend bool operator==(const small_vector &a,
                                       const small_vector &b) {
    return a.m_size == b.m_size && std::equal(a.begin(), a.end(), b.begin());
  }

  [[nodiscard]] friend auto operator<=>(const small_vector &a,
                                        const small_vector &b)
    requires std::three_way_comparable<T>
  {
    return std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(),
                                                  b.end());
  }

  [[nodiscard]] allocator_type get_allocator() const noexcept {
    return m_alloc;
  }

private:
  alignas(T) std::byte m_inline_storage[N * sizeof(T)];

  NX_NO_UNIQUE_ADDRESS Alloc m_alloc;

  T *m_data = reinterpret_cast<T *>(m_inline_storage);
  usize m_size = 0;
  usize m_capacity = N;

  T *inline_ptr() noexcept { return reinterpret_cast<T *>(m_inline_storage); }
  const T *inline_ptr() const noexcept {
    return reinterpret_cast<const T *>(m_inline_storage);
  }
  bool is_inline() const noexcept { return m_data == inline_ptr(); }

  void release() noexcept {
    clear();
    if (!is_inline()) {
      alloc_traits::deallocate(m_alloc, m_data, m_capacity);
      m_data = inline_ptr();
      m_capacity = N;
    }
  }

  void grow() { reallocate(m_capacity * 2); }

  void ensure_space(usize extra) {
    if (m_size + extra <= m_capacity)
      return;
    usize new_cap = m_capacity;
    while (new_cap < m_size + extra)
      new_cap *= 2;
    reallocate(new_cap);
  }

  void reallocate(usize new_cap) {
    T *new_data = alloc_traits::allocate(m_alloc, new_cap);
    for (usize i = 0; i < m_size; ++i) {
      alloc_traits::construct(m_alloc, new_data + i, std::move(m_data[i]));
      alloc_traits::destroy(m_alloc, m_data + i);
    }
    if (!is_inline())
      alloc_traits::deallocate(m_alloc, m_data, m_capacity);
    m_data = new_data;
    m_capacity = new_cap;
  }

  void steal_or_move(small_vector &other) noexcept(
      std::is_nothrow_move_constructible_v<T>) {
    if (other.is_inline()) {
      for (usize i = 0; i < other.m_size; ++i) {
        alloc_traits::construct(m_alloc, m_data + i,
                                std::move(other.m_data[i]));
        alloc_traits::destroy(m_alloc, other.m_data + i);
      }
      m_size = other.m_size;
      other.m_size = 0;
    } else {
      m_data = other.m_data;
      m_capacity = other.m_capacity;
      m_size = other.m_size;
      other.m_data = other.inline_ptr();
      other.m_capacity = N;
      other.m_size = 0;
    }
  }

  void shift_right(T *pos, usize count) {
    T *old_end = m_data + m_size;
    const auto n = nx::cast<usize>(old_end - pos);
    if (n == 0)
      return;

    if (count >= n) {
      for (usize i = 0; i < n; ++i)
        alloc_traits::construct(m_alloc, pos + count + i, std::move(pos[i]));
      for (usize i = 0; i < n; ++i)
        alloc_traits::destroy(m_alloc, pos + i);
    } else {
      T *tail = old_end - count;
      for (usize i = 0; i < count; ++i)
        alloc_traits::construct(m_alloc, old_end + i, std::move(tail[i]));
      for (isize j = nx::cast<isize>(n - count) - 1; j >= 0; --j)
        pos[count + j] = std::move(pos[j]);
      for (usize i = 0; i < count; ++i)
        alloc_traits::destroy(m_alloc, pos + i);
    }
  }
};

template <typename T, usize N, typename A>
void swap(small_vector<T, N, A> &a,
          small_vector<T, N, A> &b) noexcept(noexcept(a.swap(b))) {
  a.swap(b);
}

template <typename T, usize N, typename A, typename U>
usize erase(small_vector<T, N, A> &c, const U &value) {
  auto it = std::remove(c.begin(), c.end(), value);
  const auto n = nx::cast<usize>(c.end() - it);
  c.erase(it, c.end());
  return n;
}

template <typename T, usize N, typename A, typename Pred>
usize erase_if(small_vector<T, N, A> &c, Pred pred) {
  auto it = std::remove_if(c.begin(), c.end(), std::move(pred));
  const auto n = nx::cast<usize>(c.end() - it);
  c.erase(it, c.end());
  return n;
}

} // namespace nx