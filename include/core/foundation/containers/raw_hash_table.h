#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/core/hash.h"

#include <bit>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>

#if defined(NX_ARCH_X64) && !defined(NX_HASHMAP_FORCE_PORTABLE)
#include <emmintrin.h>
#define NX_HASHMAP_SSE2 1
#elif defined(NX_ARCH_AARCH64) && !defined(NX_HASHMAP_FORCE_PORTABLE)
#include <arm_neon.h>
#define NX_HASHMAP_NEON 1
#endif

namespace nx {
namespace detail {

enum class ctrl_t : i8 {
  empty = -128,
  deleted = -2,
  sentinel = -1,
};

constexpr bool ctrl_is_full(ctrl_t c) noexcept {
  return static_cast<i8>(c) >= 0;
}
constexpr bool ctrl_is_empty(ctrl_t c) noexcept { return c == ctrl_t::empty; }
constexpr bool ctrl_is_deleted(ctrl_t c) noexcept {
  return c == ctrl_t::deleted;
}
constexpr bool ctrl_is_empty_or_deleted(ctrl_t c) noexcept {
  return static_cast<i8>(c) < static_cast<i8>(ctrl_t::sentinel);
}

// Bitmask over the result of a group scan. Each match occupies one bit at
// position index*(1<<Shift)+(1<<Shift)-1, so iteration yields slot indices.
template <typename T, i32 Bits, i32 Shift = 0> class bit_mask {
public:
  explicit bit_mask(T mask) noexcept : m_mask(mask) {}

  bit_mask &operator++() noexcept {
    m_mask &= (m_mask - 1);
    return *this;
  }
  explicit operator bool() const noexcept { return m_mask != 0; }
  u32 operator*() const noexcept { return lowest_bit_set(); }

  [[nodiscard]] u32 lowest_bit_set() const noexcept {
    return static_cast<u32>(std::countr_zero(m_mask)) >> Shift;
  }
  [[nodiscard]] u32 trailing_zeros() const noexcept {
    return static_cast<u32>(std::countr_zero(m_mask)) >> Shift;
  }
  [[nodiscard]] u32 leading_zeros() const noexcept {
    constexpr i32 total = Bits << Shift;
    constexpr i32 extra = static_cast<i32>(sizeof(T) * 8) - total;
    return static_cast<u32>(
               std::countl_zero(static_cast<T>(m_mask << extra))) >>
           Shift;
  }

  [[nodiscard]] bit_mask begin() const noexcept { return *this; }
  [[nodiscard]] bit_mask end() const noexcept { return bit_mask(0); }
  friend bool operator!=(const bit_mask &a, const bit_mask &b) noexcept {
    return a.m_mask != b.m_mask;
  }

private:
  T m_mask;
};

#if NX_HASHMAP_SSE2

struct group {
  static constexpr usize WIDTH = 16;

  explicit group(const ctrl_t *pos) noexcept
      : ctrl(_mm_loadu_si128(reinterpret_cast<const __m128i *>(pos))) {}

  [[nodiscard]] bit_mask<u32, 16> match(u8 h) const noexcept {
    const __m128i dup = _mm_set1_epi8(static_cast<char>(h));
    return bit_mask<u32, 16>(
        static_cast<u32>(_mm_movemask_epi8(_mm_cmpeq_epi8(dup, ctrl))));
  }
  [[nodiscard]] bit_mask<u32, 16> match_empty() const noexcept {
    const __m128i e = _mm_set1_epi8(static_cast<char>(ctrl_t::empty));
    return bit_mask<u32, 16>(
        static_cast<u32>(_mm_movemask_epi8(_mm_cmpeq_epi8(e, ctrl))));
  }
  [[nodiscard]] bit_mask<u32, 16> match_empty_or_deleted() const noexcept {
    const __m128i s = _mm_set1_epi8(static_cast<char>(ctrl_t::sentinel));
    return bit_mask<u32, 16>(
        static_cast<u32>(_mm_movemask_epi8(_mm_cmpgt_epi8(s, ctrl))));
  }
  [[nodiscard]] u32 count_leading_empty_or_deleted() const noexcept {
    const __m128i s = _mm_set1_epi8(static_cast<char>(ctrl_t::sentinel));
    const u32 mask =
        static_cast<u32>(_mm_movemask_epi8(_mm_cmpgt_epi8(s, ctrl)));
    return static_cast<u32>(std::countr_zero(mask + 1));
  }

  __m128i ctrl;
};

#elif NX_HASHMAP_NEON

struct group {
  static constexpr usize WIDTH = 8;
  static constexpr u64 MSBS = 0x8080808080808080ull;

  explicit group(const ctrl_t *pos) noexcept
      : ctrl(vld1_u8(reinterpret_cast<const u8 *>(pos))) {}

