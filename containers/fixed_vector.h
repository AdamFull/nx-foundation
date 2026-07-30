#pragma once

/**
 * @file fixed_vector.h
 * @brief Inline fixed-capacity vector (nx::fixed_vector).
 */

#include <algorithm>
#include <compare>
#include <initializer_list>
#include <iterator>
#include <span>
#include <stdexcept>

#include "foundation/core/foundation.h"

#include <cassert>

namespace nx {

/**
 * @brief Vector with a compile-time capacity and no heap allocation.
 *
 * Stores up to @p N elements inline. The interface mirrors std::vector
 * (push_back, emplace_back, insert, erase, iterators, ...) but capacity is
 * fixed: growing past @p N is a programming error. Useful on hot paths and on
 * the stack.
 *
 * @tparam T Element type.
 * @tparam N Maximum number of elements.
 */
template <typename T, usize N> class fixed_vector {
  static_assert(N > 0, "fixed_vector requires N > 0");

public:
  using value_type = T;
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

  fixed_vector() noexcept {}

  explicit fixed_vector(const usize count) {
    assert(count <= N);
    for (; m_size < count; ++m_size)
      new (ptr() + m_size) T();
  }

  fixed_vector(const usize count, const T &value) {
    assert(count <= N);
    for (; m_size < count; ++m_size)
      new (ptr() + m_size) T(value);
  }

  template <std::input_iterator It> fixed_vector(It first, It last) {
    assign(first, last);
  }

  fixed_vector(std::initializer_list<T> il) { assign(il.begin(), il.end()); }

  explicit fixed_vector(std::span<const T> sp) { assign(sp.begin(), sp.end()); }

  fixed_vector(const fixed_vector &other) {
    for (usize i = 0; i < other.m_size; ++i)
      new (ptr() + i) T(other.ptr()[i]);
    m_size = other.m_size;
  }

  fixed_vector(fixed_vector &&other) noexcept(
      std::is_nothrow_move_constructible_v<T>) {
    for (usize i = 0; i < other.m_size; ++i)
      new (ptr() + i) T(std::move(other.ptr()[i]));
    m_size = other.m_size;
    other.clear();
  }

  ~fixed_vector() noexcept { clear(); }

  fixed_vector &operator=(const fixed_vector &other) {
    if (this != &other)
      assign(other.begin(), other.end());
    return *this;
  }

  fixed_vector &operator=(fixed_vector &&other) noexcept(
      std::is_nothrow_move_constructible_v<T>) {
    if (this != &other) {
      clear();
      for (usize i = 0; i < other.m_size; ++i)
        new (ptr() + i) T(std::move(other.ptr()[i]));
      m_size = other.m_size;
      other.clear();
    }
    return *this;
  }

  fixed_vector &operator=(std::initializer_list<T> il) {
    assign(il.begin(), il.end());
    return *this;
  }

  void assign(const usize count, const T &value) {
    assert(count <= N);
    clear();
    for (usize i = 0; i < count; ++i)
      new (ptr() + i) T(value);
    m_size = count;
  }

  template <std::input_iterator It> void assign(It first, It last) {
    clear();
    for (; first != last; ++first)
      emplace_back(*first);
  }

  void assign(std::initializer_list<T> il) { assign(il.begin(), il.end()); }

  [[nodiscard]] reference at(usize i) {
    if (i >= m_size)
      throw std::out_of_range("fixed_vector::at");
    return ptr()[i];
  }
  [[nodiscard]] const_reference at(usize i) const {
    if (i >= m_size)
      throw std::out_of_range("fixed_vector::at");
    return ptr()[i];
  }

  [[nodiscard]] reference operator[](usize i) noexcept {
    assert(i < m_size);
    return ptr()[i];
  }
  [[nodiscard]] const_reference operator[](usize i) const noexcept {
    assert(i < m_size);
    return ptr()[i];
  }
  [[nodiscard]] reference front() noexcept {
    assert(!empty());
    return ptr()[0];
  }
  [[nodiscard]] const_reference front() const noexcept {
    assert(!empty());
    return ptr()[0];
  }
  [[nodiscard]] reference back() noexcept {
    assert(!empty());
    return ptr()[m_size - 1];
  }
  [[nodiscard]] const_reference back() const noexcept {
    assert(!empty());
    return ptr()[m_size - 1];
  }
  [[nodiscard]] pointer data() noexcept { return ptr(); }
  [[nodiscard]] const_pointer data() const noexcept { return ptr(); }

  [[nodiscard]] iterator begin() noexcept { return ptr(); }
  [[nodiscard]] iterator end() noexcept { return ptr() + m_size; }
  [[nodiscard]] const_iterator begin() const noexcept { return ptr(); }
  [[nodiscard]] const_iterator end() const noexcept { return ptr() + m_size; }
  [[nodiscard]] const_iterator cbegin() const noexcept { return ptr(); }
  [[nodiscard]] const_iterator cend() const noexcept { return ptr() + m_size; }
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
  [[nodiscard]] usize max_size() const noexcept { return N; }
  [[nodiscard]] usize capacity() const noexcept { return N; }

  void reserve(usize n) const noexcept {
    assert(n <= N);
    (void)n;
  }
  void shrink_to_fit() const noexcept {}

  void clear() noexcept {
    for (usize i = 0; i < m_size; ++i)
      ptr()[i].~T();
    m_size = 0;
  }

  void push_back(const T &v) { emplace_back(v); }
  void push_back(T &&v) { emplace_back(std::move(v)); }

  template <typename... Args> reference emplace_back(Args &&...args) {
    assert(m_size < N && "fixed_vector: capacity exceeded");
    new (ptr() + m_size) T(std::forward<Args>(args)...);
    return ptr()[m_size++];
  }

  void pop_back() noexcept {
    assert(!empty());
    ptr()[--m_size].~T();
  }

  template <typename... Args>
  iterator emplace(const_iterator pos, Args &&...args) {
    assert(m_size < N && "fixed_vector: capacity exceeded");
    usize off = nx::cast<usize>(pos - ptr());
    T *p = ptr() + off;
    shift_right(p, ptr() + m_size, 1);
    new (p) T(std::forward<Args>(args)...);
    ++m_size;
    return p;
  }

  iterator insert(const_iterator pos, const T &v) { return emplace(pos, v); }
  iterator insert(const_iterator pos, T &&v) {
    return emplace(pos, std::move(v));
  }

  iterator insert(const_iterator pos, const usize count, const T &v) {
    if (count == 0)
      return const_cast<iterator>(pos);
    assert(m_size + count <= N && "fixed_vector: capacity exceeded");
    usize off = nx::cast<usize>(pos - ptr());
    T *p = ptr() + off;
    shift_right(p, ptr() + m_size, count);
    for (usize i = 0; i < count; ++i)
      new (p + i) T(v);
    m_size += count;
    return p;
  }

  template <std::input_iterator It>
  iterator insert(const_iterator pos, It first, It last) {
    usize off = nx::cast<usize>(pos - ptr());
    if constexpr (std::random_access_iterator<It>) {
      const usize count = nx::cast<usize>(std::distance(first, last));
      if (count == 0)
        return ptr() + off;
      assert(m_size + count <= N && "fixed_vector: capacity exceeded");
      T *p = ptr() + off;
      shift_right(p, ptr() + m_size, count);
      for (usize i = 0; first != last; ++first, ++i)
        new (p + i) T(*first);
      m_size += count;
      return p;
    } else {
      // Append to end then rotate into position — avoids heap, handles unknown
      // count.
      usize tail = m_size;
      for (; first != last; ++first)
        emplace_back(*first);
      std::rotate(ptr() + off, ptr() + tail, ptr() + m_size);
      return ptr() + off;
    }
  }

  iterator insert(const_iterator pos, std::initializer_list<T> il) {
    return insert(pos, il.begin(), il.end());
  }

  iterator erase(const_iterator pos) { return erase(pos, pos + 1); }

  iterator erase(const_iterator first, const_iterator last) {
    T *f = const_cast<T *>(first);
    T *l = const_cast<T *>(last);
    const usize n = nx::cast<usize>(l - f);
    if (n == 0)
      return f;
    T *new_end = std::move(l, ptr() + m_size, f);
    for (T *p = new_end; p != ptr() + m_size; ++p)
      p->~T();
    m_size -= n;
    return f;
  }

  void resize(const usize count) {
    assert(count <= N && "fixed_vector: capacity exceeded");
    if (count < m_size) {
      for (usize i = count; i < m_size; ++i)
        ptr()[i].~T();
    } else {
      for (usize i = m_size; i < count; ++i)
        new (ptr() + i) T();
    }
    m_size = count;
  }

  void resize(usize count, const T &value) {
    assert(count <= N && "fixed_vector: capacity exceeded");
    if (count < m_size) {
      for (usize i = count; i < m_size; ++i)
        ptr()[i].~T();
    } else {
      for (usize i = m_size; i < count; ++i)
        new (ptr() + i) T(value);
    }
    m_size = count;
  }

  void swap(fixed_vector &other) noexcept(std::is_nothrow_swappable_v<T>) {
    if (this == &other)
      return;
    const usize min_s = nx::min(m_size, other.m_size);
    for (usize i = 0; i < min_s; ++i)
      std::swap(ptr()[i], other.ptr()[i]);
    if (m_size > other.m_size) {
      for (usize i = other.m_size; i < m_size; ++i) {
        new (other.ptr() + i) T(std::move(ptr()[i]));
        ptr()[i].~T();
      }
    } else {
      for (usize i = m_size; i < other.m_size; ++i) {
        new (ptr() + i) T(std::move(other.ptr()[i]));
        other.ptr()[i].~T();
      }
    }
    std::swap(m_size, other.m_size);
  }

  operator std::span<const T>() const noexcept { return {ptr(), m_size}; }
  explicit operator std::span<T>() noexcept { return {ptr(), m_size}; }

  [[nodiscard]] friend bool operator==(const fixed_vector &a,
                                       const fixed_vector &b) {
    return a.m_size == b.m_size && std::equal(a.begin(), a.end(), b.begin());
  }

  [[nodiscard]] friend auto operator<=>(const fixed_vector &a,
                                        const fixed_vector &b)
    requires std::three_way_comparable<T>
  {
    return std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(),
                                                  b.end());
  }

private:
  alignas(T) std::byte m_storage[N * sizeof(T)];
  usize m_size = 0;

