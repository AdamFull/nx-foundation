#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/core/time.h"
#include "core/foundation/threading/thread_pool.h"

#include <chrono>
#include <concepts>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>
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

/// Values travel with their keys by copy and memcpy.
template <typename T>
concept radix_value = std::is_trivially_copyable_v<T>;

/// sort_by_key sorts 32-bit keys; a wider or signed key would be cut or
/// reordered on the way in.
template <typename F, typename T>
concept sort_key_fn =
    std::invocable<F &, const T &> &&
    std::unsigned_integral<std::invoke_result_t<F &, const T &>> &&
    sizeof(std::invoke_result_t<F &, const T &>) <= sizeof(u32);

namespace detail {

[[nodiscard]] inline bool
radix_buffers_fit(const usize count, const usize values,
                  const usize key_scratch, const usize value_scratch) noexcept {
  const bool fit = values >= count && key_scratch >= count &&
                   value_scratch >= count &&
                   count <= std::numeric_limits<u32>::max();
  NX_ASSERT(fit &&
            "radix sort: a buffer is shorter than the keys, or there are "
            "more than 2^32 keys");
  return fit;
}

}

/// Sorts @p keys ascending, permuting @p values alongside. Scratch buffers must each be at least
/// keys.size(). The payload is normally a 4-byte index gathered once at the end, not the elements
/// themselves.
template <radix_key Key, radix_value Value>
void radix_sort_pairs(const std::span<Key> keys, const std::span<Value> values,
                      const std::span<Key> key_scratch,
                      const std::span<Value> value_scratch) {
  const usize count = keys.size();
  if (count < 2 ||
      !detail::radix_buffers_fit(count, values.size(), key_scratch.size(),
                                 value_scratch.size()))
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

  template <radix_key Key, radix_value Value>
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

  template <radix_key Key, radix_value Value>
  void sort_parallel(std::span<Key> keys, std::span<Value> values,
                     std::span<Key> key_scratch, std::span<Value> value_scratch,
                     thread_pool &pool) {
    const usize count = keys.size();
    if (count < 2 ||
        !detail::radix_buffers_fit(count, values.size(), key_scratch.size(),
                                   value_scratch.size()))
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
  /// How many elements `indices` put in order last, for
  /// coherent_sort_by_key_into to start from; zero when nothing is reusable.
  usize ordered = 0;
  /// Whether the last coherent sort finished from that order, without the
  /// radix.
  bool reused = false;
};

namespace detail {

/// Below this, the element passes around the radix are cheaper serial than a
/// fork and join.
inline constexpr usize SORT_PARALLEL_PASS_MIN = 8 * 1024;

/// Fewer elements out of place than this are insertion-sorted, not radix
/// sorted.
inline constexpr usize COHERENT_SMALL_SIDE = 64;
/// With more than one element in this many out of place, the radix over all of
/// them is the cheaper sort: about 17 ns an element, against a few for the
/// split and merge plus two radix passes over those set aside.
inline constexpr usize COHERENT_MAX_SIDE_SHARE = 4;
/// How far back in the run an element that does not fit may land, setting
/// aside the ones above it, before it is set aside itself.
inline constexpr usize COHERENT_LOOK_BACK = 4;

/// Runs @p body(i) for every index, across @p pool when it pays.
template <typename Body>
void for_each_index(thread_pool *const pool, const usize count, Body &&body) {
  if (pool != nullptr && pool->worker_count() > 0 &&
      count >= SORT_PARALLEL_PASS_MIN)
    pool->parallel_for(0, count, body);
  else
    for (usize i = 0; i < count; ++i)
      body(i);
}

template <typename T, typename KeyFn>
[[nodiscard]] bool sort_permutation(const std::span<const T> src,
                                    sort_scratch<T> &s, KeyFn key) {
  const usize count = src.size();
  s.keys.resize(count);
  s.indices.resize(count);
  u32 *const keys_out = s.keys.data();
  u32 *const indices_out = s.indices.data();
  for_each_index(s.pool, count, [&](const usize i) {
    keys_out[i] = key(src[i]);
    indices_out[i] = nx::cast<u32>(i);
  });
  bool ordered = true;
  for (usize i = 1; i < count && ordered; ++i)
    ordered = keys_out[i - 1] <= keys_out[i];
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

[[nodiscard]] inline bool pair_less(const u32 key_a, const u32 index_a,
                                    const u32 key_b,
                                    const u32 index_b) noexcept {
  return key_a < key_b || (key_a == key_b && index_a < index_b);
}

/// Insertion-sorts (key, index) pairs by key, ties by index: the order the
/// stable radix gives.
inline void insertion_sort_pairs(u32 *const keys, u32 *const indices,
                                 const usize count) noexcept {
  for (usize i = 1; i < count; ++i) {
    const u32 key = keys[i];
    const u32 index = indices[i];
    usize j = i;
    for (; j > 0 && pair_less(key, index, keys[j - 1], indices[j - 1]); --j) {
      keys[j] = keys[j - 1];
      indices[j] = indices[j - 1];
    }
    keys[j] = key;
    indices[j] = index;
  }
}

/// Orders @p src starting from the permutation that ordered the previous
/// call's elements. Sprites drawn frame after frame barely change order: one
/// pass keeps the run still in order and sets aside the elements that moved,
/// however far; those are sorted alone and merged back. Too many set aside and
/// it gives up for the radix.
template <typename T, typename KeyFn>
[[nodiscard]] bool coherent_permutation(const std::span<const T> src,
                                        sort_scratch<T> &s, KeyFn key) {
  const usize count = src.size();
  if (s.ordered != count || s.keys.size() < count)
    return false;
  s.key_scratch.resize(count);
  s.index_scratch.resize(count);
  u32 *const keys = s.keys.data();
  u32 *const indices = s.indices.data();
  u32 *const side_keys = s.key_scratch.data();
  u32 *const side_indices = s.index_scratch.data();
  for_each_index(s.pool, count,
                 [&](const usize i) { keys[i] = key(src[indices[i]]); });

  // kept <= i, so the run compacts in place ahead of what is still unread.
  const usize max_side = count / COHERENT_MAX_SIDE_SHARE;
  usize kept = 0;
  usize side = 0;
  const auto set_aside = [&](const u32 k, const u32 v) {
    side_keys[side] = k;
    side_indices[side] = v;
    ++side;
  };
  for (usize i = 0; i < count; ++i) {
    const u32 k = keys[i];
    const u32 v = indices[i];
    if (kept == 0 || !pair_less(k, v, keys[kept - 1], indices[kept - 1])) {
      keys[kept] = k;
      indices[kept] = v;
      ++kept;
      continue;
    }
    // Where it fits a little way back, the few above it are the ones that
    // moved ahead; keeping them would set aside everything after.
    const usize lowest =
        kept > COHERENT_LOOK_BACK ? kept - COHERENT_LOOK_BACK : 0;
    usize at = kept - 1;
    while (at > lowest && pair_less(k, v, keys[at - 1], indices[at - 1]))
      --at;
    const bool fits =
        at == 0 || !pair_less(k, v, keys[at - 1], indices[at - 1]);
    if (side + (fits ? kept - at : 1u) > max_side)
      return false;
    if (!fits) {
      set_aside(k, v);
      continue;
    }
    for (usize j = at; j < kept; ++j)
      set_aside(keys[j], indices[j]);
    keys[at] = k;
    indices[at] = v;
    kept = at + 1;
  }

  if (side < COHERENT_SMALL_SIDE) {
    insertion_sort_pairs(side_keys, side_indices, side);
  } else {
    // The run's leftover tail is exactly the scratch a radix needs. By index,
    // then stably by key: ties end in index order.
    const std::span<u32> by_key(side_keys, side);
    const std::span<u32> by_index(side_indices, side);
    const std::span<u32> key_room(keys + kept, side);
    const std::span<u32> index_room(indices + kept, side);
    radix_sort_pairs(by_index, by_key, index_room, key_room);
    radix_sort_pairs(by_key, by_index, key_room, index_room);
  }

  // Merge from the back, so the run is never overwritten before it is read.
  usize a = kept;
  usize b = side;
  usize out = count;
  while (b > 0) {
    --out;
    if (a > 0 && pair_less(side_keys[b - 1], side_indices[b - 1], keys[a - 1],
                           indices[a - 1])) {
      --a;
      keys[out] = keys[a];
      indices[out] = indices[a];
    } else {
      --b;
      keys[out] = side_keys[b];
      indices[out] = side_indices[b];
    }
  }
  return true;
}
}

/// The radix runs on (key, index) pairs and one gather applies the permutation, so an element is
/// copied once rather than once per pass.
template <typename T, sort_key_fn<T> KeyFn>
void sort_by_key(nx::vector<T> &items, sort_scratch<T> &scratch, KeyFn key) {
  const usize count = items.size();
  scratch.ordered = 0;
  if (count < 2)
    return;
  if (!detail::sort_permutation(std::span<const T>(items.data(), count),
                                scratch, key))
    return;

  scratch.gathered.resize(count);
  T *const gathered = scratch.gathered.data();
  const u32 *const indices = scratch.indices.data();
  detail::for_each_index(scratch.pool, count, [&](const usize i) {
    gathered[i] = items[indices[i]];
  });
  items.swap(scratch.gathered);
}

/// Leaves @p dst holding exactly src's elements, in key order.
template <typename T, sort_key_fn<T> KeyFn>
void sort_by_key_into(const std::span<const T> src, nx::vector<T> &dst,
                      sort_scratch<T> &scratch, KeyFn key) {
  const usize count = src.size();
  dst.resize(count);
  scratch.ordered = 0;
  if (count < 2) {
    if (count == 1)
      dst[0] = src[0];
    return;
  }

  T *const out = dst.data();
  if (!detail::sort_permutation(src, scratch, key)) {
    detail::for_each_index(scratch.pool, count,
                           [&](const usize i) { out[i] = src[i]; });
    scratch.ordered = count;
    return;
  }
  const u32 *const indices = scratch.indices.data();
  detail::for_each_index(scratch.pool, count,
                         [&](const usize i) { out[i] = src[indices[i]]; });
  scratch.ordered = count;
}

/// sort_by_key_into for data sorted again and again with little change, such
/// as a frame's sprites: it starts from the order the previous call on this
/// scratch produced whenever the element count is unchanged, and falls back
/// to the radix when that order is too far off. Element i should be the same
/// thing from call to call for the start to help; the result is the same
/// either way.
template <typename T, sort_key_fn<T> KeyFn>
void coherent_sort_by_key_into(const std::span<const T> src, nx::vector<T> &dst,
                               sort_scratch<T> &scratch, KeyFn key) {
  scratch.reused =
      src.size() >= 2 && detail::coherent_permutation(src, scratch, key);
  if (!scratch.reused) {
    sort_by_key_into(src, dst, scratch, key);
    return;
  }

  const usize count = src.size();
  dst.resize(count);
  T *const out = dst.data();
  const u32 *const indices = scratch.indices.data();
  detail::for_each_index(scratch.pool, count,
                         [&](const usize i) { out[i] = src[indices[i]]; });
}
}
