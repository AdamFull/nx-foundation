#include "core/foundation/threading/serial_queue.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/fibers/scheduler.h"
#include "core/foundation/threading/cancellation.h"
#include "core/foundation/threading/sync.h"

#include <exception>

namespace nx {

serial_queue::serial_queue(thread_pool &pool, const config &cfg)
    : m_pool(pool), m_config(cfg) {}

serial_queue::~serial_queue() {
  close();
  flush();
}

bool serial_queue::post(task fn, const bool over_capacity) {
  bool schedule = false;
  {
    const scoped_lock lock(m_lock);
    if (m_closed || (!over_capacity && m_tasks.size() >= m_config.capacity))
      return false;
    m_tasks.push_back(std::move(fn));
    schedule = !m_draining;
    m_draining = true;
  }
  if (schedule)
    m_pool.submit(m_config.priority, [this] { drain(); });
  return true;
}

void serial_queue::drain() noexcept {
  for (;;) {
    vector<task> batch;
    {
      const scoped_lock lock(m_lock);
      if (m_tasks.empty()) {
        m_draining = false;
        m_idle.notify_all();
        return;
      }
      batch = std::move(m_tasks);
      m_tasks.clear();
      m_running = batch.size();
    }
    const auto run = [&batch] {
      for (task &fn : batch) {
        try {
          fn();
        } catch (const task_cancelled &) {
        } catch (const std::exception &e) {
          nx::loge("serial queue: unhandled task exception: {}", e.what());
        } catch (...) {
          nx::loge("serial queue: unhandled non-standard task exception");
        }
      }
    };
    if (m_config.blocking)
      fiber::run_blocking(run);
    else
      run();
    const scoped_lock lock(m_lock);
    m_running = 0;
  }
}

void serial_queue::flush() noexcept {
  const scoped_lock lock(m_lock);
  m_idle.wait(m_lock, [this] { return !m_draining; });
}

void serial_queue::close() noexcept {
  const scoped_lock lock(m_lock);
  m_closed = true;
}

bool serial_queue::closed() const noexcept {
  const scoped_lock lock(m_lock);
  return m_closed;
}

usize serial_queue::pending() const noexcept {
  const scoped_lock lock(m_lock);
  return m_tasks.size() + m_running;
}

} // namespace nx
