#pragma once

/**
 * @file radix_sort.h
 * @brief Stable LSD radix sort over unsigned 32- or 64-bit keys.
 *
 * Linear in n, and stable by construction rather than by asking for it - which
 * matters wherever ties must not reorder between runs. It never compares, so
 * it wins over a comparison sort once n is large enough that the O(log n)
 * factor and the branch mispredictions dominate.
 *
 * Digits are 11 bits: 2048 counters stay comfortably in L1, where a 16-bit
 * digit's 65536 would not, and 11 bits needs fewer passes than 8.
 *
 * Every histogram is built in one read of the keys, up front. That costs one
 * extra pass over the keys but pays for itself twice: a digit whose values are
 * all identical is skipped entirely, and the number of surviving passes is
 * known before any data moves, so the ping-pong can be arranged to finish in
 * the caller's buffer and skip the final copy.
 *
 * The skip is what makes a 64-bit key cheap when its values are small. Sorting
 * u64 keys that all fit in 32 bits costs the same as sorting u32 keys - the
 * three empty high digits cost one histogram read and nothing else.
 *
 * Nothing is allocated: the caller supplies scratch the size of the input, so
 * this runs every frame without touching the allocator.
 */

#include "foundation/core/foundation.h"
#include "foundation/threading/thread_pool.h"

#include <concepts>
#include <cstring>
#include <span>

namespace nx {

namespace detail {

inline constexpr u32 RADIX_DIGIT_BITS = 11;
inline constexpr u32 RADIX_BUCKETS = 1u << RADIX_DIGIT_BITS;
inline constexpr u32 RADIX_DIGIT_MASK = RADIX_BUCKETS - 1;

/// Passes needed to cover a key of @p bits, rounded up.
[[nodiscard]] constexpr u32 radix_pass_count(const u32 bits) noexcept {
  return (bits + RADIX_DIGIT_BITS - 1) / RADIX_DIGIT_BITS;
}

} // namespace detail

/// Keys must be unsigned: the digit extraction is a shift and mask, and a
/// signed key would sort negatives after positives.
template <typename T>
concept radix_key =
    std::unsigned_integral<T> && (sizeof(T) == 4 || sizeof(T) == 8);

/**
 * @brief Sorts @p keys ascending, permuting @p values alongside.
 *
 * @param keys           Sort keys; reordered in place.
 * @param values         Payload, one per key; reordered to match.
 * @param key_scratch    Scratch, at least keys.size().
 * @param value_scratch  Scratch, at least values.size().
 *
 * The payload is normally an index into the real array rather than the array
 * itself: each pass moves the payload once, so keeping it to four bytes and
 * gathering once at the end beats shuffling large elements repeatedly.
 */
template <radix_key Key, typename Value>
void radix_sort_pairs(const std::span<Key> keys, const std::span<Value> values,
                      const std::span<Key> key_scratch,
                      const std::span<Value> value_scratch) {
  const usize count = keys.size();
  if (count < 2 || values.size() < count || key_scratch.size() < count ||
      value_scratch.size() < count)
    return;

  constexpr u32 PASSES =
      detail::radix_pass_count(nx::cast<u32>(sizeof(Key) * 8));

  // All histograms in one read of the keys.
  u32 histogram[PASSES][detail::RADIX_BUCKETS] = {};
  for (usize i = 0; i < count; ++i) {
    const Key k = keys[i];
    for (u32 p = 0; p < PASSES; ++p)
      ++histogram[p][nx::cast<u32>((k >> (p * detail::RADIX_DIGIT_BITS)) &
                                   detail::RADIX_DIGIT_MASK)];
  }

  // A digit where every key lands in one bucket is already sorted by that
  // digit, so moving the data would be pure cost.
  bool active[PASSES] = {};
  u32 active_count = 0;
  for (u32 p = 0; p < PASSES; ++p) {
    const u32 digit = nx::cast<u32>(
        (keys[0] >> (p * detail::RADIX_DIGIT_BITS)) & detail::RADIX_DIGIT_MASK);
    active[p] = histogram[p][digit] != nx::cast<u32>(count);
    active_count += active[p] ? 1u : 0u;
  }
  if (active_count == 0)
    return; // every key identical

  // With an odd number of moves the result ends up in scratch, so start there
  // and let the ping-pong land it back in the caller's buffers for free.
  Key *keys_src = keys.data();
  Value *values_src = values.data();
  Key *keys_dst = key_scratch.data();
  Value *values_dst = value_scratch.data();
  if ((active_count & 1u) != 0) {
    std::memcpy(key_scratch.data(), keys.data(), count * sizeof(Key));
    std::memcpy(value_scratch.data(), values.data(), count * sizeof(Value));
    keys_src = key_scratch.data();
    values_src = value_scratch.data();
    keys_dst = keys.data();
    values_dst = values.data();
  }

  for (u32 p = 0; p < PASSES; ++p) {
    if (!active[p])
      continue;

    u32 *counts = histogram[p];
    // Exclusive prefix sum turns the histogram into output offsets.
    u32 total = 0;
    for (u32 b = 0; b < detail::RADIX_BUCKETS; ++b) {
      const u32 c = counts[b];
      counts[b] = total;
      total += c;
    }

    const u32 shift = p * detail::RADIX_DIGIT_BITS;
    // Ascending iteration is what makes this stable: equal keys leave in the
    // order they arrived.
    for (usize i = 0; i < count; ++i) {
      const u32 digit =
          nx::cast<u32>((keys_src[i] >> shift) & detail::RADIX_DIGIT_MASK);
      const u32 slot = counts[digit]++;
      keys_dst[slot] = keys_src[i];
      values_dst[slot] = values_src[i];
    }

    Key *tk = keys_src;
    keys_src = keys_dst;
    keys_dst = tk;
    Value *tv = values_src;
    values_src = values_dst;
    values_dst = tv;
  }
}

} // namespace nx
