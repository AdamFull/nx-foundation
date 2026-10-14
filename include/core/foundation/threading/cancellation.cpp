#include "core/foundation/threading/cancellation.h"

#include "core/foundation/threading/thread.h"

namespace nx {
namespace detail {

void cancellation_state::run_callbacks() noexcept {
  lock.lock();
  while (cancellation_callback *callback = callbacks) {
    callbacks = callback->m_next;
    if (callbacks != nullptr)
      callbacks->m_prev = nullptr;
    callback->m_linked = false;
    running = callback;
    running_thread = this_thread::id();
    lock.unlock();
    callback->m_fn();
    lock.lock();
    running = nullptr;
  }
  lock.unlock();
}

} // namespace detail

cancellation_callback::cancellation_callback(const cancellation_token &token,
                                             Fn fn) noexcept
    : m_fn(std::move(fn)) {
  if (token.m_state == nullptr || !m_fn)
    return;
  {
    scoped_lock guard(token.m_state->lock);
    if (!token.m_state->requested.load(std::memory_order_acquire)) {
      m_state = token.m_state;
      m_next = m_state->callbacks;
      if (m_next != nullptr)
        m_next->m_prev = this;
      m_state->callbacks = this;
      m_linked = true;
      return;
    }
  }
  m_fn();
}

cancellation_callback::~cancellation_callback() {
  if (m_state == nullptr)
    return;
  for (;;) {
    {
      scoped_lock guard(m_state->lock);
      if (m_linked) {
        (m_prev != nullptr ? m_prev->m_next : m_state->callbacks) = m_next;
        if (m_next != nullptr)
          m_next->m_prev = m_prev;
        return;
      }
      // Running on this thread means the callback is destroying itself.
      if (m_state->running != this ||
          m_state->running_thread == this_thread::id())
        return;
    }
    this_thread::yield();
  }
}

} // namespace nx
