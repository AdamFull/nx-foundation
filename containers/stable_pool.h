#pragma once

/**
 * @file stable_pool.h
 * @brief Pointer-stable handle-keyed pool (nx::stable_pool).
 */

#include <bit>
#include <iterator>
#include <stdexcept>
#include <type_traits>

#include "foundation/core/foundation.h"
#include "foundation/core/handle.h"

#include <cassert>

namespace nx {

/**
 * @brief Handle-keyed pool whose element addresses stay valid until erased.
 *
 * Elements live in fixed-size pages that are never reallocated, so a T& or T*
 * remains valid until that specific element is erased. Choose this over
 * nx::slot_map when other systems hold raw references into the pool. Iteration
 * walks the pages and skips empty slots, so it is less cache-dense than a slot
 * map.
 *
 * @tparam T      Element type.
 * @tparam Handle Handle type used as the key (carries index and version).
 * @tparam Alloc  Allocator type.
 */
template <typename T, typename Handle = handle32, typename Alloc = Allocator<T>>
class stable_pool {
  using repr = Handle::repr_type;
  using alloc_traits = std::allocator_traits<Alloc>;

  static constexpr usize page_size = 256;
  static_assert(is_pow2(nx::cast<usize>(page_size)),
                "page_size must be a power of two");
  static constexpr usize page_shift = std::countr_zero(page_size);
  static constexpr usize page_mask = page_size - 1;

  enum class state : u8 { free, occupied, retired };

  struct slot {
    alignas(T) std::byte storage[sizeof(T)];
    repr version;
    repr next_free;
    state st;
  };

  using slot_alloc = alloc_traits::template rebind_alloc<slot>;
  using slot_alloc_traits = std::allocator_traits<slot_alloc>;
  using page_ptr_alloc = alloc_traits::template rebind_alloc<slot *>;

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

  template <bool Const> class iterator_t {
  public:
    using value_type = T;
    using reference = std::conditional_t<Const, const T &, T &>;
    using pointer = std::conditional_t<Const, const T *, T *>;
    using difference_type = isize;
    using iterator_category = std::bidirectional_iterator_tag;

    iterator_t() = default;

    template <bool C = Const, std::enable_if_t<C, int> = 0>
    iterator_t(const iterator_t<false> &o) noexcept
        : pool_(o.pool_), index_(o.index_) {}

    reference operator*() const noexcept { return *pool_->value_ptr(index_); }
    pointer operator->() const noexcept { return pool_->value_ptr(index_); }

    [[nodiscard]] handle_type handle() const noexcept {
      return Handle::from_parts(nx::cast<repr>(index_),
                                pool_->slot_at(index_).version);
    }

    iterator_t &operator++() noexcept {
      index_ = pool_->next_occupied(index_ + 1);
      return *this;
    }
    iterator_t operator++(int) noexcept {
      iterator_t tmp = *this;
      ++*this;
      return tmp;
    }
    iterator_t &operator--() noexcept {
      index_ = pool_->prev_occupied(index_ - 1);
      return *this;
    }
    iterator_t operator--(int) noexcept {
      iterator_t tmp = *this;
      --*this;
      return tmp;
    }

    [[nodiscard]] friend bool operator==(const iterator_t &a,
                                         const iterator_t &b) noexcept {
      return a.index_ == b.index_;
    }
    [[nodiscard]] friend bool operator!=(const iterator_t &a,
                                         const iterator_t &b) noexcept {
      return a.index_ != b.index_;
    }

  private:
    friend class stable_pool;
    template <bool> friend class iterator_t;
    using pool_ptr =
        std::conditional_t<Const, const stable_pool *, stable_pool *>;

    iterator_t(pool_ptr p, usize i) noexcept : pool_(p), index_(i) {}

    pool_ptr pool_ = nullptr;
    usize index_ = 0;
  };

  using iterator = iterator_t<false>;
  using const_iterator = iterator_t<true>;

  stable_pool() noexcept(std::is_nothrow_default_constructible_v<Alloc>) {}

  explicit stable_pool(const Alloc &alloc) noexcept
      : alloc_(alloc), salloc_(alloc), pages_(page_ptr_alloc(alloc)) {}

