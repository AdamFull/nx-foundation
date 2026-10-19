#pragma once

#include "core/foundation/core/foundation.h"

#include <atomic>
#include <bit>
#include <cstring>
#include <type_traits>

namespace nx {

/// Bounded lock-free SPSC ring over bulk trivially-copyable data.
namespace spsc {

template <typename T> class ring {
  static_assert(std::is_trivially_copyable_v<T>,
                "spsc::ring moves its elements with memcpy");

public:
  ring() noexcept = default;

  ~ring() { release_storage(); }

  ring(const ring &) = delete;
  ring &operator=(const ring &) = delete;
  ring(ring &&) = delete;
  ring &operator=(ring &&) = delete;

  /// Allocates @p capacity elements, rounded up to a power of two, and drops
  /// whatever was buffered. No other thread may be touching the ring.
  [[nodiscard]] bool reset(const usize capacity) noexcept {
    release_storage();
    if (capacity == 0 || capacity > MAX_CAPACITY)
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

  usize discard(const usize count) noexcept {
    const usize r = m_read.load(std::memory_order_relaxed);
    const usize w = m_write.load(std::memory_order_acquire);
    const usize n = nx::min(count, w - r);
    m_read.store(r + n, std::memory_order_release);
    return n;
  }

  void clear() noexcept {
    m_read.store(m_write.load(std::memory_order_acquire),
                 std::memory_order_release);
  }

private:
  static constexpr usize CACHE_LINE = 64;
  static constexpr usize MAX_CAPACITY = std::bit_floor(~usize{0} / sizeof(T));

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

/// Deprecated: the name before foundation types were snake_case.
template <typename T> using Ring = ring<T>;
}

}
