#include "core/foundation/fibers/stack.h"

#include "core/foundation/diagnostics/diagnostic.h"
#include "core/foundation/platform/os.h"

#if NX_HAS_WINDOWS_API
#include <windows.h>
#elif NX_PLATFORM_POSIX
#include <sys/mman.h>
#endif

namespace nx::fiber {

namespace {

u8 *bytes(void *p) noexcept { return nx::cast<u8 *>(p); }

void release_reservation(const fiber::stack &stack) noexcept {
  vmem::release(stack.reservation, stack.reservation_size);
}

#if NX_HAS_WINDOWS_API
bool arm_guard_page(void *committed, usize page_size) noexcept {
  return VirtualAlloc(bytes(committed) - page_size, page_size, MEM_COMMIT,
                      PAGE_READWRITE | PAGE_GUARD) != nullptr;
}
#endif

} // namespace

usize resident_bytes(const fiber::stack &stack) noexcept {
  if (!stack)
    return 0;
  usize total = 0;
#if NX_HAS_WINDOWS_API
  u8 *cursor = bytes(stack.reservation);
  u8 *const end = bytes(stack.reservation) + stack.reservation_size;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(cursor, &info, sizeof(info)))
      break;
    u8 *region_end = nx::cast<u8 *>(info.BaseAddress) + info.RegionSize;
    if (region_end > end)
      region_end = end;
    if (info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD))
      total += nx::cast<usize>(region_end - cursor);
    cursor = region_end;
  }
#elif NX_PLATFORM_POSIX
  const usize page = vmem::get_page_size();
  constexpr usize BATCH = 256;
#if NX_PLATFORM_APPLE
  char residency[BATCH];
#else
  unsigned char residency[BATCH];
#endif
  u8 *cursor = bytes(stack.limit);
  while (cursor < bytes(stack.base)) {
    const usize pages =
        min(BATCH, nx::cast<usize>(bytes(stack.base) - cursor) / page);
    if (::mincore(cursor, pages * page, residency) != 0)
      break;
    for (usize i = 0; i < pages; ++i)
      if (residency[i] & 1)
        total += page;
    cursor += pages * page;
  }
#endif
  return total;
}

stack_pool::stack_pool(const config &config) noexcept {
  NX_ASSERT(config.stack_size > 0);
  m_page_size = vmem::get_page_size();
  m_guard_size = m_page_size;
  m_max_cached = config.max_cached;

  const usize granularity =
      max(vmem::get_allocation_granularity(), m_page_size);
  m_reservation_size =
      align_up(m_guard_size + max(config.stack_size, m_page_size), granularity);
  m_stack_size = m_reservation_size - m_guard_size;
  m_initial_commit = clamp(align_up(config.initial_commit, m_page_size),
                           m_page_size, m_stack_size - m_page_size);
}

stack_pool::~stack_pool() noexcept {
  NX_ASSERT(m_stats.live == 0, "{} fiber stacks outlive their pool",
            m_stats.live);
  for (const fiber::stack &stack : m_cached)
    release_reservation(stack);
}

result<stack, stack_error> stack_pool::acquire() noexcept {
  {
    scoped_lock guard(m_lock);
    if (!m_cached.empty()) {
      fiber::stack stack = m_cached.back();
      m_cached.pop_back();
      m_stats.cached = nx::cast<u32>(m_cached.size());
      m_stats.peak_live = max(m_stats.peak_live, ++m_stats.live);
      return stack;
    }
  }

  fiber::stack stack;
  if (!vmem::reserve(&stack.reservation, m_reservation_size))
    return err(stack_error::OutOfMemory);
  stack.reservation_size = m_reservation_size;
  stack.limit = bytes(stack.reservation) + m_guard_size;
  stack.base = bytes(stack.reservation) + m_reservation_size;

#if NX_HAS_WINDOWS_API
  stack.committed = bytes(stack.base) - m_initial_commit;
  const bool ready = vmem::commit(stack.committed, m_initial_commit) &&
                     arm_guard_page(stack.committed, m_page_size);
#else
  stack.committed = stack.limit;
  const bool ready = vmem::commit(stack.limit, m_stack_size);
#endif
  if (!ready) {
    release_reservation(stack);
    return err(stack_error::OutOfMemory);
  }

  scoped_lock guard(m_lock);
  m_stats.reserved_bytes += m_reservation_size;
  m_stats.peak_live = max(m_stats.peak_live, ++m_stats.live);
  return stack;
}

void stack_pool::release(const fiber::stack &stack) noexcept {
  if (!stack)
    return;
  NX_ASSERT(stack.reservation_size == m_reservation_size,
            "fiber stack released to a pool that did not create it");
  {
    scoped_lock guard(m_lock);
    NX_ASSERT(m_stats.live > 0);
    --m_stats.live;
    if (m_cached.size() < m_max_cached) {
      m_cached.push_back(stack);
      m_stats.cached = nx::cast<u32>(m_cached.size());
      return;
    }
    m_stats.reserved_bytes -= m_reservation_size;
  }
  release_reservation(stack);
}

void stack_pool::trim() noexcept {
  scoped_lock guard(m_lock);
  for (const fiber::stack &stack : m_cached) {
    const usize grown = nx::cast<usize>(bytes(stack.base) - m_initial_commit -
                                        bytes(stack.limit));
#if NX_HAS_WINDOWS_API
    // The OS moved the guard page down as the stack grew; decommitting the
    // whole grown range removes it, so it has to be re-armed at the top.
    VirtualFree(stack.limit, grown, MEM_DECOMMIT);
    arm_guard_page(stack.committed, m_page_size);
#elif NX_PLATFORM_APPLE
    ::madvise(stack.limit, grown, MADV_FREE);
#elif NX_PLATFORM_POSIX
    ::madvise(stack.limit, grown, MADV_DONTNEED);
#endif
  }
}

stack_pool_stats stack_pool::stats() const noexcept {
  scoped_lock guard(m_lock);
  return m_stats;
}

} // namespace nx::fiber