  bit_mask<u64, 8, 3> match(u8 h) const noexcept {
    const uint8x8_t dup = vdup_n_u8(h);
    const uint8x8_t eq = vceq_u8(ctrl, dup);
    return bit_mask<u64, 8, 3>(vget_lane_u64(vreinterpret_u64_u8(eq), 0) &
                               MSBS);
  }
  bit_mask<u64, 8, 3> match_empty() const noexcept {
    const uint8x8_t dup = vdup_n_u8(static_cast<u8>(ctrl_t::empty));
    const uint8x8_t eq = vceq_u8(ctrl, dup);
    return bit_mask<u64, 8, 3>(vget_lane_u64(vreinterpret_u64_u8(eq), 0) &
                               MSBS);
  }
  bit_mask<u64, 8, 3> match_empty_or_deleted() const noexcept {
    const int8x8_t s = vdup_n_s8(static_cast<i8>(ctrl_t::sentinel));
    const uint8x8_t lt = vclt_s8(vreinterpret_s8_u8(ctrl), s);
    return bit_mask<u64, 8, 3>(vget_lane_u64(vreinterpret_u64_u8(lt), 0) &
                               MSBS);
  }
  u32 count_leading_empty_or_deleted() const noexcept {
    const int8x8_t s = vdup_n_s8(static_cast<i8>(ctrl_t::sentinel));
    const uint8x8_t ge = vcge_s8(vreinterpret_s8_u8(ctrl), s);
    const u64 mask = vget_lane_u64(vreinterpret_u64_u8(ge), 0);
    return static_cast<u32>(std::countr_zero(mask)) >> 3;
  }

  uint8x8_t ctrl;
};

#else

struct group {
  static constexpr usize WIDTH = 8;
  static constexpr u64 MSBS = 0x8080808080808080ull;
  static constexpr u64 LSBS = 0x0101010101010101ull;

  explicit group(const ctrl_t *pos) noexcept {
    u64 v;
    std::memcpy(&v, pos, sizeof(v));
    if constexpr (std::endian::native != std::endian::little)
      v = byteswap64(v);
    ctrl = v;
  }

  bit_mask<u64, 8, 3> match(u8 h) const noexcept {
    const u64 x = ctrl ^ (LSBS * h);
    return bit_mask<u64, 8, 3>((x - LSBS) & ~x & MSBS);
  }
  bit_mask<u64, 8, 3> match_empty() const noexcept {
    return bit_mask<u64, 8, 3>((ctrl & ~(ctrl << 6)) & MSBS);
  }
  bit_mask<u64, 8, 3> match_empty_or_deleted() const noexcept {
    return bit_mask<u64, 8, 3>((ctrl & ~(ctrl << 7)) & MSBS);
  }
  u32 count_leading_empty_or_deleted() const noexcept {
    constexpr u64 gaps = 0x00FEFEFEFEFEFEFEull;
    return (static_cast<u32>(
                std::countr_zero(((~ctrl & (ctrl >> 7)) | gaps) + 1)) +
            7) >>
           3;
  }

  static u64 byteswap64(u64 v) noexcept {
    return ((v & 0x00000000000000FFull) << 56) |
           ((v & 0x000000000000FF00ull) << 40) |
           ((v & 0x0000000000FF0000ull) << 24) |
           ((v & 0x00000000FF000000ull) << 8) |
           ((v & 0x000000FF00000000ull) >> 8) |
           ((v & 0x0000FF0000000000ull) >> 24) |
           ((v & 0x00FF000000000000ull) >> 40) |
           ((v & 0xFF00000000000000ull) >> 56);
  }

  u64 ctrl;
};

#endif

template <typename T, typename = void>
struct has_is_transparent : std::false_type {};
template <typename T>
struct has_is_transparent<T, std::void_t<typename T::is_transparent>>
    : std::true_type {};

[[nodiscard]] inline ctrl_t *empty_group() noexcept {
  alignas(16) static ctrl_t g[16] = {
      ctrl_t::sentinel, ctrl_t::empty, ctrl_t::empty, ctrl_t::empty,
      ctrl_t::empty,    ctrl_t::empty, ctrl_t::empty, ctrl_t::empty,
      ctrl_t::empty,    ctrl_t::empty, ctrl_t::empty, ctrl_t::empty,
      ctrl_t::empty,    ctrl_t::empty, ctrl_t::empty, ctrl_t::empty};
  return g;
}

/// The Swiss table under `flat_hash_map` and `flat_hash_set`. The policy says
/// what a slot holds: its key, and how it is built, moved and destroyed.
template <typename Policy, typename Hash, typename KeyEqual, typename Alloc>
class raw_hash_table {
  using alloc_traits = std::allocator_traits<Alloc>;

protected:
  using slot_type = typename Policy::slot_type;

