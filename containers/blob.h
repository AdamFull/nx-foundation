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
  explicit blob(const Alloc &a) noexcept : alloc_(a) {}

  explicit blob(usize count, const Alloc &a = Alloc()) : alloc_(a) {
    data_ = allocate_n(count);
    usize i = 0;
    try {
      for (; i < count; ++i)
        ATraits::construct(alloc_, data_ + i);
    } catch (...) {
      destroy_range(data_, i);
      deallocate_n(data_, count);
      data_ = nullptr;
      throw;
    }
    size_ = count;
  }

  blob(usize count, const T &value, const Alloc &a = Alloc()) : alloc_(a) {
    data_ = allocate_n(count);
    usize i = 0;
    try {
      for (; i < count; ++i)
        ATraits::construct(alloc_, data_ + i, value);
    } catch (...) {
      destroy_range(data_, i);
      deallocate_n(data_, count);
      data_ = nullptr;
      throw;
    }
    size_ = count;
  }

  template <std::input_iterator It>
  blob(It first, It last, const Alloc &a = Alloc()) : alloc_(a) {
    if constexpr (std::forward_iterator<It>) {
      const auto n = nx::cast<usize>(std::distance(first, last));
      data_ = allocate_n(n);
      usize i = 0;
      try {
        for (; first != last; ++first, ++i)
          ATraits::construct(alloc_, data_ + i, *first);
      } catch (...) {
        destroy_range(data_, i);
        deallocate_n(data_, n);
        data_ = nullptr;
        throw;
      }
      size_ = n;
    } else {
      // Single-pass source: buffer first, then take a single fixed allocation.
      vector<T, Alloc> tmp(alloc_);
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
      : alloc_(ATraits::select_on_container_copy_construction(o.alloc_)) {
    assign_copy_from(o.data_, o.size_);
  }
  blob(const blob &o, const Alloc &a) : alloc_(a) {
    assign_copy_from(o.data_, o.size_);
  }

  blob(blob &&o) noexcept : alloc_(std::move(o.alloc_)) { steal(o); }
  blob(blob &&o, const Alloc &a) : alloc_(a) {
    if (alloc_ == o.alloc_)
      steal(o);
    else {
      assign_move_from(o.data_, o.size_);
      o.free_all();
    }
  }

  ~blob() { free_all(); }

  blob &operator=(const blob &o) {
    if (this == &o)
      return *this;
    if constexpr (ATraits::propagate_on_container_copy_assignment::value) {
      if (alloc_ != o.alloc_)
        free_all();
      alloc_ = o.alloc_;
    }
    assign_copy_from(o.data_, o.size_);
    return *this;
  }

  blob &operator=(blob &&o) noexcept(
      ATraits::propagate_on_container_move_assignment::value ||
      ATraits::is_always_equal::value) {
    if (this == &o)
      return *this;
    if constexpr (ATraits::propagate_on_container_move_assignment::value) {
      free_all();
      alloc_ = std::move(o.alloc_);
      steal(o);
    } else if (alloc_ == o.alloc_) {
      free_all();
      steal(o);
    } else {
      assign_move_from(o.data_, o.size_);
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
    b.data_ = data;
    b.size_ = size;
    return b;
  }

  struct owned {
    T *data;
    usize size;
  };

  /// Releases ownership of the buffer to the caller, leaving the blob empty.
  [[nodiscard]] owned release() noexcept {
    owned r{data_, size_};
    data_ = nullptr;
    size_ = 0;
    return r;
  }

  [[nodiscard]] Alloc get_allocator() const noexcept { return alloc_; }

  [[nodiscard]] reference at(usize i) {
    if (i >= size_)
      throw std::out_of_range("nx::blob::at");
    return data_[i];
  }
  [[nodiscard]] const_reference at(usize i) const {
    if (i >= size_)
      throw std::out_of_range("nx::blob::at");
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

  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] usize size() const noexcept { return size_; }
  [[nodiscard]] usize bytes_size() const noexcept { return size_ * sizeof(T); }

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

  [[nodiscard]] vector<T, Alloc> to_vector() const {
    return vector<T, Alloc>(data_, data_ + size_, alloc_);
  }

  operator std::span<const T>() const noexcept { return {data_, size_}; }
  explicit operator std::span<T>() noexcept { return {data_, size_}; }

  void swap(blob &o) noexcept {
    using std::swap;
    if constexpr (ATraits::propagate_on_container_swap::value)
      swap(alloc_, o.alloc_);
    swap(data_, o.data_);
    swap(size_, o.size_);
  }

  [[nodiscard]] friend bool operator==(const blob &a, const blob &b) {
    return a.size_ == b.size_ && std::equal(a.begin(), a.end(), b.begin());
  }
  [[nodiscard]] friend auto operator<=>(const blob &a, const blob &b)
    requires std::three_way_comparable<T>
  {
    return std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(),
                                                  b.end());
  }

private:
  NX_NO_UNIQUE_ADDRESS Alloc alloc_;
  T *data_ = nullptr;
  usize size_ = 0;

  T *allocate_n(usize n) { return n ? ATraits::allocate(alloc_, n) : nullptr; }
  void deallocate_n(T *p, usize n) noexcept {
    if (p)
      ATraits::deallocate(alloc_, p, n);
  }
  void destroy_range(T *p, usize n) noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>)
      for (usize i = 0; i < n; ++i)
        ATraits::destroy(alloc_, p + i);
  }

  void free_all() noexcept {
    destroy_range(data_, size_);
    deallocate_n(data_, size_);
    data_ = nullptr;
    size_ = 0;
  }

  void steal(blob &o) noexcept {
    data_ = o.data_;
    size_ = o.size_;
    o.data_ = nullptr;
    o.size_ = 0;
  }

  void assign_copy_from(const T *src, usize n) {
    free_all();
    if (n == 0)
      return;
    T *nd = allocate_n(n);
    usize i = 0;
    try {
      for (; i < n; ++i)
        ATraits::construct(alloc_, nd + i, src[i]);
    } catch (...) {
      destroy_range(nd, i);
      deallocate_n(nd, n);
      throw;
    }
    data_ = nd;
    size_ = n;
  }

  void assign_move_from(T *src, usize n) {
    free_all();
    if (n == 0)
      return;
    T *nd = allocate_n(n);
    usize i = 0;
    try {
      for (; i < n; ++i)
        ATraits::construct(alloc_, nd + i, std::move(src[i]));
    } catch (...) {
      destroy_range(nd, i);
      deallocate_n(nd, n);
      throw;
    }
    data_ = nd;
    size_ = n;
  }

  template <typename, typename> friend class blob;
};

template <typename T, typename Alloc>
void swap(blob<T, Alloc> &a, blob<T, Alloc> &b) noexcept {
  a.swap(b);
}

} // namespace nx