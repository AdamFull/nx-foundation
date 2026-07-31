#pragma once

/**
 * @file concurrent_queue.h
 * @brief Lock-free bounded queues: nx::mpmc::Queue and nx::mpsc::Queue.
 */

#include "core/foundation/core/foundation.h"

#include <atomic>
#include <new>
#include <type_traits>
#include <utility>

namespace nx {

/**
 * @brief Bounded lock-free multi-producer / multi-consumer queue.
 *
 * Fixed-capacity ring buffer safe for any number of concurrent producers and
 * consumers. The try_* operations never block and report failure when the queue
 * is full or empty; the plain push/pop/emplace variants spin until they
 * succeed. Capacity is rounded up to a power of two.
 *
 * @tparam T        Element type (must be nothrow movable).
 * @tparam Capacity Requested capacity; rounded up to the next power of two.
 */
namespace mpmc {
template <typename T, usize Capacity> class Queue {
public:
  static_assert(Capacity > 0);
  static constexpr usize CAPACITY =
      nx::max<usize>(next_pow2<usize>(Capacity), 2);
  static constexpr usize MASK = CAPACITY - 1;

  Queue() noexcept {
    for (usize i = 0; i < CAPACITY; i++)
      m_slots[i].sequence.store(i, std::memory_order_relaxed);
  }

  ~Queue() noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      const usize eq = m_enqueue_pos.load(std::memory_order_relaxed);
      const usize dq = m_dequeue_pos.load(std::memory_order_relaxed);
      for (usize pos = dq; pos != eq; ++pos)
        m_slots[pos & MASK].ptr()->~T();
    }
  }

  Queue(const Queue &) = delete;
  Queue &operator=(const Queue &) = delete;
  Queue(Queue &&) = delete;
  Queue &operator=(Queue &&) = delete;

  template <typename... Args>
  NX_FORCE_INLINE bool try_emplace(Args &&...args) noexcept {
    static_assert(std::is_nothrow_constructible_v<T, Args...>);
    usize pos = m_enqueue_pos.load(std::memory_order_relaxed);
    for (;;) {
      Slot *slot = &m_slots[pos & MASK];
      const usize seq = slot->sequence.load(std::memory_order_acquire);
      const isize diff = nx::cast<isize>(seq) - nx::cast<isize>(pos);
      if (NX_LIKELY(diff == 0)) {
        if (m_enqueue_pos.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
          ::new (slot->storage) T(std::forward<Args>(args)...);
          slot->sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false; // full
      } else {
        pos = m_enqueue_pos.load(std::memory_order_relaxed);
      }
    }
  }

  NX_FORCE_INLINE bool try_push(const T &val) noexcept {
    static_assert(std::is_nothrow_copy_constructible_v<T>);
    return try_emplace(val);
  }

  NX_FORCE_INLINE bool try_push(T &&val) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<T>);
    return try_emplace(std::move(val));
  }

  NX_FORCE_INLINE bool try_pop(T &out) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<T>);
    usize pos = m_dequeue_pos.load(std::memory_order_relaxed);
    for (;;) {
      Slot *slot = &m_slots[pos & MASK];
      const usize seq = slot->sequence.load(std::memory_order_acquire);
      const isize diff = nx::cast<isize>(seq) - nx::cast<isize>(pos + 1);
      if (NX_LIKELY(diff == 0)) {
        if (m_dequeue_pos.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
          out = std::move(*slot->ptr());
          slot->ptr()->~T();
          slot->sequence.store(pos + CAPACITY, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false; // empty
      } else {
        pos = m_dequeue_pos.load(std::memory_order_relaxed);
      }
    }
  }

  template <typename... Args>
  NX_FORCE_INLINE void emplace(Args &&...args) noexcept {
    static_assert(std::is_nothrow_constructible_v<T, Args...>);
    const usize pos = m_enqueue_pos.fetch_add(1, std::memory_order_relaxed);
    Slot *slot = &m_slots[pos & MASK];
    while (NX_UNLIKELY(slot->sequence.load(std::memory_order_acquire) != pos))
      detail::cpu_relax();
    ::new (slot->storage) T(std::forward<Args>(args)...);
    slot->sequence.store(pos + 1, std::memory_order_release);
  }

  NX_FORCE_INLINE void push(const T &val) noexcept {
    static_assert(std::is_nothrow_copy_constructible_v<T>);
    emplace(val);
  }

  NX_FORCE_INLINE void push(T &&val) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<T>);
    emplace(std::move(val));
  }

  NX_FORCE_INLINE void pop(T &out) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<T>);
    const usize pos = m_dequeue_pos.fetch_add(1, std::memory_order_relaxed);
    Slot *slot = &m_slots[pos & MASK];
    while (
        NX_UNLIKELY(slot->sequence.load(std::memory_order_acquire) != pos + 1))
      detail::cpu_relax();
    out = std::move(*slot->ptr());
    slot->ptr()->~T();
    slot->sequence.store(pos + CAPACITY, std::memory_order_release);
  }

  // May be negative in blocking mode: consumers can advance m_dequeue_pos
  // ahead of m_enqueue_pos while spinning.
  [[nodiscard]] isize size_approx() const noexcept {
    return nx::cast<isize>(m_enqueue_pos.load(std::memory_order_relaxed)) -
           nx::cast<isize>(m_dequeue_pos.load(std::memory_order_relaxed));
  }

  [[nodiscard]] bool empty_approx() const noexcept {
    return size_approx() <= 0;
  }
  [[nodiscard]] bool full_approx() const noexcept {
    return size_approx() >= nx::cast<isize>(CAPACITY);
  }