  static constexpr usize NPOS = ~usize{0};
  static constexpr usize CLONED = group::WIDTH - 1;

public:
  using key_type = typename Policy::key_type;
  using value_type = typename Policy::value_type;
  using size_type = usize;
  using difference_type = isize;
  using hasher = Hash;
  using key_equal = KeyEqual;
  using allocator_type = Alloc;
  using reference = std::conditional_t<Policy::constant_elements,
                                       const value_type &, value_type &>;
  using const_reference = const value_type &;
  using pointer = typename alloc_traits::pointer;
  using const_pointer = typename alloc_traits::const_pointer;

private:
  using ctrl_allocator = typename alloc_traits::template rebind_alloc<ctrl_t>;
  using ctrl_traits = std::allocator_traits<ctrl_allocator>;
  using slot_allocator = alloc_traits::template rebind_alloc<slot_type>;
  using slot_traits = std::allocator_traits<slot_allocator>;

protected:
  static constexpr bool kTransparent =
      has_is_transparent<Hash>::value && has_is_transparent<KeyEqual>::value;

public:
  template <bool Const> class iterator_t {
    static constexpr bool kConst = Const || Policy::constant_elements;

  public:
    using value_type = raw_hash_table::value_type;
    using reference =
        std::conditional_t<kConst, const value_type &, value_type &>;
    using pointer =
        std::conditional_t<kConst, const value_type *, value_type *>;
    using difference_type = isize;
    using iterator_category = std::forward_iterator_tag;

    iterator_t() = default;

    template <bool C = Const, std::enable_if_t<C, i32> = 0>
    iterator_t(const iterator_t<false> &o) noexcept
        : m_ctrl(o.m_ctrl), m_slot(o.m_slot) {}

    reference operator*() const noexcept { return Policy::element(*m_slot); }
    pointer operator->() const noexcept {
      return std::addressof(Policy::element(*m_slot));
    }

    iterator_t &operator++() noexcept {
      ++m_ctrl;
      ++m_slot;
      skip_empty_or_deleted();
      return *this;
    }
    iterator_t operator++(i32) noexcept {
      iterator_t tmp = *this;
      ++*this;
      return tmp;
    }

    friend bool operator==(const iterator_t &a, const iterator_t &b) noexcept {
      return a.m_ctrl == b.m_ctrl;
    }
    friend bool operator!=(const iterator_t &a, const iterator_t &b) noexcept {
      return a.m_ctrl != b.m_ctrl;
    }

  private:
    friend class raw_hash_table;
    template <bool> friend class iterator_t;

    iterator_t(ctrl_t *c, slot_type *s) noexcept : m_ctrl(c), m_slot(s) {}

    void skip_empty_or_deleted() noexcept {
      while (ctrl_is_empty_or_deleted(*m_ctrl)) {
        u32 shift = group(m_ctrl).count_leading_empty_or_deleted();
        m_ctrl += shift;
        m_slot += shift;
      }
      if (*m_ctrl == ctrl_t::sentinel) {
        m_ctrl = nullptr;
        m_slot = nullptr;
      }
    }

    ctrl_t *m_ctrl = nullptr;
    slot_type *m_slot = nullptr;
  };

  using iterator = iterator_t<false>;
  using const_iterator = iterator_t<true>;

  raw_hash_table() noexcept(std::is_nothrow_default_constructible_v<Alloc>) =
      default;

  explicit raw_hash_table(const Alloc &alloc) noexcept : m_alloc(alloc) {}

  explicit raw_hash_table(usize bucket_count, const Alloc &alloc = Alloc())
      : m_alloc(alloc) {
    if (bucket_count)
      reserve(bucket_count);
  }

  template <typename It>
  raw_hash_table(It first, It last, const Alloc &alloc = Alloc())
      : m_alloc(alloc) {
    guarded([&] { insert(first, last); });
  }

  raw_hash_table(std::initializer_list<value_type> il,
                 const Alloc &alloc = Alloc())
      : m_alloc(alloc) {
    guarded([&] { insert(il.begin(), il.end()); });
  }

  raw_hash_table(const raw_hash_table &o)
      : m_alloc(alloc_traits::select_on_container_copy_construction(o.m_alloc)),
        m_hash_fn(o.m_hash_fn), m_eq(o.m_eq) {
    guarded([&] { copy_elements_from(o); });
  }

  raw_hash_table(const raw_hash_table &o, const Alloc &alloc)
      : m_alloc(alloc), m_hash_fn(o.m_hash_fn), m_eq(o.m_eq) {
    guarded([&] { copy_elements_from(o); });
  }

