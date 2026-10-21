#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/core/time.h"

#include <algorithm>
#include <atomic>
#include <optional>
#include <utility>

namespace nx {

/// A place reserved in a completion_queue for one result.
struct completion_ticket {
  u64 sequence = 0;
};

/// Results of work done on other threads, published on their owner's thread
/// in the order the work was asked for, a bounded number at a time.
///
/// The owner reserves a place for each piece of work before handing it out,
/// and later publishes: results go to it in reservation order, so a result
/// that arrives early waits for those before it, and the owner sees the same
/// order however the workers ran. Each reserved place must be finished once,
/// from any thread: complete() with the result, or abandon() for work that
/// was cancelled or failed, which publication skips. At most `capacity`
/// places are reserved and not yet published; reserve() refuses past it, so
/// work cannot pile up faster than the owner takes it.
///
/// reserve() and publish() are the owner's; complete() and abandon() may be
/// called from any thread. The queue must outlive the work holding tickets.
template <class T> class completion_queue {
public:
  explicit completion_queue(const usize capacity)
      : m_slots(std::max<usize>(capacity, 1)) {}

  completion_queue(const completion_queue &) = delete;
  completion_queue &operator=(const completion_queue &) = delete;

  [[nodiscard]] usize capacity() const noexcept { return m_slots.size(); }
  /// Places reserved and not yet published or skipped.
  [[nodiscard]] usize pending() const noexcept {
    return static_cast<usize>(m_tail - m_head);
  }

  /// The next place in order; none while `capacity` places are pending.
  [[nodiscard]] std::optional<completion_ticket> reserve() {
    if (pending() == capacity())
      return std::nullopt;
    slot &place = at(m_tail);
    NX_ASSERT(place.state.load(std::memory_order_acquire) == FREE);
    place.sequence = m_tail;
    place.state.store(RESERVED, std::memory_order_release);
    return completion_ticket{m_tail++};
  }

  void complete(const completion_ticket ticket, T value) {
    slot &place = finishing(ticket);
    place.value.emplace(std::move(value));
    place.state.store(READY, std::memory_order_release);
  }

  void abandon(const completion_ticket ticket) {
    finishing(ticket).state.store(ABANDONED, std::memory_order_release);
  }

  /// Calls @p fn(T&&) for the results ready at the front, in order, up to
  /// @p max_count of them and, past the first, while @p max_time (when not
  /// zero) has not run out; stops at the first place not finished. Abandoned
  /// places at the front are let go along the way. Returns how many were
  /// published.
  template <class F>
  usize publish(const usize max_count, F &&fn, const duration max_time = {}) {
    const stopwatch watch;
    usize published = 0;
    while (m_head != m_tail) {
      slot &place = at(m_head);
      const u32 state = place.state.load(std::memory_order_acquire);
      if (state == ABANDONED) {
        place.state.store(FREE, std::memory_order_relaxed);
        ++m_head;
        continue;
      }
      if (state != READY || published == max_count)
        break;
      if (published != 0 && max_time.nanos() > 0 && watch.elapsed() >= max_time)
        break;
      T value = std::move(*place.value);
      place.value.reset();
      place.state.store(FREE, std::memory_order_relaxed);
      ++m_head;
      ++published;
      fn(std::move(value));
    }
    return published;
  }

private:
  static constexpr u32 FREE = 0;
  static constexpr u32 RESERVED = 1;
  static constexpr u32 READY = 2;
  static constexpr u32 ABANDONED = 3;

  struct slot {
    std::atomic<u32> state{FREE};
    u64 sequence = 0;
    std::optional<T> value;
  };

  [[nodiscard]] slot &at(const u64 sequence) noexcept {
    return m_slots[static_cast<usize>(sequence % m_slots.size())];
  }

  [[nodiscard]] slot &finishing(const completion_ticket ticket) noexcept {
    slot &place = at(ticket.sequence);
    NX_ASSERT(place.sequence == ticket.sequence &&
              place.state.load(std::memory_order_acquire) == RESERVED &&
              "completion_queue: a ticket finished twice or never reserved");
    return place;
  }

  nx::vector<slot> m_slots;
  u64 m_head = 0;
  u64 m_tail = 0;
};

} // namespace nx
