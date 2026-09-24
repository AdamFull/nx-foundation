#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/core/time.h"
#include "core/foundation/threading/thread_pool.h"

#include <chrono>
#include <concepts>
#include <cstring>
#include <span>
#include <utility>

namespace nx {

namespace detail {

inline constexpr u32 RADIX_DIGIT_BITS = 11;
inline constexpr u32 RADIX_BUCKETS = 1u << RADIX_DIGIT_BITS;
inline constexpr u32 RADIX_DIGIT_MASK = RADIX_BUCKETS - 1;

/// Passes needed to cover a key of @p bits, rounded up.
[[nodiscard]] constexpr u32 radix_pass_count(const u32 bits) noexcept {
  return (bits + RADIX_DIGIT_BITS - 1) / RADIX_DIGIT_BITS;
}

}

/// Keys must be unsigned: the digit extraction is a shift and mask, and a
/// signed key would sort negatives after positives.
template <typename T>
concept radix_key =
    std::unsigned_integral<T> && (sizeof(T) == 4 || sizeof(T) == 8);

/// Sorts @p keys ascending, permuting @p values alongside. Scratch buffers must each be at least
/// keys.size(). The payload is normally a 4-byte index gathered once at the end, not the elements
/// themselves.
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

  u32 histogram[PASSES][detail::RADIX_BUCKETS] = {};
  for (usize i = 0; i < count; ++i) {
    const Key k = keys[i];
    for (u32 p = 0; p < PASSES; ++p)
      ++histogram[p][nx::cast<u32>((k >> (p * detail::RADIX_DIGIT_BITS)) &
                                   detail::RADIX_DIGIT_MASK)];
  }

  bool active[PASSES] = {};
  u32 active_count = 0;
  for (u32 p = 0; p < PASSES; ++p) {
    const u32 digit = nx::cast<u32>(
        (keys[0] >> (p * detail::RADIX_DIGIT_BITS)) & detail::RADIX_DIGIT_MASK);
    active[p] = histogram[p][digit] != nx::cast<u32>(count);
    active_count += active[p] ? 1u : 0u;
  }
  if (active_count == 0)
    return;

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

/// Not thread-safe: one context per caller.
class radix_context {
public:
  static constexpr usize DEFAULT_CROSSOVER = 128 * 1024;
  static constexpr usize MIN_CROSSOVER = 8 * 1024;
  static constexpr usize MAX_CROSSOVER = 4 * 1024 * 1024;

  [[nodiscard]] usize crossover() const noexcept { return m_crossover; }
  void set_crossover(usize n) noexcept { m_crossover = n; }

  template <radix_key Key, typename Value>
  void sort(std::span<Key> keys, std::span<Value> values,
            std::span<Key> key_scratch, std::span<Value> value_scratch,
            thread_pool &pool) {
    if (keys.size() < m_crossover || pool.worker_count() == 0) {
      radix_sort_pairs(keys, values, key_scratch, value_scratch);
      return;
    }
    sort_parallel(keys, values, key_scratch, value_scratch, pool);
  }

  static u32 default_calibration_key(usize i) noexcept {
    u32 x = nx::cast<u32>(i) * 2654435761u;
    x ^= x >> 15;
    x *= 2246822519u;
    return x ^ (x >> 13);
  }

  void calibrate(thread_pool &pool) {
    calibrate(pool, &default_calibration_key);
  }

