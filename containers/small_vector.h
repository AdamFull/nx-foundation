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
 * Behaves like std::vector but avoids the heap while the size stays within @p N;
 * it spills to an allocation only when it grows beyond the inline buffer. Good
 * for collections that are usually small but occasionally large.
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

  explicit small_vector(const Alloc &alloc) noexcept : alloc_(alloc) {}

  explicit small_vector(const usize count, const Alloc &alloc = Alloc())
      : alloc_(alloc) {
    resize(count);
  }

  small_vector(const usize count, const T &value, const Alloc &alloc = Alloc())
      : alloc_(alloc) {
    assign(count, value);
  }

  template <std::input_iterator It>
  small_vector(It first, It last, const Alloc &alloc = Alloc())
      : alloc_(alloc) {
    assign(first, last);
  }

  small_vector(std::initializer_list<T> il, const Alloc &alloc = Alloc())
      : alloc_(alloc) {
    assign(il.begin(), il.end());
  }

  explicit small_vector(std::span<const T> sp, const Alloc &alloc = Alloc())
      : alloc_(alloc) {
    assign(sp.begin(), sp.end());
  }

  small_vector(const small_vector &other)
      : alloc_(
            alloc_traits::select_on_container_copy_construction(other.alloc_)) {
    reserve(other.size_);
    for (usize i = 0; i < other.size_; ++i)
      alloc_traits::construct(alloc_, data_ + i, other.data_[i]);
    size_ = other.size_;
  }

  small_vector(const small_vector &other, const Alloc &alloc) : alloc_(alloc) {
    reserve(other.size_);
    for (usize i = 0; i < other.size_; ++i)
      alloc_traits::construct(alloc_, data_ + i, other.data_[i]);
    size_ = other.size_;
  }

  small_vector(small_vector &&other) noexcept(
      std::is_nothrow_move_constructible_v<T>)
      : alloc_(std::move(other.alloc_)) {
    steal_or_move(other);
  }

  small_vector(small_vector &&other, const Alloc &alloc) noexcept(
      std::is_nothrow_move_constructible_v<T>)
      : alloc_(alloc) {
    if (alloc_ == other.alloc_)
      steal_or_move(other);
    else {
      reserve(other.size_);
      for (usize i = 0; i < other.size_; ++i) {
        alloc_traits::construct(alloc_, data_ + i, std::move(other.data_[i]));
        alloc_traits::destroy(alloc_, other.data_ + i);
      }
      size_ = other.size_;
      other.size_ = 0;
    }
  }

  ~small_vector() noexcept { release(); }

  small_vector &operator=(const small_vector &other) {
    if (this == &other)
      return *this;

    if constexpr (alloc_traits::propagate_on_container_copy_assignment::value) {
      if (alloc_ != other.alloc_) {
        release();
        alloc_ = other.alloc_;
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
      alloc_ = std::move(other.alloc_);
      steal_or_move(other);
    } else {
      if (alloc_ == other.alloc_)
        steal_or_move(other);
      else {
        if (other.size_ > capacity_)
          reallocate(other.size_);
        for (usize i = 0; i < other.size_; ++i) {
          alloc_traits::construct(alloc_, data_ + i, std::move(other.data_[i]));
          alloc_traits::destroy(alloc_, other.data_ + i);
        }
        size_ = other.size_;
        other.size_ = 0;
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
    if (count > capacity_)
      reallocate(count);

    for (usize i = 0; i < count; ++i)
      alloc_traits::construct(alloc_, data_ + i, value);
    size_ = count;
  }

  template <std::input_iterator It> void assign(It first, It last) {
    clear();
    if constexpr (std::random_access_iterator<It>) {
      if (usize count = cast<usize>(std::distance(first, last));
          count > capacity_)
        reallocate(count);
    }
    for (; first != last; ++first)
      emplace_back(*first);
  }

  void assign(std::initializer_list<T> il) { assign(il.begin(), il.end()); }

  [[nodiscard]] reference at(usize i) {
    if (i >= size_)
      throw std::out_of_range("small_vector::at");
    return data_[i];
  }
  [[nodiscard]] const_reference at(usize i) const {
    if (i >= size_)
      throw std::out_of_range("small_vector::at");
    return data_[i];
  }

  [[nodiscard]] reference operator[](usize i) noexcept {
    assert(i < size_);
    return data_[i];
  }
  [[nodiscard]] const_reference operator[](usize i) const noexcept {
    assert(i < size_);
    return data_[i];
  }
  [[nodiscard]] reference front() noexcept {
    assert(!empty());
    return data_[0];
  }
  [[nodiscard]] const_reference front() const noexcept {
    assert(!empty());
    return data_[0];
  }
  [[nodiscard]] reference back() noexcept {
    assert(!empty());
    return data_[size_ - 1];
  }
  [[nodiscard]] const_reference back() const noexcept {
    assert(!empty());
    return data_[size_ - 1];
  }
  [[nodiscard]] pointer data() noexcept { return data_; }
  [[nodiscard]] const_pointer data() const noexcept { return data_; }

  [[nodiscard]] iterator begin() noexcept { return data_; }
  [[nodiscard]] iterator end() noexcept { return data_ + size_; }
  [[nodiscard]] const_iterator begin() const noexcept { return data_; }
  [[nodiscard]] const_iterator end() const noexcept { return data_ + size_; }
  [[nodiscard]] const_iterator cbegin() const noexcept { return data_; }
  [[nodiscard]] const_iterator cend() const noexcept { return data_ + size_; }
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

  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] usize size() const noexcept { return size_; }
  [[nodiscard]] usize max_size() const noexcept {
    return alloc_traits::max_size(alloc_);
  }
  [[nodiscard]] usize capacity() const noexcept { return capacity_; }

  void reserve(usize n) {
    if (n > capacity_)
      reallocate(n);
  }

  void shrink_to_fit() {
    if (is_inline())
      return;

    if (size_ <= N) {
      T *old_data = data_;
      usize old_cap = capacity_;
      data_ = inline_ptr();
      capacity_ = N;
      for (usize i = 0; i < size_; ++i) {
        alloc_traits::construct(alloc_, data_ + i, std::move(old_data[i]));
        alloc_traits::destroy(alloc_, old_data + i);
      }
      alloc_traits::deallocate(alloc_, old_data, old_cap);
    } else if (size_ < capacity_)
      reallocate(size_);
  }

  void clear() noexcept {
    for (usize i = 0; i < size_; ++i)
      alloc_traits::destroy(alloc_, data_ + i);
    size_ = 0;
  }

  void push_back(const T &v) { emplace_back(v); }
  void push_back(T &&v) { emplace_back(std::move(v)); }

  template <typename... Args> reference emplace_back(Args &&...args) {
    if (size_ == capacity_)
      grow();
    alloc_traits::construct(alloc_, data_ + size_, std::forward<Args>(args)...);
    return data_[size_++];
  }

  void pop_back() noexcept {
    assert(!empty());
    alloc_traits::destroy(alloc_, data_ + --size_);
  }

  template <typename... Args>
  iterator emplace(const_iterator pos, Args &&...args) {
    usize off = nx::cast<usize>(pos - data_);
    ensure_space(1);
    T *p = data_ + off;
    shift_right(p, 1);
    alloc_traits::construct(alloc_, p, std::forward<Args>(args)...);
    ++size_;
    return p;
  }

  iterator insert(const_iterator pos, const T &v) { return emplace(pos, v); }
  iterator insert(const_iterator pos, T &&v) {
    return emplace(pos, std::move(v));
  }

  iterator insert(const_iterator pos, usize count, const T &v) {
    if (count == 0)
      return const_cast<iterator>(pos);

    usize off = nx::cast<usize>(pos - data_);
    ensure_space(count);
    T *p = data_ + off;
    shift_right(p, count);
    for (usize i = 0; i < count; ++i)
      alloc_traits::construct(alloc_, p + i, v);

    size_ += count;
    return p;
  }

  template <std::input_iterator It>
  iterator insert(const_iterator pos, It first, It last) {
    usize off = nx::cast<usize>(pos - data_);
    if constexpr (std::random_access_iterator<It>) {
      const usize count = nx::cast<usize>(std::distance(first, last));
      if (count == 0)
        return data_ + off;

      ensure_space(count);
      T *p = data_ + off;
      shift_right(p, count);
      for (usize i = 0; first != last; ++first, ++i)
        alloc_traits::construct(alloc_, p + i, *first);

      size_ += count;
      return p;
    } else {
      usize tail = size_;
      for (; first != last; ++first)
        emplace_back(*first);
      std::rotate(data_ + off, data_ + tail, data_ + size_);
      return data_ + off;
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

    T *new_end = std::move(l, data_ + size_, f);
    for (T *p = new_end; p != data_ + size_; ++p)
      alloc_traits::destroy(alloc_, p);
    size_ -= n;
    return f;
  }

  void resize(const usize count) {
    if (count < size_) {
      for (usize i = count; i < size_; ++i)
        alloc_traits::destroy(alloc_, data_ + i);
    } else if (count > size_) {
      if (count > capacity_)
        reallocate(count);
      for (usize i = size_; i < count; ++i)
        alloc_traits::construct(alloc_, data_ + i);
    }
    size_ = count;
  }

  void resize(const usize count, const T &value) {
    if (count < size_) {
      for (usize i = count; i < size_; ++i)
        alloc_traits::destroy(alloc_, data_ + i);
    } else if (count > size_) {
      if (count > capacity_)
        reallocate(count);
      for (usize i = size_; i < count; ++i)
        alloc_traits::construct(alloc_, data_ + i, value);
    }
    size_ = count;
  }

  void swap(small_vector &other) noexcept {
    if (this == &other)
      return;

    if (!is_inline() && !other.is_inline()) {
      if constexpr (alloc_traits::propagate_on_container_swap::value) {
        using std::swap;
        swap(alloc_, other.alloc_);
      } else {
        assert(alloc_ == other.alloc_ &&
               "small_vector::swap: non-propagating unequal allocators");
      }
      std::swap(data_, other.data_);
      std::swap(size_, other.size_);
      std::swap(capacity_, other.capacity_);
    } else {
      small_vector tmp(std::move(*this));
      *this = std::move(other);
      other = std::move(tmp);
    }
  }

  operator std::span<const T>() const noexcept { return {data_, size_}; }
  explicit operator std::span<T>() noexcept { return {data_, size_}; }

  [[nodiscard]] friend bool operator==(const small_vector &a,
                                       const small_vector &b) {
    return a.size_ == b.size_ && std::equal(a.begin(), a.end(), b.begin());
  }

  [[nodiscard]] friend auto operator<=>(const small_vector &a,
                                        const small_vector &b)
    requires std::three_way_comparable<T>
  {
    return std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(),
                                                  b.end());
  }

  [[nodiscard]] allocator_type get_allocator() const noexcept { return alloc_; }

private:
  alignas(T) std::byte inline_storage_[N * sizeof(T)];

  NX_NO_UNIQUE_ADDRESS Alloc alloc_;

  T *data_ = reinterpret_cast<T *>(inline_storage_);
  usize size_ = 0;
  usize capacity_ = N;

  T *inline_ptr() noexcept { return reinterpret_cast<T *>(inline_storage_); }
  const T *inline_ptr() const noexcept {
    return reinterpret_cast<const T *>(inline_storage_);
  }
  bool is_inline() const noexcept { return data_ == inline_ptr(); }

  void release() noexcept {
    clear();
    if (!is_inline()) {
      alloc_traits::deallocate(alloc_, data_, capacity_);
      data_ = inline_ptr();
      capacity_ = N;
    }
  }

  void grow() { reallocate(capacity_ * 2); }

  void ensure_space(usize extra) {
    if (size_ + extra <= capacity_)
      return;
    usize new_cap = capacity_;
    while (new_cap < size_ + extra)
      new_cap *= 2;
    reallocate(new_cap);
  }

  void reallocate(usize new_cap) {
    T *new_data = alloc_traits::allocate(alloc_, new_cap);
    for (usize i = 0; i < size_; ++i) {
      alloc_traits::construct(alloc_, new_data + i, std::move(data_[i]));
      alloc_traits::destroy(alloc_, data_ + i);
    }
    if (!is_inline())
      alloc_traits::deallocate(alloc_, data_, capacity_);
    data_ = new_data;
    capacity_ = new_cap;
  }

  void steal_or_move(small_vector &other) noexcept(
      std::is_nothrow_move_constructible_v<T>) {
    if (other.is_inline()) {
      for (usize i = 0; i < other.size_; ++i) {
        alloc_traits::construct(alloc_, data_ + i, std::move(other.data_[i]));
        alloc_traits::destroy(alloc_, other.data_ + i);
      }
      size_ = other.size_;
      other.size_ = 0;
    } else {
      data_ = other.data_;
      capacity_ = other.capacity_;
      size_ = other.size_;
      other.data_ = other.inline_ptr();
      other.capacity_ = N;
      other.size_ = 0;
    }
  }

  void shift_right(T *pos, usize count) {
    T *old_end = data_ + size_;
    const auto n = nx::cast<usize>(old_end - pos);
    if (n == 0)
      return;

    if (count >= n) {
      for (usize i = 0; i < n; ++i)
        alloc_traits::construct(alloc_, pos + count + i, std::move(pos[i]));
      for (usize i = 0; i < n; ++i)
        alloc_traits::destroy(alloc_, pos + i);
    } else {
      T *tail = old_end - count;
      for (usize i = 0; i < count; ++i)
        alloc_traits::construct(alloc_, old_end + i, std::move(tail[i]));
      for (isize j = nx::cast<isize>(n - count) - 1; j >= 0; --j)
        pos[count + j] = std::move(pos[j]);
      for (usize i = 0; i < count; ++i)
        alloc_traits::destroy(alloc_, pos + i);
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