private:
  struct alignas(64) Slot {
    std::atomic<usize> sequence{0};
    alignas(alignof(T)) u8 storage[sizeof(T)];

    NX_FORCE_INLINE T *ptr() noexcept {
      return std::launder(reinterpret_cast<T *>(storage));
    }
    NX_FORCE_INLINE const T *ptr() const noexcept {
      return std::launder(reinterpret_cast<const T *>(storage));
    }
  };

  alignas(64) std::atomic<usize> m_enqueue_pos{0};
  alignas(64) std::atomic<usize> m_dequeue_pos{0};
  Slot m_slots[CAPACITY];
};
} // namespace mpmc

static_assert(std::atomic<usize>::is_always_lock_free);

/**
 * @brief Bounded lock-free multi-producer / single-consumer queue.
 *
 * Like nx::mpmc::Queue but the consumer side assumes a single thread, which
 * makes dequeue cheaper. Adds consume_all() and consume_batch() helpers that
 * drain elements through a callback. Only one thread may pop at a time.
 *
 * @tparam T        Element type (must be nothrow movable).
 * @tparam Capacity Requested capacity; rounded up to the next power of two.
 */
namespace mpsc {
template <typename T, usize Capacity> class Queue {
public:
  static_assert(Capacity > 0);
  static constexpr usize CAPACITY =
      nx::max<usize>(next_pow2<usize>(Capacity), 2);
  static constexpr usize MASK = CAPACITY - 1;

  Queue() noexcept {
    for (usize i = 0; i < CAPACITY; ++i)
      m_slots[i].sequence.store(i, std::memory_order_relaxed);
  }

  ~Queue() noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      const usize eq = m_enqueue_pos.load(std::memory_order_relaxed);
      const usize dq = m_dequeue_pos.load(std::memory_order_relaxed);
      for (usize pos = dq; pos != eq; ++pos)
        m_slots[pos & MASK].ptr()->~T();
    }
  }

  Queue(const Queue &) = delete;
  Queue &operator=(const Queue &) = delete;
  Queue(Queue &&) = delete;
  Queue &operator=(Queue &&) = delete;

  template <typename... Args>
  NX_FORCE_INLINE bool try_emplace(Args &&...args) noexcept {
    static_assert(std::is_nothrow_constructible_v<T, Args...>);
    usize pos = m_enqueue_pos.load(std::memory_order_relaxed);
    for (;;) {
      Slot *slot = &m_slots[pos & MASK];
      const usize seq = slot->sequence.load(std::memory_order_acquire);
      const isize diff = nx::cast<isize>(seq) - nx::cast<isize>(pos);
      if (NX_LIKELY(diff == 0)) {
        if (m_enqueue_pos.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
          ::new (slot->storage) T(std::forward<Args>(args)...);
          slot->sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false; // full
      } else {
        pos = m_enqueue_pos.load(std::memory_order_relaxed);
      }
    }
  }

  NX_FORCE_INLINE bool try_push(const T &val) noexcept {
    static_assert(std::is_nothrow_copy_constructible_v<T>);
    return try_emplace(val);
  }

  NX_FORCE_INLINE bool try_push(T &&val) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<T>);
    return try_emplace(std::move(val));
  }

  template <typename... Args>
  NX_FORCE_INLINE void emplace(Args &&...args) noexcept {
    static_assert(std::is_nothrow_constructible_v<T, Args...>);
    const usize pos = m_enqueue_pos.fetch_add(1, std::memory_order_relaxed);
    Slot *slot = &m_slots[pos & MASK];
    while (NX_UNLIKELY(slot->sequence.load(std::memory_order_acquire) != pos))
      detail::cpu_relax();
    ::new (slot->storage) T(std::forward<Args>(args)...);
    slot->sequence.store(pos + 1, std::memory_order_release);
  }

  NX_FORCE_INLINE void push(const T &val) noexcept {
    static_assert(std::is_nothrow_copy_constructible_v<T>);
    emplace(val);
  }

  NX_FORCE_INLINE void push(T &&val) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<T>);
    emplace(std::move(val));
  }

  NX_FORCE_INLINE bool try_pop(T &out) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<T>);
    const usize pos = m_dequeue_pos.load(std::memory_order_relaxed);
    Slot *slot = &m_slots[pos & MASK];
    const usize seq = slot->sequence.load(std::memory_order_acquire);
    const isize diff = nx::cast<isize>(seq) - nx::cast<isize>(pos + 1);
    if (NX_LIKELY(diff == 0)) {
      out = std::move(*slot->ptr());
      slot->ptr()->~T();
      slot->sequence.store(pos + CAPACITY, std::memory_order_release);
      m_dequeue_pos.store(pos + 1, std::memory_order_relaxed);
      return true;
    }
    return false;
  }

  NX_FORCE_INLINE void pop(T &out) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<T>);
    const usize pos = m_dequeue_pos.load(std::memory_order_relaxed);
    Slot *slot = &m_slots[pos & MASK];
    while (
        NX_UNLIKELY(slot->sequence.load(std::memory_order_acquire) != pos + 1))
      detail::cpu_relax();
    out = std::move(*slot->ptr());
    slot->ptr()->~T();
    slot->sequence.store(pos + CAPACITY, std::memory_order_release);
    m_dequeue_pos.store(pos + 1, std::memory_order_relaxed);
  }

  template <typename Func>
  NX_FORCE_INLINE usize consume_all(Func &&fn) noexcept {
    usize count = 0;
    T item;
    while (try_pop(item)) {
      fn(std::move(item));
      ++count;
    }
    return count;
  }

  template <typename Func>
  NX_FORCE_INLINE usize consume_batch(Func &&fn, usize max) noexcept {
    usize count = 0;
    T item;
    while (count < max && try_pop(item)) {
      fn(std::move(item));
      ++count;
    }
    return count;
  }

  [[nodiscard]] isize size_approx() const noexcept {
    const isize eq =
        nx::cast<isize>(m_enqueue_pos.load(std::memory_order_relaxed));
    const isize dq =
        nx::cast<isize>(m_dequeue_pos.load(std::memory_order_relaxed));
    return eq - dq;
  }

  [[nodiscard]] bool empty_approx() const noexcept {
    return size_approx() <= 0;
  }
  [[nodiscard]] bool full_approx() const noexcept {
    return size_approx() >= nx::cast<isize>(CAPACITY);
  }

private:
  struct alignas(64) Slot {
    std::atomic<usize> sequence{0};
    alignas(alignof(T)) u8 storage[sizeof(T)];

    NX_FORCE_INLINE T *ptr() noexcept {
      return std::launder(reinterpret_cast<T *>(storage));
    }
    NX_FORCE_INLINE const T *ptr() const noexcept {
      return std::launder(reinterpret_cast<const T *>(storage));
    }
  };

  alignas(64) std::atomic<usize> m_enqueue_pos{0};
  alignas(64) std::atomic<usize> m_dequeue_pos{0};
  Slot m_slots[CAPACITY];
};
} // namespace mpsc

} // namespace nx