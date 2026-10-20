#pragma once

#include <algorithm>
#include <compare>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>

#include "core/foundation/core/foundation.h"

#include <cassert>

namespace nx {

/// A vector with N elements of inline storage, like C++26's inplace_vector:
/// growing past N throws std::bad_alloc in every build, and try_emplace_back /
/// try_push_back return nullptr instead.
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
    guarded([&] { resize(count); });
  }

  fixed_vector(const usize count, const T &value) {
    guarded([&] { assign(count, value); });
  }

  template <std::input_iterator It> fixed_vector(It first, It last) {
    guarded([&] { assign(first, last); });
  }

  fixed_vector(std::initializer_list<T> il) {
    guarded([&] { assign(il.begin(), il.end()); });
  }

  explicit fixed_vector(std::span<const T> sp) {
    guarded([&] { assign(sp.begin(), sp.end()); });
  }

  fixed_vector(const fixed_vector &other) {
    guarded([&] { append(other.begin(), other.end()); });
  }

  fixed_vector(fixed_vector &&other) noexcept(
      std::is_nothrow_move_constructible_v<T>) {
    guarded([&] {
      for (usize i = 0; i < other.m_size; ++i) {
        new (ptr() + i) T(std::move(other.ptr()[i]));
        ++m_size;
      }
    });
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
      for (usize i = 0; i < other.m_size; ++i) {
        new (ptr() + i) T(std::move(other.ptr()[i]));
        ++m_size;
      }
      other.clear();
    }
    return *this;
  }

  fixed_vector &operator=(std::initializer_list<T> il) {
    assign(il.begin(), il.end());
    return *this;
  }

  void assign(const usize count, const T &value) {
    require_room(count, 0);
    if (aliases(value)) {
      const T copy(value);
      assign(count, copy);
      return;
    }
    clear();
    append_n(count, value);
  }

  template <std::input_iterator It> void assign(It first, It last) {
    if constexpr (std::random_access_iterator<It>)
      require_room(cast<usize>(std::distance(first, last)), 0);
    clear();
    append(first, last);
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
  [[nodiscard]] bool full() const noexcept { return m_size == N; }
  [[nodiscard]] usize size() const noexcept { return m_size; }
  [[nodiscard]] usize max_size() const noexcept { return N; }
  [[nodiscard]] usize capacity() const noexcept { return N; }

  void reserve(const usize n) const { require_room(n, 0); }
  void shrink_to_fit() const noexcept {}

  void clear() noexcept {
    for (usize i = 0; i < m_size; ++i)
      ptr()[i].~T();
    m_size = 0;
  }

  void push_back(const T &v) { emplace_back(v); }
  void push_back(T &&v) { emplace_back(std::move(v)); }

  template <typename... Args> reference emplace_back(Args &&...args) {
    require_room(1);
    new (ptr() + m_size) T(std::forward<Args>(args)...);
    return ptr()[m_size++];
  }

  template <typename... Args> pointer try_emplace_back(Args &&...args) {
    if (full())
      return nullptr;
    new (ptr() + m_size) T(std::forward<Args>(args)...);
    return ptr() + m_size++;
  }
  pointer try_push_back(const T &v) { return try_emplace_back(v); }
  pointer try_push_back(T &&v) { return try_emplace_back(std::move(v)); }

  void pop_back() noexcept {
    assert(!empty());
    ptr()[--m_size].~T();
  }

  // Inserting appends and rotates into place, so a throwing constructor never
  // leaves a destroyed slot inside the vector, and an argument that refers to
  // one of its elements is read before anything moves.
  template <typename... Args>
  iterator emplace(const_iterator pos, Args &&...args) {
    const usize off = offset_of(pos);
    emplace_back(std::forward<Args>(args)...);
    std::rotate(ptr() + off, ptr() + m_size - 1, ptr() + m_size);
    return ptr() + off;
  }

  iterator insert(const_iterator pos, const T &v) { return emplace(pos, v); }
  iterator insert(const_iterator pos, T &&v) {
    return emplace(pos, std::move(v));
  }

  iterator insert(const_iterator pos, const usize count, const T &v) {
    const usize off = offset_of(pos);
    require_room(count);
    const usize tail = m_size;
    append_n(count, v);
    std::rotate(ptr() + off, ptr() + tail, ptr() + m_size);
    return ptr() + off;
  }

  /// [first, last) must not be a range of this vector, as with std::vector.
  template <std::input_iterator It>
  iterator insert(const_iterator pos, It first, It last) {
    const usize off = offset_of(pos);
    if constexpr (std::random_access_iterator<It>)
      require_room(cast<usize>(std::distance(first, last)));
    const usize tail = m_size;
    append(first, last);
    std::rotate(ptr() + off, ptr() + tail, ptr() + m_size);
    return ptr() + off;
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
    require_room(count, 0);
    if (count <= m_size) {
      truncate(count);
      return;
    }
    const usize start = m_size;
    try {
      for (; m_size < count; ++m_size)
        new (ptr() + m_size) T();
    } catch (...) {
      truncate(start);
      throw;
    }
  }

  void resize(const usize count, const T &value) {
    require_room(count, 0);
    if (count <= m_size)
      truncate(count);
    else
      append_n(count - m_size, value);
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

  // Throws when @p extra more elements, on top of @p base, would not fit.
  void require_room(const usize extra) const { require_room(extra, m_size); }
  static void require_room(const usize extra, const usize base) {
    if (extra > N - base) [[unlikely]]
      throw std::bad_alloc{};
  }

  [[nodiscard]] usize offset_of(const const_iterator pos) const noexcept {
    assert(pos >= ptr() && pos <= ptr() + m_size);
    return nx::cast<usize>(pos - ptr());
  }

  [[nodiscard]] bool aliases(const T &value) const noexcept {
    const T *const p = std::addressof(value);
    return std::less_equal<const T *>{}(ptr(), p) &&
           std::less<const T *>{}(p, ptr() + m_size);
  }

  template <typename F> void guarded(F &&build) {
    try {
      build();
    } catch (...) {
      clear();
      throw;
    }
  }

  void truncate(const usize count) noexcept {
    for (usize i = count; i < m_size; ++i)
      ptr()[i].~T();
    m_size = count;
  }

  void append_n(const usize count, const T &value) {
    require_room(count);
    const usize start = m_size;
    try {
      for (usize i = 0; i < count; ++i) {
        new (ptr() + m_size) T(value);
        ++m_size;
      }
    } catch (...) {
      truncate(start);
      throw;
    }
  }

  template <typename It> void append(It first, It last) {
    const usize start = m_size;
    try {
      for (; first != last; ++first)
        emplace_back(*first);
    } catch (...) {
      truncate(start);
      throw;
    }
  }
};

/// Vectors of one element type compare whatever their capacities.
template <typename T, usize N, usize M>
  requires(N != M)
[[nodiscard]] bool operator==(const fixed_vector<T, N> &a,
                              const fixed_vector<T, M> &b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}
template <typename T, usize N, usize M>
  requires(N != M && std::three_way_comparable<T>)
[[nodiscard]] auto operator<=>(const fixed_vector<T, N> &a,
                               const fixed_vector<T, M> &b) {
  return std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(),
                                                b.end());
}

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
