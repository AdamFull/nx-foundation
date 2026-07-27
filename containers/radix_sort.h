#pragma once

/**
 * @file radix_sort.h
 * @brief Stable LSD radix sort over unsigned 32- or 64-bit keys.
 *
 * Linear in n and stable by construction, so ties never reorder between runs.
 * Beats a comparison sort once n is large enough to hide the O(log n) factor.
 *
 * Digits are 11 bits: 2048 counters stay in L1 where a 16-bit digit's 65536
 * would not, and 11 bits needs fewer passes than 8. All histograms are built
 * in one read up front, which lets a digit whose values all agree be skipped
 * and fixes the pass count before any data moves - so a u64 whose values fit
 * in 32 bits costs the same as a u32. Nothing is allocated: the caller
 * supplies scratch the size of the input.
 */

#include "foundation/core/foundation.h"
#include "foundation/threading/thread_pool.h"

#include <chrono>
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
 * @brief Sorts @p keys ascending, permuting @p values alongside. Scratch
 * buffers must each be at least keys.size(). The payload is normally a 4-byte
 * index gathered once at the end, not the elements themselves.
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

/**
 * @brief Parallel stable LSD radix sort.
 *
 * Splits the input into one chunk per pool worker plus the caller. Each chunk
 * builds local histograms, which are converted into disjoint per-chunk offsets
 * for a stable scatter without atomics.
 *
 * All passes run inside one parallel_region, with barriers between phases.
 * For small inputs, sort() uses the serial implementation; calibrate() finds
 * the crossover point for the current machine.
 *
 * Reuses internal buffers between calls. Not thread-safe: use one context per
 * caller, and do not call it from a pool worker.
 */
class radix_context {
public:
  /// Default crossover until calibrate() runs: the measured desktop break-even.
  static constexpr usize DEFAULT_CROSSOVER = 128 * 1024;
  static constexpr usize MIN_CROSSOVER = 8 * 1024;
  static constexpr usize MAX_CROSSOVER = 4 * 1024 * 1024;

  [[nodiscard]] usize crossover() const noexcept { return crossover_; }
  void set_crossover(usize n) noexcept { crossover_ = n; }

  /// Serial below the crossover, the parallel region above it.
  template <radix_key Key, typename Value>
  void sort(std::span<Key> keys, std::span<Value> values,
            std::span<Key> key_scratch, std::span<Value> value_scratch,
            thread_pool &pool) {
    if (keys.size() < crossover_ || pool.worker_count() == 0) {
      radix_sort_pairs(keys, values, key_scratch, value_scratch);
      return;
    }
    sort_parallel(keys, values, key_scratch, value_scratch, pool);
  }

  void calibrate(thread_pool &pool) {
    if (pool.worker_count() == 0) {
      crossover_ = ~usize{0}; // no team: never parallel
      return;
    }

    static constexpr usize probes[] = {16384,  32768,  49152,  65536, 98304,
                                       131072, 196608, 262144, 393216};
    constexpr usize N = 393216;
    nx::vector<u32> keys(N), vals(N), ks(N), vs(N);
    const auto fill = [&](const usize n) {
      for (usize i = 0; i < n; ++i) {
        u32 x = nx::cast<u32>(i) * 2654435761u;
        x ^= x >> 15;
        x *= 2246822519u;
        x ^= x >> 13;
        keys[i] =
            ((x & 15u) << 20) | (((x >> 4) & 4095u) << 8) | ((x >> 16) & 63u);
        vals[i] = nx::cast<u32>(i);
      }
    };
    const auto best_us = [&](const usize n, bool parallel) {
      double best = 1e30;
      for (int r = 0; r < 3; ++r) {
        fill(n);
        // Let the pool idle first: per frame the workers sleep between sorts,
        // so the region pays a cold wake-up. Timing it warm understates the
        // fixed cost and places the crossover too low.
        if (parallel)
          nx::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto t0 = std::chrono::steady_clock::now();
        if (parallel)
          sort_parallel(
              std::span<u32>(keys.data(), n), std::span<u32>(vals.data(), n),
              std::span<u32>(ks.data(), n), std::span<u32>(vs.data(), n), pool);
        else
          radix_sort_pairs(
              std::span<u32>(keys.data(), n), std::span<u32>(vals.data(), n),
              std::span<u32>(ks.data(), n), std::span<u32>(vs.data(), n));
        const double us = std::chrono::duration<double, std::micro>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        best = us < best ? us : best;
      }
      return best;
    };

    usize cross = MAX_CROSSOVER; // parallel never clearly wins -> stay serial
    for (usize n : probes) {
      // Require a 10% margin so a near-tie (where the two are interchangeable)
      // does not flip the switch on noise.
      if (best_us(n, true) < 0.9 * best_us(n, false)) {
        cross = n;
        break;
      }
    }
    crossover_ = nx::min(nx::max(cross, MIN_CROSSOVER), MAX_CROSSOVER);
  }