  template <class KeyGen> void calibrate(thread_pool &pool, KeyGen key_gen) {
    if (pool.worker_count() == 0) {
      m_crossover = ~usize{0};
      return;
    }

    static constexpr usize probes[] = {16384,  32768,  49152,  65536, 98304,
                                       131072, 196608, 262144, 393216};
    constexpr usize N = 393216;
    nx::vector<u32> keys(N), vals(N), ks(N), vs(N);
    const auto fill = [&](const usize n) {
      for (usize i = 0; i < n; ++i) {
        keys[i] = key_gen(i);
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
        const nx::stopwatch watch;
        if (parallel)
          sort_parallel(
              std::span<u32>(keys.data(), n), std::span<u32>(vals.data(), n),
              std::span<u32>(ks.data(), n), std::span<u32>(vs.data(), n), pool);
        else
          radix_sort_pairs(
              std::span<u32>(keys.data(), n), std::span<u32>(vals.data(), n),
              std::span<u32>(ks.data(), n), std::span<u32>(vs.data(), n));
        const f64 us = watch.elapsed().micros_f();
        best = us < best ? us : best;
      }
      return best;
    };

    usize cross = MAX_CROSSOVER;
    for (usize n : probes) {
      if (best_us(n, true) < 0.9 * best_us(n, false)) {
        cross = n;
        break;
      }
    }
    m_crossover = nx::min(nx::max(cross, MIN_CROSSOVER), MAX_CROSSOVER);
  }

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

    const u32 chunks = workers + 1;
    const usize per_chunk = (count + chunks - 1) / chunks;
    m_histograms.resize(nx::cast<usize>(chunks) * PASSES *
                        detail::RADIX_BUCKETS);
    m_offsets.resize(nx::cast<usize>(chunks) * detail::RADIX_BUCKETS);
    u32 *const hist = m_histograms.data();
    u32 *const offs = m_offsets.data();
    const auto hrow = [=](const usize chunk, const u32 pass) {
      return hist + (chunk * PASSES + pass) * detail::RADIX_BUCKETS;
    };
    const auto orow = [=](const usize chunk) {
      return offs + chunk * detail::RADIX_BUCKETS;
    };
    const auto slice = [=](const usize chunk) {
      const usize begin = nx::min(chunk * per_chunk, count);
      return std::pair{begin, nx::min(begin + per_chunk, count)};
    };
    // Each phase is its own fork and join over the chunks, so a phase starts
    // on whichever workers are free instead of waiting for a whole team.
    const auto each_chunk = [&](auto &&body) {
      pool.parallel_for(0, chunks, 1, body);
    };

    each_chunk([&](const usize c) {
      const auto [begin, end] = slice(c);
      for (u32 p = 0; p < PASSES; ++p)
        std::memset(hrow(c, p), 0, detail::RADIX_BUCKETS * sizeof(u32));
      for (usize i = begin; i < end; ++i) {
        const Key k = keys[i];
        for (u32 p = 0; p < PASSES; ++p)
          ++hrow(c, p)[nx::cast<u32>((k >> (p * detail::RADIX_DIGIT_BITS)) &
                                     detail::RADIX_DIGIT_MASK)];
      }
    });

    bool active[PASSES] = {};
    u32 active_count = 0;
    for (u32 p = 0; p < PASSES; ++p) {
      const u32 digit =
          nx::cast<u32>((keys[0] >> (p * detail::RADIX_DIGIT_BITS)) &
                        detail::RADIX_DIGIT_MASK);
      u32 total = 0;
      for (usize c = 0; c < chunks; ++c)
        total += hrow(c, p)[digit];
      active[p] = total != nx::cast<u32>(count);
      active_count += active[p] ? 1u : 0u;
    }
    if (active_count == 0)
      return;

    Key *ks = keys.data();
    Value *vs = values.data();
    Key *kd = key_scratch.data();
    Value *vd = value_scratch.data();

