#pragma once

/**
 * @file type_index.h
 * @brief Process-wide sequential ids for types (nx::type_index).
 */

#include "foundation/core/foundation.h"

#include <atomic>

namespace nx {
namespace detail {

/// Atomic because two threads may first use two different types at once. The
/// per-type static below is already thread-safe on its own; this is what makes
/// the counter behind it safe as well.
inline u32 next_type_index() noexcept {
  static std::atomic<u32> counter{0};
  return counter.fetch_add(1, std::memory_order_relaxed);
}

} // namespace detail

/// The id of T. Cheap after the first call: a load of a function-local static.
template <class T> [[nodiscard]] u32 type_index() noexcept {
  static const u32 id = detail::next_type_index();
  return id;
}

} // namespace nx