  raw_hash_table(raw_hash_table &&o) noexcept
      : m_ctrl(o.m_ctrl), m_slots(o.m_slots), m_size(o.m_size),
        m_capacity(o.m_capacity), m_growth_left(o.m_growth_left),
        m_alloc(std::move(o.m_alloc)), m_hash_fn(std::move(o.m_hash_fn)),
        m_eq(std::move(o.m_eq)) {
    o.m_ctrl = empty_group();
    o.m_slots = nullptr;
    o.m_size = 0;
    o.m_capacity = 0;
    o.m_growth_left = 0;
  }

  raw_hash_table(raw_hash_table &&o, const Alloc &alloc)
      : m_alloc(alloc), m_hash_fn(std::move(o.m_hash_fn)),
        m_eq(std::move(o.m_eq)) {
    if (m_alloc == o.m_alloc)
      steal_storage_from(o);
    else
      guarded([&] { move_elements_from(o); });
  }

  raw_hash_table &operator=(const raw_hash_table &o) {
    if (this == &o)
      return *this;

    if constexpr (alloc_traits::propagate_on_container_copy_assignment::value) {
      raw_hash_table tmp(o, o.m_alloc);
      swap(tmp);
    } else {
      raw_hash_table tmp(o, m_alloc);
      swap(tmp);
    }
    return *this;
  }

  raw_hash_table &operator=(raw_hash_table &&o) noexcept(
      alloc_traits::propagate_on_container_move_assignment::value ||
      alloc_traits::is_always_equal::value) {
    if (this == &o)
      return *this;

    if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
      destroy_and_free();
      m_alloc = std::move(o.m_alloc);
      steal_storage_from(o);
      m_hash_fn = std::move(o.m_hash_fn);
      m_eq = std::move(o.m_eq);
    } else if (m_alloc == o.m_alloc) {
      destroy_and_free();
      steal_storage_from(o);
      m_hash_fn = std::move(o.m_hash_fn);
      m_eq = std::move(o.m_eq);
    } else {
      destroy_and_free();
      m_hash_fn = std::move(o.m_hash_fn);
      m_eq = std::move(o.m_eq);
      move_elements_from(o);
    }
    return *this;
  }

  ~raw_hash_table() { destroy_and_free(); }

  iterator begin() noexcept {
    if (m_capacity == 0)
      return end();
    iterator it{m_ctrl, m_slots};
    it.skip_empty_or_deleted();
    return it;
  }
  const_iterator begin() const noexcept {
    if (m_capacity == 0)
      return end();
    const_iterator it{m_ctrl, m_slots};
    it.skip_empty_or_deleted();
    return it;
  }
  const_iterator cbegin() const noexcept { return begin(); }
  iterator end() noexcept { return iterator{}; }
  const_iterator end() const noexcept { return const_iterator{}; }
  const_iterator cend() const noexcept { return const_iterator{}; }