    bool first = true;
    for (u32 p = 0; p < PASSES; ++p) {
      if (!active[p])
        continue;
      const u32 shift = p * detail::RADIX_DIGIT_BITS;

      // The first histograms counted the input; later passes count the
      // previous pass's output, which each chunk slices differently.
      if (!first)
        each_chunk([&](const usize c) {
          const auto [begin, end] = slice(c);
          u32 *const h = hrow(c, p);
          std::memset(h, 0, detail::RADIX_BUCKETS * sizeof(u32));
          for (usize i = begin; i < end; ++i)
            ++h[nx::cast<u32>((ks[i] >> shift) & detail::RADIX_DIGIT_MASK)];
        });
      first = false;

      u32 running = 0;
      for (u32 b = 0; b < detail::RADIX_BUCKETS; ++b)
        for (usize c = 0; c < chunks; ++c) {
          orow(c)[b] = running;
          running += hrow(c, p)[b];
        }

      each_chunk([&](const usize c) {
        const auto [begin, end] = slice(c);
        u32 *const off = orow(c);
        for (usize i = begin; i < end; ++i) {
          const u32 digit =
              nx::cast<u32>((ks[i] >> shift) & detail::RADIX_DIGIT_MASK);
          const u32 slot = off[digit]++;
          kd[slot] = ks[i];
          vd[slot] = vs[i];
        }
      });

      Key *tk = ks;
      ks = kd;
      kd = tk;
      Value *tv = vs;
      vs = vd;
      vd = tv;
    }

    if (ks != keys.data())
      each_chunk([&](const usize c) {
        const auto [begin, end] = slice(c);
        for (usize i = begin; i < end; ++i) {
          keys[i] = ks[i];
          values[i] = vs[i];
        }
      });
  }

private:
  usize m_crossover = DEFAULT_CROSSOVER;
  nx::vector<u32> m_histograms;
  nx::vector<u32> m_offsets;
};

/// Buffers sort_by_key reuses between calls. One per caller; not thread-safe.
template <typename T> struct sort_scratch {
  thread_pool *pool = nullptr;
  radix_context context;

  nx::vector<u32> keys, indices, key_scratch, index_scratch;
  nx::vector<T> gathered;
};

namespace detail {

template <typename T, typename KeyFn>
[[nodiscard]] bool sort_permutation(const std::span<const T> src,
                                    sort_scratch<T> &s, KeyFn key) {
  const usize count = src.size();
  s.keys.resize(count);
  s.indices.resize(count);
  bool ordered = true;
  for (usize i = 0; i < count; ++i) {
    const u32 k = key(src[i]);
    ordered = ordered && (i == 0 || s.keys[i - 1] <= k);
    s.keys[i] = k;
    s.indices[i] = nx::cast<u32>(i);
  }
  if (ordered)
    return false;

  s.key_scratch.resize(count);
  s.index_scratch.resize(count);
  const std::span<u32> keys(s.keys.data(), count);
  const std::span<u32> indices(s.indices.data(), count);
  const std::span<u32> key_scratch(s.key_scratch.data(), count);
  const std::span<u32> index_scratch(s.index_scratch.data(), count);
  if (s.pool != nullptr)
    s.context.sort(keys, indices, key_scratch, index_scratch, *s.pool);
  else
    radix_sort_pairs(keys, indices, key_scratch, index_scratch);
  return true;
}

}

/// The radix runs on (key, index) pairs and one gather applies the permutation, so an element is
/// copied once rather than once per pass.
template <typename T, typename KeyFn>
void sort_by_key(nx::vector<T> &items, sort_scratch<T> &scratch, KeyFn key) {
  const usize count = items.size();
  if (count < 2)
    return;
  if (!detail::sort_permutation(std::span<const T>(items.data(), count),
                                scratch, key))
    return;

  scratch.gathered.resize(count);
  for (usize i = 0; i < count; ++i)
    scratch.gathered[i] = items[scratch.indices[i]];
  items.swap(scratch.gathered);
}

template <typename T, typename KeyFn>
void sort_by_key_into(const std::span<const T> src, nx::vector<T> &dst,
                      sort_scratch<T> &scratch, KeyFn key) {
  const usize count = src.size();
  if (dst.size() < count)
    dst.resize(count);
  if (count < 2) {
    if (count == 1)
      dst[0] = src[0];
    return;
  }

  if (!detail::sort_permutation(src, scratch, key)) {
    for (usize i = 0; i < count; ++i)
      dst[i] = src[i];
    return;
  }
  for (usize i = 0; i < count; ++i)
    dst[i] = src[scratch.indices[i]];
}

}
