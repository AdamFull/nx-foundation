#include "core/foundation/fibers/context.h"

#include "core/foundation/diagnostics/diagnostic.h"
#include "core/foundation/fibers/stack.h"

#include <cstdlib>
#include <cstring>

extern "C" {
void *nx_fiber_switch(void **from_sp, void *to_sp, void *data);
void nx_fiber_start();
}

#if NX_ASAN_ENABLED
extern "C" {
void __sanitizer_start_switch_fiber(void **fake_stack_save, const void *bottom,
                                    usize size);
void __sanitizer_finish_switch_fiber(void *fake_stack_save,
                                     const void **bottom_old, usize *size_old);
}
#endif

#if NX_TSAN_ENABLED
extern "C" {
void *__tsan_get_current_fiber();
void *__tsan_create_fiber(unsigned flags);
void __tsan_destroy_fiber(void *fiber);
void __tsan_switch_to_fiber(void *fiber, unsigned flags);
}
#endif

namespace nx::fiber {

namespace {

struct RawTransfer {
  Context *from;
  Context *to;
  void *data;
};

#if defined(NX_ARCH_X64)
constexpr u32 DEFAULT_MXCSR = 0x1F80;
constexpr u16 DEFAULT_X87_CW = 0x027F;
#endif

template <typename T> void store(u8 *frame, usize offset, T value) noexcept {
  std::memcpy(frame + offset, &value, sizeof(T));
}

// Builds a frame that nx_fiber_switch restores as if the fiber had suspended
// itself, returning into nx_fiber_start. Offsets mirror the context_*.asm/.S
// frame layouts.
void *build_initial_frame(const Stack &stack) noexcept {
  const uintptr_t top =
      reinterpret_cast<uintptr_t>(stack.base) & ~uintptr_t(15);
  auto *const start = reinterpret_cast<void *>(&nx_fiber_start);
#if defined(NX_ARCH_X64)
  auto **slots = reinterpret_cast<void **>(top);
  slots[-1] = nullptr;
  slots[-2] = nullptr; // nx_fiber_start's return address: ends every unwind
  slots[-3] = start;
#if NX_PLATFORM_WINDOWS
  constexpr usize FRAME = 0x108;
  u8 *frame = reinterpret_cast<u8 *>(slots - 3) - FRAME;
  std::memset(frame, 0, FRAME);
  store(frame, 0xA0, stack.base);
  store(frame, 0xA8, stack.committed);
  store(frame, 0xB0, stack.reservation);
  store(frame, 0xB8, DEFAULT_MXCSR);
  store(frame, 0xBC, DEFAULT_X87_CW);
#else
  constexpr usize FRAME = 0x38;
  u8 *frame = reinterpret_cast<u8 *>(slots - 3) - FRAME;
  std::memset(frame, 0, FRAME);
  store(frame, 0x00, DEFAULT_MXCSR);
  store(frame, 0x04, DEFAULT_X87_CW);
#endif
#elif defined(NX_ARCH_AARCH64)
  constexpr usize FRAME = 0xB0;
  u8 *frame = reinterpret_cast<u8 *>(top) - 16 - FRAME;
  std::memset(frame, 0, FRAME);
  store(frame, 0xA8, start);
#else
#error "nx::fiber has no context switch for this architecture"
#endif
  return frame;
}

} // namespace

struct ContextAccess {
  static void before_switch(Context &from, Context &to,
                            bool from_exits) noexcept {
    NX_ASSERT(to.m_sp, "switching to a fiber that is running or never started");
#if NX_TSAN_ENABLED
    if (!from.m_tsan_fiber)
      from.m_tsan_fiber = __tsan_get_current_fiber();
    __tsan_switch_to_fiber(to.m_tsan_fiber, 0);
#endif
#if NX_ASAN_ENABLED
    __sanitizer_start_switch_fiber(from_exits ? nullptr
                                              : &from.m_asan_fake_stack,
                                   to.m_stack_bottom, to.m_stack_size);
#else
    (void)from;
    (void)from_exits;
#endif
  }

  static void after_switch(const RawTransfer &in) noexcept {
    Context &self = *in.to;
    self.m_sp = nullptr;
#if NX_ASAN_ENABLED
    const void *from_bottom = nullptr;
    usize from_size = 0;
    __sanitizer_finish_switch_fiber(self.m_asan_fake_stack, &from_bottom,
                                    &from_size);
    self.m_asan_fake_stack = nullptr;
    // A thread's own context learns its stack bounds the first time it
    // switches away, which is the only way ASan can switch back to it.
    if (!in.from->m_stack_bottom) {
      in.from->m_stack_bottom = from_bottom;
      in.from->m_stack_size = from_size;
    }
#endif
  }

  [[noreturn]] static void run(const RawTransfer &in) noexcept {
    after_switch(in);
    in.to->m_entry(Transfer{in.from, in.data});
    NX_ASSERT(false, "fiber entry returned");
    std::abort();
  }
};

Context::Context(const Stack &stack, const EntryFn entry) noexcept {
  reset(stack, entry);
}

Context::~Context() noexcept {
#if NX_TSAN_ENABLED
  if (m_owns_tsan_fiber)
    __tsan_destroy_fiber(m_tsan_fiber);
#endif
}

void Context::reset(const Stack &stack, const EntryFn entry) noexcept {
  NX_ASSERT(stack && entry);
  m_entry = entry;
  m_stack_bottom = stack.limit;
  m_stack_size = stack.size();
#if NX_ASAN_ENABLED
  m_asan_fake_stack = nullptr;
#endif
#if NX_TSAN_ENABLED
  if (m_owns_tsan_fiber)
    __tsan_destroy_fiber(m_tsan_fiber);
  m_tsan_fiber = __tsan_create_fiber(0);
  m_owns_tsan_fiber = true;
#endif
  m_sp = build_initial_frame(stack);
}

Transfer switch_to(Context &from, Context &to, void *data) noexcept {
  ContextAccess::before_switch(from, to, false);
  RawTransfer out{&from, &to, data};
  const auto *in = static_cast<const RawTransfer *>(
      nx_fiber_switch(&from.m_sp, to.m_sp, &out));
  NX_ASSERT(in->to == &from);
  ContextAccess::after_switch(*in);
  return Transfer{in->from, in->data};
}

void exit_to(Context &from, Context &to, void *data) noexcept {
  ContextAccess::before_switch(from, to, true);
  RawTransfer out{&from, &to, data};
  nx_fiber_switch(&from.m_sp, to.m_sp, &out);
  NX_ASSERT(false, "an exited fiber was resumed");
  std::abort();
}

} // namespace nx::fiber

extern "C" [[noreturn]] void nx_fiber_main(void *transfer) {
  nx::fiber::ContextAccess::run(
      *static_cast<const nx::fiber::RawTransfer *>(transfer));
}