  /// The parallel region path regardless of size. sort() calls this above the
  /// crossover; exposed directly so tests exercise it at any size. Falls back
  /// to serial only when the pool has no workers.
  template <radix_key Key, typename Value>
  void sort_parallel(std::span<Key> keys, std::span<Value> values,
                     std::span<Key> key_scratch, std::span<Value> value_scratch,
                     thread_pool &pool) {
    const usize count = keys.size();
    if (count < 2 || values.size() < count || key_scratch.size() < count ||
        value_scratch.size() < count)
      return;

    constexpr u32 PASSES =
        detail::radix_pass_count(nx::cast<u32>(sizeof(Key) * 8));

    const u32 workers = pool.worker_count();
    if (workers == 0) {
      radix_sort_pairs(keys, values, key_scratch, value_scratch);
      return;
    }

    const u32 chunks = workers + 1; // the caller is a participant too
    const usize per_chunk = (count + chunks - 1) / chunks;
    histograms_.resize(nx::cast<usize>(chunks) * PASSES *
                       detail::RADIX_BUCKETS);
    offsets_.resize(nx::cast<usize>(chunks) * detail::RADIX_BUCKETS);
    u32 *const hist = histograms_.data();
    u32 *const offs = offsets_.data();
    const auto hrow = [=](const u32 chunk, const u32 pass) {
      return hist +
             ((nx::cast<usize>(chunk) * PASSES + pass) * detail::RADIX_BUCKETS);
    };
    const auto orow = [=](const u32 chunk) {
      return offs + nx::cast<usize>(chunk) * detail::RADIX_BUCKETS;
    };

    pool.parallel_region(chunks, [&](const u32 c, const u32 n, barrier &bar) {
      usize begin = nx::cast<usize>(c) * per_chunk;
      if (begin > count)
        begin = count;
      const usize end = nx::min(begin + per_chunk, count);

      // Phase A: this chunk's histogram, every pass, in one read of its slice.
      for (u32 p = 0; p < PASSES; ++p)
        std::memset(hrow(c, p), 0, detail::RADIX_BUCKETS * sizeof(u32));
      for (usize i = begin; i < end; ++i) {
        const Key k = keys[i];
        for (u32 p = 0; p < PASSES; ++p)
          ++hrow(c, p)[nx::cast<u32>((k >> (p * detail::RADIX_DIGIT_BITS)) &
                                     detail::RADIX_DIGIT_MASK)];
      }
      bar.arrive_and_wait();

      // Every participant derives the same plan from the shared histograms:
      // per-digit totals are order-independent, so no shared write and no
      // barrier.
      bool active[PASSES] = {};
      u32 active_count = 0;
      for (u32 p = 0; p < PASSES; ++p) {
        const u32 digit =
            nx::cast<u32>((keys[0] >> (p * detail::RADIX_DIGIT_BITS)) &
                          detail::RADIX_DIGIT_MASK);
        u32 total = 0;
        for (u32 cc = 0; cc < n; ++cc)
          total += hrow(cc, p)[digit];
        active[p] = total != nx::cast<u32>(count);
        active_count += active[p] ? 1u : 0u;
      }
      if (active_count == 0)
        return; // every key identical

      // Always scatter keys -> scratch; if an odd pass count leaves the result
      // in scratch, copy it home at the end. Copying last, each chunk over its
      // own output slice, avoids the barrier a copy-first scheme needs to stop
      // one chunk clobbering input another still reads.
      Key *ks = keys.data();
      Value *vs = values.data();
      Key *kd = key_scratch.data();
      Value *vd = value_scratch.data();

      bool first = true;
      for (u32 p = 0; p < PASSES; ++p) {
        if (!active[p])
          continue;
        const u32 shift = p * detail::RADIX_DIGIT_BITS;

        // Phase A matches the first active pass; later passes moved the data.
        if (!first) {
          std::memset(hrow(c, p), 0, detail::RADIX_BUCKETS * sizeof(u32));
          for (usize i = begin; i < end; ++i)
            ++hrow(
                c,
                p)[nx::cast<u32>((ks[i] >> shift) & detail::RADIX_DIGIT_MASK)];
          bar.arrive_and_wait();
        }
        first = false;

        // Start offset for each bucket: all lower buckets, then this bucket's
        // earlier chunks. Written only to this chunk's own row.
        u32 *off = orow(c);
        u32 running = 0;
        for (u32 b = 0; b < detail::RADIX_BUCKETS; ++b) {
          for (u32 cc = 0; cc < n; ++cc) {
            const u32 cnt = hrow(cc, p)[b];
            if (cc == c)
              off[b] = running;
            running += cnt;
          }
        }

        for (usize i = begin; i < end; ++i) { // ascending i keeps it stable
          const u32 digit =
              nx::cast<u32>((ks[i] >> shift) & detail::RADIX_DIGIT_MASK);
          const u32 slot = off[digit]++;
          kd[slot] = ks[i];
          vd[slot] = vs[i];
        }
        bar.arrive_and_wait();

        Key *tk = ks;
        ks = kd;
        kd = tk;
        Value *tv = vs;
        vs = vd;
        vd = tv;
      }

      if (ks != keys.data()) // odd pass count: the result is in scratch
        for (usize i = begin; i < end; ++i) {
          keys[i] = ks[i];
          values[i] = vs[i];
        }
    });
  }

private:
  usize crossover_ = DEFAULT_CROSSOVER;
  nx::vector<u32> histograms_;
  nx::vector<u32> offsets_;
};

} // namespace nx