  T *ptr() noexcept { return reinterpret_cast<T *>(m_storage); }
  const T *ptr() const noexcept {
    return reinterpret_cast<const T *>(m_storage);
  }

  static void shift_right(T *pos, T *old_end, usize count) {
    usize n = nx::cast<usize>(old_end - pos);
    if (n == 0)
      return;
    if (count >= n) {
      for (usize i = 0; i < n; ++i)
        new (pos + count + i) T(std::move(pos[i]));
      for (usize i = 0; i < n; ++i)
        pos[i].~T();
    } else {
      T *tail = old_end - count;
      for (usize i = 0; i < count; ++i)
        new (old_end + i) T(std::move(tail[i]));
      for (isize j = nx::cast<isize>(n - count) - 1; j >= 0; --j)
        pos[count + j] = std::move(pos[j]);
      for (usize i = 0; i < count; ++i)
        pos[i].~T();
    }
  }
};

template <typename T, usize N>
void swap(fixed_vector<T, N> &a,
          fixed_vector<T, N> &b) noexcept(noexcept(a.swap(b))) {
  a.swap(b);
}

template <typename T, usize N, typename U>
usize erase(fixed_vector<T, N> &c, const U &value) {
  auto it = std::remove(c.begin(), c.end(), value);
  const usize n = nx::cast<usize>(c.end() - it);
  c.erase(it, c.end());
  return n;
}

template <typename T, usize N, typename Pred>
usize erase_if(fixed_vector<T, N> &c, Pred pred) {
  auto it = std::remove_if(c.begin(), c.end(), std::move(pred));
  const usize n = nx::cast<usize>(c.end() - it);
  c.erase(it, c.end());
  return n;
}

} // namespace nx