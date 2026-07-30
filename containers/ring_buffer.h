#pragma once

/**
 * @file ring_buffer.h
 * @brief Bounded lock-free single-producer / single-consumer ring:
 * nx::spsc::Ring.
 */

#include "foundation/core/foundation.h"

#include <atomic>
#include <cstring>
#include <type_traits>

namespace nx {

/**
 * @brief Bounded lock-free SPSC ring over bulk trivially-copyable data.
 *
 * One thread writes, one thread reads, neither blocks or allocates after
 * reset(). Where the queues in concurrent_queue.h move one element per call,
 * this moves runs of them: a decoder hands over a few thousand samples and the
 * consumer takes whatever count it needs, so the unit of transfer is a span
 * rather than an item.
 *
 * Capacity is a reset() argument rather than a template parameter because the
 * size a stream wants is its channel count times its sample rate times a
 * latency budget - none of them known at compile time. It is rounded up to a
 * power of two.
 *
 * size() is exact when the consumer asks and space() is exact when the
 * producer asks, because the other side only ever makes more room available,
 * never less. Asked from any third thread both are lower bounds.
 */
namespace spsc {

template <typename T> class Ring {
  static_assert(std::is_trivially_copyable_v<T>,
                "spsc::Ring moves its elements with memcpy");

public:
  Ring() noexcept = default;

  ~Ring() { release_storage(); }

  Ring(const Ring &) = delete;
  Ring &operator=(const Ring &) = delete;
  Ring(Ring &&) = delete;
  Ring &operator=(Ring &&) = delete;

  /// Allocates @p capacity elements, rounded up to a power of two, and drops
  /// whatever was buffered. No other thread may be touching the ring.
  [[nodiscard]] bool reset(const usize capacity) noexcept {
    release_storage();
    if (capacity == 0)
      return false;

    const usize cap = nx::max<usize>(next_pow2<usize>(capacity), 2);
    void *raw = mem_alloc(cap * sizeof(T), CACHE_LINE);
    if (raw == nullptr)
      return false;

    m_data = static_cast<T *>(raw);
    m_capacity = cap;
    m_mask = cap - 1;
    m_write.store(0, std::memory_order_relaxed);
    m_read.store(0, std::memory_order_relaxed);
    return true;
  }

  [[nodiscard]] bool valid() const noexcept { return m_data != nullptr; }
  [[nodiscard]] usize capacity() const noexcept { return m_capacity; }

  [[nodiscard]] usize size() const noexcept {
    return m_write.load(std::memory_order_acquire) -
           m_read.load(std::memory_order_acquire);
  }

  [[nodiscard]] usize space() const noexcept { return m_capacity - size(); }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] bool full() const noexcept { return space() == 0; }

  /// Copies up to @p count elements in, returning how many fit. Producer only.
  usize write(const T *src, const usize count) noexcept {
    const usize w = m_write.load(std::memory_order_relaxed);
    const usize r = m_read.load(std::memory_order_acquire);
    const usize n = nx::min(count, m_capacity - (w - r));
    if (n == 0)
      return 0;

    const usize head = w & m_mask;
    const usize first = nx::min(n, m_capacity - head);
    std::memcpy(m_data + head, src, first * sizeof(T));
    if (n > first)
      std::memcpy(m_data, src + first, (n - first) * sizeof(T));

    m_write.store(w + n, std::memory_order_release);
    return n;
  }

  /// Copies up to @p count elements out, returning how many there were.
  /// Consumer only.
  usize read(T *dst, const usize count) noexcept {
    const usize r = m_read.load(std::memory_order_relaxed);
    const usize w = m_write.load(std::memory_order_acquire);
    const usize n = nx::min(count, w - r);
    if (n == 0)
      return 0;

    const usize tail = r & m_mask;
    const usize first = nx::min(n, m_capacity - tail);
    std::memcpy(dst, m_data + tail, first * sizeof(T));
    if (n > first)
      std::memcpy(dst + first, m_data, (n - first) * sizeof(T));

    m_read.store(r + n, std::memory_order_release);
    return n;
  }

  /// Drops up to @p count elements without copying them. Consumer only.
  usize discard(const usize count) noexcept {
    const usize r = m_read.load(std::memory_order_relaxed);
    const usize w = m_write.load(std::memory_order_acquire);
    const usize n = nx::min(count, w - r);
    m_read.store(r + n, std::memory_order_release);
    return n;
  }

  /// Drops everything buffered. Consumer only - it moves the read cursor, so
  /// a producer running concurrently keeps working rather than tearing.
  void clear() noexcept {
    m_read.store(m_write.load(std::memory_order_acquire),
                 std::memory_order_release);
  }

private:
  static constexpr usize CACHE_LINE = 64;

  void release_storage() noexcept {
    if (m_data != nullptr) {
      mem_free(m_data);
      m_data = nullptr;
    }
    m_capacity = 0;
    m_mask = 0;
  }

  // The cursors are monotonic rather than wrapped, so w - r is the count with
  // no ambiguity between full and empty. Unsigned wraparound keeps that
  // difference correct once the counters themselves wrap.
  alignas(CACHE_LINE) std::atomic<usize> m_write{0};
  alignas(CACHE_LINE) std::atomic<usize> m_read{0};
  alignas(CACHE_LINE) T *m_data = nullptr;
  usize m_capacity = 0;
  usize m_mask = 0;
};

} // namespace spsc

} // namespace nx