  stable_pool(const stable_pool &other)
      : alloc_(
            alloc_traits::select_on_container_copy_construction(other.alloc_)),
        salloc_(alloc_), pages_(page_ptr_alloc(alloc_)) {
    copy_from(other);
  }

  stable_pool(stable_pool &&other) noexcept
      : alloc_(std::move(other.alloc_)), salloc_(std::move(other.salloc_)),
        pages_(std::move(other.pages_)), free_head_(other.free_head_),
        size_(other.size_), capacity_(other.capacity_) {
    other.free_head_ = Handle::null_index;
    other.size_ = 0;
    other.capacity_ = 0;
  }

  ~stable_pool() { destroy_all(); }

  stable_pool &operator=(const stable_pool &other) {
    if (this == &other)
      return *this;
    clear_and_release();
    if constexpr (alloc_traits::propagate_on_container_copy_assignment::value) {
      alloc_ = other.alloc_;
      salloc_ = slot_alloc(alloc_);
    }
    copy_from(other);
    return *this;
  }

  stable_pool &operator=(stable_pool &&other) noexcept {
    if (this == &other)
      return *this;
    clear_and_release();
    if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
      alloc_ = std::move(other.alloc_);
      salloc_ = std::move(other.salloc_);
    }
    pages_ = std::move(other.pages_);
    free_head_ = other.free_head_;
    size_ = other.size_;
    capacity_ = other.capacity_;
    other.free_head_ = Handle::null_index;
    other.size_ = 0;
    other.capacity_ = 0;
    return *this;
  }

  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] usize size() const noexcept { return size_; }
  [[nodiscard]] usize capacity() const noexcept { return capacity_; }
  [[nodiscard]] usize max_size() const noexcept { return Handle::max_index; }

  [[nodiscard]] bool contains(Handle h) const noexcept {
    const repr idx = h.index();
    if (idx >= capacity_)
      return false;
    const slot &s = slot_at(idx);
    return s.st == state::occupied && s.version == h.version();
  }

  [[nodiscard]] Handle insert(const T &value) { return emplace(value); }
  [[nodiscard]] Handle insert(T &&value) { return emplace(std::move(value)); }

  template <typename... Args> [[nodiscard]] Handle emplace(Args &&...args) {
    const repr idx = acquire_slot();
    slot &s = slot_at(idx);
    try {
      ::new (static_cast<void *>(s.storage)) T(std::forward<Args>(args)...);
    } catch (...) {
      s.next_free = free_head_;
      free_head_ = idx;
      throw;
    }
    s.st = state::occupied;
    ++size_;
    return Handle::from_parts(idx, s.version);
  }

  bool erase(Handle h) {
    if (!contains(h))
      return false;
    const repr idx = h.index();
    slot &s = slot_at(idx);
    value_ptr(idx)->~T();
    --size_;
    recycle(idx, s);
    return true;
  }

  [[nodiscard]] T *try_get(Handle h) noexcept {
    return contains(h) ? value_ptr(h.index()) : nullptr;
  }
  [[nodiscard]] const T *try_get(Handle h) const noexcept {
    return contains(h) ? value_ptr(h.index()) : nullptr;
  }

  [[nodiscard]] reference operator[](Handle h) noexcept {
    assert(contains(h));
    return *value_ptr(h.index());
  }
  [[nodiscard]] const_reference operator[](Handle h) const noexcept {
    assert(contains(h));
    return *value_ptr(h.index());
  }

  [[nodiscard]] reference at(Handle h) {
    if (!contains(h))
      throw std::out_of_range("stable_pool::at");
    return *value_ptr(h.index());
  }
  [[nodiscard]] const_reference at(Handle h) const {
    if (!contains(h))
      throw std::out_of_range("stable_pool::at");
    return *value_ptr(h.index());
  }

  void reserve(const usize n) {
    while (capacity_ < n)
      add_page();
  }

  void clear() noexcept {
    for (usize idx = 0; idx < capacity_; ++idx) {
      slot &s = slot_at(idx);
      if (s.st == state::occupied) {
        value_ptr(idx)->~T();
        s.st = state::free;
        if (s.version < Handle::max_version)
          ++s.version;
        else
          s.st = state::retired;
      }
    }
    rebuild_free_list();
    size_ = 0;
  }

  [[nodiscard]] iterator begin() noexcept {
    return iterator(this, next_occupied(0));
  }
  [[nodiscard]] iterator end() noexcept { return iterator(this, capacity_); }
  [[nodiscard]] const_iterator begin() const noexcept {
    return const_iterator(this, next_occupied(0));
  }
  [[nodiscard]] const_iterator end() const noexcept {
    return const_iterator(this, capacity_);
  }
  [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
  [[nodiscard]] const_iterator cend() const noexcept { return end(); }

  [[nodiscard]] allocator_type get_allocator() const noexcept { return alloc_; }

private:
  template <bool> friend class iterator_t;

  slot &slot_at(const usize idx) noexcept {
    return pages_[idx >> page_shift][idx & page_mask];
  }
  const slot &slot_at(const usize idx) const noexcept {
    return pages_[idx >> page_shift][idx & page_mask];
  }
  T *value_ptr(const usize idx) noexcept {
    return std::launder(reinterpret_cast<T *>(slot_at(idx).storage));
  }
  const T *value_ptr(const usize idx) const noexcept {
    return std::launder(reinterpret_cast<const T *>(slot_at(idx).storage));
  }

  usize next_occupied(const usize from) const noexcept {
    for (usize i = from; i < capacity_; ++i)
      if (slot_at(i).st == state::occupied)
        return i;
    return capacity_;
  }
  usize prev_occupied(const usize from) const noexcept {
    for (usize i = from; i + 1 > 0; --i) {
      if (slot_at(i).st == state::occupied)
        return i;
      if (i == 0)
        break;
    }
    return capacity_;
  }

  void add_page() {
    const usize base = capacity_;
    assert(base < Handle::null_index && "stable_pool: index space exhausted");
    slot *page = slot_alloc_traits::allocate(salloc_, page_size);
    for (usize i = 0; i < page_size; ++i) {
      slot &s = page[i];
      s.version = 0;
      s.st = state::free;
      s.next_free =
          (i + 1 < page_size) ? nx::cast<repr>(base + i + 1) : free_head_;
    }
    pages_.push_back(page);
    capacity_ += page_size;
    free_head_ = nx::cast<repr>(base);
  }

  repr acquire_slot() {
    if (free_head_ == Handle::null_index)
      add_page();
    const repr idx = free_head_;
    free_head_ = slot_at(idx).next_free;
    return idx;
  }

  void recycle(repr idx, slot &s) noexcept {
    if (s.version < Handle::max_version) {
      ++s.version;
      s.st = state::free;
      s.next_free = free_head_;
      free_head_ = idx;
    } else {
      s.st = state::retired;
    }
  }

  void rebuild_free_list() noexcept {
    free_head_ = Handle::null_index;
    for (usize i = capacity_; i-- > 0;) {
      slot &s = slot_at(i);
      if (s.st == state::free) {
        s.next_free = free_head_;
        free_head_ = nx::cast<repr>(i);
      }
    }
  }

  void destroy_all() noexcept {
    for (usize idx = 0; idx < capacity_; ++idx) {
      slot &s = slot_at(idx);
      if (s.st == state::occupied)
        value_ptr(idx)->~T();
    }
    for (slot *page : pages_)
      slot_alloc_traits::deallocate(salloc_, page, page_size);
    pages_.clear();
  }

  void clear_and_release() noexcept {
    destroy_all();
    free_head_ = Handle::null_index;
    size_ = 0;
    capacity_ = 0;
  }

  void copy_from(const stable_pool &other) {
    reserve(other.capacity_);
    for (usize idx = 0; idx < other.capacity_; ++idx) {
      const slot &src = other.slot_at(idx);
      slot &dst = slot_at(idx);
      dst.version = src.version;
      dst.st = src.st;
      if (src.st == state::occupied)
        ::new (static_cast<void *>(dst.storage)) T(*other.value_ptr(idx));
    }
    rebuild_free_list();
    size_ = other.size_;
  }

  NX_NO_UNIQUE_ADDRESS Alloc alloc_;
  NX_NO_UNIQUE_ADDRESS slot_alloc salloc_;
  vector<slot *, page_ptr_alloc> pages_;
  repr free_head_ = Handle::null_index;
  usize size_ = 0;
  usize capacity_ = 0;
};

} // namespace nx