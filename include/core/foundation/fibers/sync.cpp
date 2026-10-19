#include "core/foundation/fibers/sync.h"

#include "core/foundation/containers/small_vector.h"

namespace nx::fiber {

using internal::block;
using internal::claim;
using internal::Claimed;
using internal::NO_DEADLINE;
using internal::resume;
using internal::WaitResult;

namespace {

// A woken waiter may return and destroy the primitive at once, so waiters are
// claimed under its lock and resumed only after the lock is released.
using Woken = small_vector<Claimed, 8>;

void claim_all(internal::WaitList &list, Woken &woken) {
  while (const Claimed waiter = claim(list))
    woken.push_back(waiter);
}

void resume_all(const Woken &woken) noexcept {
  for (const Claimed &waiter : woken)
    resume(waiter);
}

} // namespace

void mutex::lock_slow() noexcept {
  for (i32 spin = 0; spin < nx::detail::SYNC_SPIN_COUNT; ++spin) {
    u32 expected = 0;
    if (m_state.load(std::memory_order_relaxed) == 0 &&
        m_state.compare_exchange_weak(expected, LOCKED,
                                      std::memory_order_acquire,
                                      std::memory_order_relaxed))
      return;
    nx::detail::cpu_relax();
  }
  for (;;) {
    m_lock.lock();
    if (m_state.exchange(CONTENDED, std::memory_order_acquire) == 0) {
      m_lock.unlock();
      return;
    }
    block(&m_lock, &m_waiters, NO_DEADLINE);
  }
}

void mutex::wake_one() noexcept {
  m_lock.lock();
  const Claimed claimed = claim(m_waiters);
  m_lock.unlock();
  if (claimed)
    resume(claimed);
}

void rw_lock::lock() noexcept {
  m_lock.lock();
  if (!m_writer && m_readers == 0) {
    m_writer = true;
    m_lock.unlock();
    return;
  }
  block(&m_lock, &m_waiting_writers, NO_DEADLINE);
}

bool rw_lock::try_lock() noexcept {
  scoped_lock guard(m_lock);
  if (m_writer || m_readers != 0)
    return false;
  m_writer = true;
  return true;
}

void rw_lock::unlock() noexcept {
  Woken woken;
  m_lock.lock();
  if (const Claimed writer = claim(m_waiting_writers)) {
    woken.push_back(writer);
  } else {
    m_writer = false;
    while (const Claimed reader = claim(m_waiting_readers)) {
      ++m_readers;
      woken.push_back(reader);
    }
  }
  m_lock.unlock();
  resume_all(woken);
}

void rw_lock::lock_shared() noexcept {
  m_lock.lock();
  if (!m_writer && m_waiting_writers.empty()) {
    ++m_readers;
    m_lock.unlock();
    return;
  }
  block(&m_lock, &m_waiting_readers, NO_DEADLINE);
}

bool rw_lock::try_lock_shared() noexcept {
  scoped_lock guard(m_lock);
  if (m_writer || !m_waiting_writers.empty())
    return false;
  ++m_readers;
  return true;
}

void rw_lock::unlock_shared() noexcept {
  m_lock.lock();
  Claimed writer;
  if (--m_readers == 0) {
    writer = claim(m_waiting_writers);
    if (writer)
      m_writer = true;
  }
  m_lock.unlock();
  if (writer)
    resume(writer);
}

void event::set() noexcept {
  Woken woken;
  m_lock.lock();
  if (m_manual_reset) {
    m_set.store(true, std::memory_order_release);
    claim_all(m_waiters, woken);
  } else if (const Claimed waiter = claim(m_waiters)) {
    woken.push_back(waiter);
  } else {
    m_set.store(true, std::memory_order_release);
  }
  m_lock.unlock();
  resume_all(woken);
}

void event::reset() noexcept {
  scoped_lock guard(m_lock);
  m_set.store(false, std::memory_order_release);
}

bool event::wait_until(const internal::clock::time_point deadline) noexcept {
  m_lock.lock();
  if (m_set.load(std::memory_order_relaxed)) {
    if (!m_manual_reset)
      m_set.store(false, std::memory_order_relaxed);
    m_lock.unlock();
    return true;
  }
  if (deadline <= internal::clock::now()) {
    m_lock.unlock();
    return false;
  }
  return block(&m_lock, &m_waiters, deadline) == WaitResult::Woken;
}

bool semaphore::try_acquire() noexcept {
  scoped_lock guard(m_lock);
  if (m_count == 0)
    return false;
  --m_count;
  return true;
}

bool semaphore::acquire_until(
    const internal::clock::time_point deadline) noexcept {
  m_lock.lock();
  if (m_count != 0) {
    --m_count;
    m_lock.unlock();
    return true;
  }
  if (deadline <= internal::clock::now()) {
    m_lock.unlock();
    return false;
  }
  return block(&m_lock, &m_waiters, deadline) == WaitResult::Woken;
}

void semaphore::release(u32 n) noexcept {
  Woken woken;
  m_lock.lock();
  for (; n != 0; --n) {
    const Claimed waiter = claim(m_waiters);
    if (!waiter)
      break;
    woken.push_back(waiter);
  }
  m_count += n;
  m_lock.unlock();
  resume_all(woken);
}

void cond_var::notify_one() noexcept {
  m_lock.lock();
  const Claimed waiter = claim(m_waiters);
  m_lock.unlock();
  if (waiter)
    resume(waiter);
}

void cond_var::notify_all() noexcept {
  Woken woken;
  m_lock.lock();
  claim_all(m_waiters, woken);
  m_lock.unlock();
  resume_all(woken);
}

bool cond_var::wait_until(mutex &m,
                          const internal::clock::time_point deadline) noexcept {
  m_lock.lock();
  m.unlock();
  const WaitResult result = block(&m_lock, &m_waiters, deadline);
  m.lock();
  return result == WaitResult::Woken;
}

void barrier::arrive_and_wait() noexcept {
  m_lock.lock();
  if (++m_arrived < m_threshold) {
    block(&m_lock, &m_waiters, NO_DEADLINE);
    return;
  }
  m_arrived = 0;
  Woken woken;
  claim_all(m_waiters, woken);
  m_lock.unlock();
  resume_all(woken);
}

void barrier::arrive_and_drop() noexcept {
  m_lock.lock();
  assert(m_threshold > 0 && "barrier: more participants dropped than it had");
  --m_threshold;
  if (m_arrived == 0 || m_arrived < m_threshold) {
    m_lock.unlock();
    return;
  }
  m_arrived = 0;
  Woken woken;
  claim_all(m_waiters, woken);
  m_lock.unlock();
  resume_all(woken);
}

} // namespace nx::fiber
