#pragma once

#include "core/foundation/containers/slot_map.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/threading/sync.h"

namespace nx {

/// A slot_map the render and main threads share: the render thread reads
/// resources (barriers, upload recording, present) while the main thread
/// creates and evicts them, too rare a access pattern to be worth a
/// lock-free design but frequent enough to race a plain slot_map - an
/// insert reallocating or an erase swap-popping the dense vector while a
/// try_get on another thread reads it.
///
/// A bare mutex around slot_map<T> is NOT enough: try_get returns a T* the
/// caller goes on using well after the lock is released, and a concurrent
/// insert/erase can still reallocate slot_map's dense array underneath that
/// pointer. Fixed by storing unique_ptr<T> in the underlying slot_map
/// instead of T directly - the dense array now moves pointers, not
/// pointees, so a pointer returned before a resize stays valid after it;
/// the mutex only has to guard the structural operations themselves. This
/// still relies on the caller's own contract that an entry in active use
/// this frame is never the one erased (e.g. a deferred-delete queue that
/// only frees an entry once the GPU work referencing it has retired).
template <typename T, typename Handle> class sync_slot_map {
public:
  [[nodiscard]] Handle insert(T &&value) {
    scoped_lock lock(m_mutex);
    return m_slots.insert(nx::make_unique<T>(std::move(value)));
  }

  [[nodiscard]] T *try_get(Handle h) noexcept {
    scoped_lock lock(m_mutex);
    unique_ptr<T> *slot = m_slots.try_get(h);
    return slot ? slot->get() : nullptr;
  }

  [[nodiscard]] const T *try_get(Handle h) const noexcept {
    scoped_lock lock(m_mutex);
    const unique_ptr<T> *slot = m_slots.try_get(h);
    return slot ? slot->get() : nullptr;
  }

  [[nodiscard]] bool contains(Handle h) const noexcept {
    scoped_lock lock(m_mutex);
    return m_slots.contains(h);
  }

  bool erase(Handle h) {
    scoped_lock lock(m_mutex);
    return m_slots.erase(h);
  }

  void clear() noexcept {
    scoped_lock lock(m_mutex);
    m_slots.clear();
  }

  template <typename Fn> void for_each(Fn &&fn) {
    scoped_lock lock(m_mutex);
    for (unique_ptr<T> &slot : m_slots)
      fn(*slot);
  }

private:
  mutable mutex m_mutex;
  slot_map<unique_ptr<T>, Handle> m_slots;
};

}