  [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
  [[nodiscard]] usize size() const noexcept { return m_size; }
  [[nodiscard]] usize capacity() const noexcept { return m_capacity; }
  [[nodiscard]] usize bucket_count() const noexcept { return m_capacity; }
  [[nodiscard]] f32 load_factor() const noexcept {
    return m_capacity ? static_cast<f32>(m_size) / static_cast<f32>(m_capacity)
                      : 0.0f;
  }
  [[nodiscard]] static f32 max_load_factor() noexcept { return 0.875f; }
  hasher hash_function() const { return m_hash_fn; }
  key_equal key_eq() const { return m_eq; }
  [[nodiscard]] allocator_type get_allocator() const noexcept {
    return m_alloc;
  }

  void clear() noexcept {
    if (m_capacity == 0)
      return;
    if constexpr (!std::is_trivially_destructible_v<value_type>) {
      for (usize i = 0; i != m_capacity; ++i)
        if (ctrl_is_full(m_ctrl[i]))
          destroy(m_slots + i);
    }
    reset_ctrl();
    m_size = 0;
    m_growth_left = capacity_to_growth(m_capacity);
  }

  std::pair<iterator, bool> insert(const value_type &v) {
    auto [idx, inserted] = find_or_prepare_insert(Policy::key_of(v));
    if (inserted)
      construct_or_abandon(idx, v);
    return {iterator_at(idx), inserted};
  }
  std::pair<iterator, bool> insert(value_type &&v) {
    auto [idx, inserted] = find_or_prepare_insert(Policy::key_of(v));
    if (inserted)
      construct_or_abandon(idx, std::move(v));
    return {iterator_at(idx), inserted};
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
  template <typename It> void insert(It first, It last) {
    for (; first != last; ++first)
      insert(*first);
  }
  void insert(std::initializer_list<value_type> il) {
    insert(il.begin(), il.end());
  }

  template <typename... Args>
  std::pair<iterator, bool> emplace(Args &&...args) {
    alignas(slot_type) unsigned char raw[sizeof(slot_type)];
    auto *tmp = reinterpret_cast<slot_type *>(raw);
    construct(tmp, std::forward<Args>(args)...);
    struct Guard {
      raw_hash_table *m;
      slot_type *s;
      ~Guard() { m->destroy(s); }
    } guard{this, tmp};

    auto [idx, inserted] = find_or_prepare_insert(key_of_slot(*tmp));
    if (inserted) {
      try {
        move_construct(m_slots + idx, tmp);
      } catch (...) {
        erase_meta_only(idx);
        throw;
      }
    }
    return {iterator_at(idx), inserted};
  }

  template <typename... Args>
  iterator emplace_hint(const_iterator, Args &&...args) {
    return emplace(std::forward<Args>(args)...).first;
  }

  iterator find(const key_type &k) {
    const usize idx = find_index(k);
    return idx == NPOS ? end() : iterator_at(idx);
  }
  const_iterator find(const key_type &k) const {
    const usize idx = find_index(k);
    return idx == NPOS ? end() : iterator_at(idx);
  }
  bool contains(const key_type &k) const { return find_index(k) != NPOS; }
  usize count(const key_type &k) const { return find_index(k) != NPOS ? 1 : 0; }

  template <typename K, bool E = kTransparent, std::enable_if_t<E, i32> = 0>
  iterator find(const K &k) {
    const usize idx = find_index(k);
    return idx == NPOS ? end() : iterator_at(idx);
  }
  template <typename K, bool E = kTransparent, std::enable_if_t<E, i32> = 0>
  const_iterator find(const K &k) const {
    const usize idx = find_index(k);
    return idx == NPOS ? end() : iterator_at(idx);
  }
  template <typename K, bool E = kTransparent, std::enable_if_t<E, i32> = 0>
  bool contains(const K &k) const {
    return find_index(k) != NPOS;
  }
  template <typename K, bool E = kTransparent, std::enable_if_t<E, i32> = 0>
  usize count(const K &k) const {
    return find_index(k) != NPOS ? 1 : 0;
  }

  usize erase(const key_type &k) {
    const usize idx = find_index(k);
    if (idx == NPOS)
      return 0;
    erase_at(idx);
    return 1;
  }
  template <typename K, bool E = kTransparent, std::enable_if_t<E, i32> = 0>
  usize erase(const K &k) {
    const usize idx = find_index(k);
    if (idx == NPOS)
      return 0;
    erase_at(idx);
    return 1;
  }
  iterator erase(const_iterator pos) {
    auto idx = static_cast<usize>(pos.m_ctrl - m_ctrl);
    erase_at(idx);
    iterator it{m_ctrl + idx, m_slots + idx};
    it.skip_empty_or_deleted();
    return it;
  }
  iterator erase(iterator pos) {
    auto idx = static_cast<usize>(pos.m_ctrl - m_ctrl);
    erase_at(idx);
    iterator it{m_ctrl + idx, m_slots + idx};
    it.skip_empty_or_deleted();
    return it;
  }

  void reserve(const usize n) {
    if (n == 0)
      return;
    usize target = normalize_capacity(growth_to_lowerbound_capacity(n));
    target = max(target, min_capacity());
    while (capacity_to_growth(target) < n)
      target = target * 2 + 1;
    if (target > m_capacity)
      resize(target);
  }

  void rehash(const usize n) {
    const usize lower = growth_to_lowerbound_capacity(m_size);
    usize target = normalize_capacity(max(n, lower));
    if (m_size != 0)
      target = max(target, min_capacity());
    if (target != m_capacity &&
        (target > m_capacity || m_size <= capacity_to_growth(target)))
      resize(target == 0 ? min_capacity() : target);
  }

  void swap(raw_hash_table &o) noexcept(
      alloc_traits::propagate_on_container_swap::value ||
      alloc_traits::is_always_equal::value) {
    using std::swap;
    if constexpr (alloc_traits::propagate_on_container_swap::value) {
      swap(m_alloc, o.m_alloc);
    } else {
      NX_ASSERT(m_alloc == o.m_alloc &&
                "raw_hash_table::swap: non-propagating unequal allocators");
    }
    swap(m_ctrl, o.m_ctrl);
    swap(m_slots, o.m_slots);
    swap(m_size, o.m_size);
    swap(m_capacity, o.m_capacity);
    swap(m_growth_left, o.m_growth_left);
    swap(m_hash_fn, o.m_hash_fn);
    swap(m_eq, o.m_eq);
  }

  friend bool operator==(const raw_hash_table &a, const raw_hash_table &b) {
    if (a.m_size != b.m_size)
      return false;
    for (const value_type &v : a) {
      const usize idx = b.find_index(Policy::key_of(v));
      if (idx == NPOS ||
          !Policy::equal_elements(v, Policy::element(b.m_slots[idx])))
        return false;
    }
    return true;
  }
  friend bool operator!=(const raw_hash_table &a, const raw_hash_table &b) {
    return !(a == b);
  }

protected:
  usize find_index(const auto &key) const {
    if (m_capacity == 0)
      return NPOS;
    return find_index_hashed(key, hash_key(key));
  }

  usize find_index_hashed(const auto &key, const usize hash) const {
    auto seq = probe(hash);
    const u8 tag = h2(hash);
    while (true) {
      group g(m_ctrl + seq.offset());
      for (u32 i : g.match(tag)) {
        const usize idx = seq.offset(i);
        if (NX_LIKELY(m_eq(key_of_slot(m_slots[idx]), key)))
          return idx;
      }
      if (NX_LIKELY(static_cast<bool>(g.match_empty())))
        return NPOS;
      seq.next();
    }
  }

  template <typename K>
  std::pair<usize, bool> find_or_prepare_insert(const K &key) {
    if (m_capacity == 0)
      resize(min_capacity());
    const usize hash = hash_key(key);
    auto seq = probe(hash);
    const u8 tag = h2(hash);
    while (true) {
      group g(m_ctrl + seq.offset());
      for (u32 i : g.match(tag)) {
        const usize idx = seq.offset(i);
        if (NX_LIKELY(m_eq(key_of_slot(m_slots[idx]), key)))
          return {idx, false};
      }
      if (NX_LIKELY(static_cast<bool>(g.match_empty())))
        break;
      seq.next();
    }
    return {prepare_insert(hash), true};
  }

  template <typename... Args> void construct(slot_type *s, Args &&...args) {
    Policy::construct(m_alloc, s, std::forward<Args>(args)...);
  }

  // A slot prepare_insert claimed is marked full before its element exists;
  // if constructing it throws, the slot is given back.
  template <typename... Args>
  void construct_or_abandon(const usize idx, Args &&...args) {
    try {
      construct(m_slots + idx, std::forward<Args>(args)...);
    } catch (...) {
      erase_meta_only(idx);
      throw;
    }
  }

  /// Finds @p key, or inserts value_type(args...) whose key equals it. The
  /// arguments may refer to elements of this table: when the insert would
  /// grow it, the element is built before anything moves.
  template <typename K, typename... Args>
  std::pair<usize, bool> find_or_emplace(const K &key, Args &&...args) {
    if (m_capacity != 0) {
      const usize hash = hash_key(key);
      const usize found = find_index_hashed(key, hash);
      if (found != NPOS)
        return {found, false};
      if (m_growth_left != 0) {
        const usize idx = prepare_insert(hash);
        construct_or_abandon(idx, std::forward<Args>(args)...);
        return {idx, true};
      }
    }
    const iterator it = emplace(std::forward<Args>(args)...).first;
    return {static_cast<usize>(it.m_ctrl - m_ctrl), true};
  }

  [[nodiscard]] slot_type *slot_at(const usize i) noexcept {
    return m_slots + i;
  }
  [[nodiscard]] const slot_type *slot_at(const usize i) const noexcept {
    return m_slots + i;
  }

  iterator iterator_at(usize i) noexcept {
    return iterator{m_ctrl + i, m_slots + i};
  }
  const_iterator iterator_at(usize i) const noexcept {
    return const_iterator{m_ctrl + i, m_slots + i};
  }

private:
  static ctrl_t *empty_group() noexcept { return detail::empty_group(); }

  static constexpr usize normalize_capacity(const usize n) noexcept {
    return n ? (~usize{0} >> static_cast<usize>(std::countl_zero(n))) : 1;
  }
  static constexpr usize min_capacity() noexcept {
    return normalize_capacity(group::WIDTH - 1);
  }
  static constexpr usize capacity_to_growth(const usize cap) noexcept {
    if constexpr (group::WIDTH == 8) {
      if (cap == 7)
        return 6;
    }
    return cap - cap / 8;
  }
  static constexpr usize
  growth_to_lowerbound_capacity(const usize growth) noexcept {
    return growth + static_cast<usize>((static_cast<i64>(growth) - 1) / 7);
  }

  struct probe_seq {
    usize m_mask;
    usize m_offset;
    usize m_index = 0;
    probe_seq(const usize hash, const usize mask) noexcept
        : m_mask(mask), m_offset(hash & mask) {}
    [[nodiscard]] usize offset() const noexcept { return m_offset; }
    [[nodiscard]] usize offset(const usize i) const noexcept {
      return (m_offset + i) & m_mask;
    }
    void next() noexcept {
      m_index += group::WIDTH;
      m_offset = (m_offset + m_index) & m_mask;
    }
    [[nodiscard]] usize index() const noexcept { return m_index; }
  };

  static const key_type &key_of_slot(const slot_type &s) noexcept {
    return Policy::key_of(Policy::element(s));
  }

  template <typename K> usize hash_key(const K &key) const {
    const auto h = static_cast<usize>(m_hash_fn(key));
    if constexpr (sizeof(usize) == 8)
      return hash_u64(h);
    else
      return hash_u32(h);
  }

  probe_seq probe(const usize hash) const noexcept {
    return probe_seq(hash >> 7, m_capacity);
  }
  static u8 h2(const usize hash) noexcept {
    return static_cast<u8>(hash & 0x7F);
  }

  void destroy(slot_type *s) noexcept { Policy::destroy(m_alloc, s); }
  void move_construct(slot_type *to, slot_type *from) {
    Policy::move_construct(m_alloc, to, from);
  }

  void set_ctrl(const usize i, const ctrl_t h) const noexcept {
    m_ctrl[i] = h;
    m_ctrl[((i - CLONED) & m_capacity) + (CLONED & m_capacity)] = h;
  }

  void reset_ctrl() const noexcept {
    std::memset(m_ctrl, static_cast<u8>(ctrl_t::empty),
                m_capacity + 1 + CLONED);
    m_ctrl[m_capacity] = ctrl_t::sentinel;
  }

  static constexpr usize ctrl_count(const usize cap) noexcept {
    return cap + 1 + CLONED;
  }

  void initialize(const usize new_cap) {
    ctrl_allocator ctrl_alloc(m_alloc);
    slot_allocator slot_alloc(m_alloc);

    ctrl_t *new_ctrl = nullptr;
    slot_type *new_slots = nullptr;
    const usize ctrl_n = ctrl_count(new_cap);

    try {
      new_ctrl = ctrl_traits::allocate(ctrl_alloc, ctrl_n);
      new_slots = slot_traits::allocate(slot_alloc, new_cap);
    } catch (...) {
      if (new_ctrl)
        ctrl_traits::deallocate(ctrl_alloc, new_ctrl, ctrl_n);
      throw;
    }

    m_ctrl = new_ctrl;
    m_slots = new_slots;
    m_capacity = new_cap;
    reset_ctrl();
    m_growth_left = capacity_to_growth(new_cap) - m_size;
  }

  void deallocate_storage(ctrl_t *ctrl, slot_type *slots,
                          const usize cap) noexcept {
    ctrl_allocator ctrl_alloc(m_alloc);
    slot_allocator slot_alloc(m_alloc);
    if (slots)
      slot_traits::deallocate(slot_alloc, slots, cap);
    if (ctrl && ctrl != empty_group())
      ctrl_traits::deallocate(ctrl_alloc, ctrl, ctrl_count(cap));
  }

  struct FindInfo {
    usize offset;
    usize probe_length;
  };
  FindInfo find_first_non_full(const usize hash) const {
    auto seq = probe(hash);
    while (true) {
      group g(m_ctrl + seq.offset());
      if (auto mask = g.match_empty_or_deleted())
        return {seq.offset(*mask), seq.index()};
      seq.next();
    }
  }

  usize prepare_insert(const usize hash) {
    FindInfo target = find_first_non_full(hash);
    if (NX_UNLIKELY(m_growth_left == 0 &&
                    !ctrl_is_deleted(m_ctrl[target.offset]))) {
      rehash_and_grow_if_necessary();
      target = find_first_non_full(hash);
    }
    ++m_size;
    m_growth_left -= ctrl_is_empty(m_ctrl[target.offset]) ? 1 : 0;
    set_ctrl(target.offset, static_cast<ctrl_t>(h2(hash)));
    return target.offset;
  }

  void erase_at(usize idx) {
    destroy(m_slots + idx);
    erase_meta_only(idx);
  }

  void erase_meta_only(const usize idx) noexcept {
    --m_size;
    const usize index_before = (idx - group::WIDTH) & m_capacity;
    const auto empty_after = group(m_ctrl + idx).match_empty();
    const auto empty_before = group(m_ctrl + index_before).match_empty();
    const bool was_never_full =
        static_cast<bool>(empty_before) && static_cast<bool>(empty_after) &&
        (empty_after.trailing_zeros() + empty_before.leading_zeros()) <
            group::WIDTH;
    set_ctrl(idx, was_never_full ? ctrl_t::empty : ctrl_t::deleted);
    m_growth_left += was_never_full ? 1 : 0;
  }

  void rehash_and_grow_if_necessary() {
    if (m_capacity == 0) {
      resize(min_capacity());
    } else if (m_capacity > group::WIDTH &&
               m_size * usize{32} <= m_capacity * usize{25}) {
      resize(m_capacity);
    } else {
      resize(m_capacity * 2 + 1);
    }
  }

  // A slot is marked full only once its element is moved in. If a move throws,
  // the table keeps what it moved, the rest is destroyed, and nothing leaks.
  void resize(const usize new_capacity) {
    ctrl_t *old_ctrl = m_ctrl;
    slot_type *old_slots = m_slots;
    const usize old_capacity = m_capacity;
    const bool had_alloc = old_capacity != 0;

    initialize(new_capacity);
    if (!had_alloc)
      return;

    usize i = 0;
    usize moved = 0;
    try {
      for (; i != old_capacity; ++i) {
        if (!ctrl_is_full(old_ctrl[i]))
          continue;
        const usize hash = hash_key(key_of_slot(old_slots[i]));
        const FindInfo t = find_first_non_full(hash);
        move_construct(m_slots + t.offset, old_slots + i);
        set_ctrl(t.offset, static_cast<ctrl_t>(h2(hash)));
        ++moved;
        destroy(old_slots + i);
      }
    } catch (...) {
      for (; i != old_capacity; ++i)
        if (ctrl_is_full(old_ctrl[i]))
          destroy(old_slots + i);
      deallocate_storage(old_ctrl, old_slots, old_capacity);
      m_size = moved;
      m_growth_left = capacity_to_growth(m_capacity) - moved;
      throw;
    }
    deallocate_storage(old_ctrl, old_slots, old_capacity);
  }

  template <typename F> void guarded(F &&build) {
    try {
      build();
    } catch (...) {
      destroy_and_free();
      throw;
    }
  }

  void destroy_and_free() noexcept {
    if (m_capacity == 0)
      return;
    if constexpr (!std::is_trivially_destructible_v<value_type>) {
      for (usize i = 0; i != m_capacity; ++i)
        if (ctrl_is_full(m_ctrl[i]))
          destroy(m_slots + i);
    }
    deallocate_storage(m_ctrl, m_slots, m_capacity);
    m_ctrl = empty_group();
    m_slots = nullptr;
    m_size = 0;
    m_capacity = 0;
    m_growth_left = 0;
  }

  void copy_elements_from(const raw_hash_table &o) {
    if (o.m_size == 0)
      return;
    reserve(o.m_size);
    for (const value_type &v : o) {
      auto [idx, inserted] = find_or_prepare_insert(Policy::key_of(v));
      (void)inserted;
      construct_or_abandon(idx, v);
    }
  }

  void steal_storage_from(raw_hash_table &o) noexcept {
    m_ctrl = o.m_ctrl;
    m_slots = o.m_slots;
    m_size = o.m_size;
    m_capacity = o.m_capacity;
    m_growth_left = o.m_growth_left;
    o.m_ctrl = empty_group();
    o.m_slots = nullptr;
    o.m_size = 0;
    o.m_capacity = 0;
    o.m_growth_left = 0;
  }

  void move_elements_from(raw_hash_table &o) {
    if (o.m_size == 0)
      return;
    reserve(o.m_size);
    for (usize i = 0; i != o.m_capacity; ++i) {
      if (ctrl_is_full(o.m_ctrl[i])) {
        slot_type *src = o.m_slots + i;
        auto [idx, inserted] = find_or_prepare_insert(key_of_slot(*src));
        (void)inserted;
        try {
          move_construct(m_slots + idx, src);
        } catch (...) {
          erase_meta_only(idx);
          throw;
        }
        o.destroy(src);
        o.set_ctrl(i, ctrl_t::deleted);
        --o.m_size;
      }
    }
    o.reset_ctrl();
    o.m_size = 0;
    o.m_growth_left = capacity_to_growth(o.m_capacity);
  }

  ctrl_t *m_ctrl = empty_group();
  slot_type *m_slots = nullptr;
  usize m_size = 0;
  usize m_capacity = 0;
  usize m_growth_left = 0;
  NX_NO_UNIQUE_ADDRESS Alloc m_alloc{};
  NX_NO_UNIQUE_ADDRESS Hash m_hash_fn{};
  NX_NO_UNIQUE_ADDRESS KeyEqual m_eq{};
};

} // namespace detail

/// Erases every element @p pred accepts from a hash_map or hash_set and
/// returns how many it erased.
template <typename Policy, typename Hash, typename KeyEqual, typename Alloc,
          typename Pred>
usize erase_if(detail::raw_hash_table<Policy, Hash, KeyEqual, Alloc> &t,
               Pred pred) {
  const usize before = t.size();
  for (auto at = t.begin(); at != t.end();)
    at = pred(*at) ? t.erase(at) : std::next(at);
  return before - t.size();
}

} // namespace nx
