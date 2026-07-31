#pragma once

/**
 * @file stream_compact.h
 * @brief Stable parallel stream compaction (filter): keep the elements a
 * predicate accepts, in order, packed into a contiguous output.
 */

#include "core/foundation/core/foundation.h"
#include "core/foundation/core/time.h"
#include "core/foundation/threading/thread_pool.h"

#include <chrono>
#include <span>
#include <type_traits>

namespace nx {

class compact_context {
public:
  static constexpr usize DEFAULT_CROSSOVER = 32 * 1024;
  static constexpr usize MIN_CROSSOVER = 4 * 1024;
  static constexpr usize MAX_CROSSOVER = 4 * 1024 * 1024;

  [[nodiscard]] usize crossover() const noexcept { return m_crossover; }
  void set_crossover(usize n) noexcept { m_crossover = n; }

  /// Serial compaction: keep in[i] where @p keep(in[i]), in order. Returns the
  /// survivor count. @p out must hold at least in.size() elements.
  template <class T, class Keep>
  static usize compact_serial(std::span<const T> in, std::span<T> out,
                              Keep keep) {
    if (out.size() < in.size())
      return 0;
    usize j = 0;
    for (usize i = 0; i < in.size(); ++i)
      if (keep(in[i]))
        out[j++] = in[i];
    return j;
  }

  /// Serial below the crossover, parallel above it.
  template <class T, class Keep>
  usize compact(std::span<const T> in, std::span<T> out, Keep keep,
                thread_pool &pool) {
    if (in.size() < m_crossover || pool.worker_count() == 0)
      return compact_serial(in, out, keep);
    return compact_parallel(in, out, keep, pool);
  }

  /// Parallel path regardless of size. compact() calls this above the
  /// crossover; exposed so tests exercise it at any size.
  template <class T, class Keep>
  usize compact_parallel(std::span<const T> in, std::span<T> out, Keep keep,
                         thread_pool &pool) {
    const usize n = in.size();
    if (out.size() < n)
      return 0;
    const u32 workers = pool.worker_count();
    if (n == 0 || workers == 0)
      return compact_serial(in, out, keep);

    const u32 chunks = workers + 1; // the caller is a participant too
    const usize per_chunk = (n + chunks - 1) / chunks;
    m_flags.resize(n);
    m_counts.resize(chunks);
    u8 *const flags = m_flags.data();
    u32 *const counts = m_counts.data();
    const T *const src = in.data();
    T *const dst = out.data();

    pool.parallel_region(chunks, [&](const u32 c, u32, barrier &bar) {
      usize begin = nx::cast<usize>(c) * per_chunk;
      if (begin > n)
        begin = n;
      const usize end = nx::min(begin + per_chunk, n);

      // Phase 1: run the predicate once, flag each element, count survivors.
      u32 kept = 0;
      for (usize i = begin; i < end; ++i) {
        const bool k = keep(src[i]);
        flags[i] = k ? u8{1} : u8{0};
        kept += k ? 1u : 0u;
      }
      counts[c] = kept;
      bar.arrive_and_wait();

      // This chunk's output start is the survivors in all earlier chunks.
      usize out_pos = 0;
      for (u32 cc = 0; cc < c; ++cc)
        out_pos += counts[cc];

      // Phase 2: scatter survivors into a range no other chunk touches, in
      // order - so the whole result is a stable filter of the input.
      for (usize i = begin; i < end; ++i)
        if (flags[i])
          dst[out_pos++] = src[i];
    });

    usize total = 0;
    for (u32 c = 0; c < chunks; ++c)
      total += counts[c];
    return total;
  }

  /// Produce-then-filter: for i in [0, count), @p produce(i, elem) fills elem
  /// and returns whether to keep it; kept elements are packed (stable) into
  /// @p out. Fuses a parallel build with the compaction, so the producer (e.g.
  /// building a sprite instance and frustum-testing it) runs across workers.
  /// Returns the survivor count. @p out holds at least @p count elements.
  template <class T, class Produce>
  static usize transform_compact_serial(usize count, std::span<T> out,
                                        Produce produce) {
    if (out.size() < count)
      return 0;
    usize j = 0;
    for (usize i = 0; i < count; ++i)
      if (produce(i, out[j])) // reuses the slot when the element is dropped
        ++j;
    return j;
  }

  /// Serial below the crossover, parallel above it. @p scratch (>= count) holds
  /// produced elements for the parallel path; the serial path ignores it.
  template <class T, class Produce>
  usize transform_compact(usize count, std::span<T> scratch, std::span<T> out,
                          Produce produce, thread_pool &pool) {
    if (count < m_crossover || pool.worker_count() == 0)
      return transform_compact_serial(count, out, produce);
    return transform_compact_parallel(count, scratch, out, produce, pool);
  }

  /// Parallel path regardless of size. Each chunk produces its slice into
  /// @p scratch, then compacts into a disjoint output range.
  template <class T, class Produce>
  usize transform_compact_parallel(usize count, std::span<T> scratch,
                                   std::span<T> out, Produce produce,
                                   thread_pool &pool) {
    if (scratch.size() < count || out.size() < count)
      return 0;
    const u32 workers = pool.worker_count();
    if (count == 0 || workers == 0)
      return transform_compact_serial(count, out, produce);

    const u32 chunks = workers + 1;
    const usize per_chunk = (count + chunks - 1) / chunks;
    m_flags.resize(count);
    m_counts.resize(chunks);
    u8 *const flags = m_flags.data();
    u32 *const counts = m_counts.data();
    T *const scr = scratch.data();
    T *const dst = out.data();

    pool.parallel_region(chunks, [&](const u32 c, u32, barrier &bar) {
      usize begin = nx::cast<usize>(c) * per_chunk;
      if (begin > count)
        begin = count;
      const usize end = nx::min(begin + per_chunk, count);

      u32 kept = 0;
      for (usize i = begin; i < end; ++i) {
        const bool k = produce(i, scr[i]);
        flags[i] = k ? u8{1} : u8{0};
        kept += k ? 1u : 0u;
      }
      counts[c] = kept;
      bar.arrive_and_wait();

      usize out_pos = 0;
      for (u32 cc = 0; cc < c; ++cc)
        out_pos += counts[cc];
      for (usize i = begin; i < end; ++i)
        if (flags[i])
          dst[out_pos++] = scr[i];
    });

    usize total = 0;
    for (u32 c = 0; c < chunks; ++c)
      total += counts[c];
    return total;
  }

  /// Measures the serial/parallel compaction crossover for @p keep on this
  /// machine and sets it. The crossover depends on the predicate's cost, so
  /// callers pass a representative @p gen(i) -> T and their real predicate.
  /// Probe ladder, first clear win, conservative - as radix_context does.
  template <class GenElem, class Keep>
  void calibrate(thread_pool &pool, GenElem gen, Keep keep) {
    using T = std::decay_t<decltype(gen(usize{0}))>;
    if (pool.worker_count() == 0) {
      m_crossover = ~usize{0};
      return;
    }

    static constexpr usize probes[] = {8192,  16384, 24576, 32768,
                                       49152, 65536, 98304, 131072};
    constexpr usize N = 131072;
    nx::vector<T> in(N), out(N);
    for (usize i = 0; i < N; ++i)
      in[i] = gen(i);

    const auto best_us = [&](const usize n, bool parallel) {
      double best = 1e30;
      for (int r = 0; r < 3; ++r) {
        if (parallel)
          nx::this_thread::sleep_for(std::chrono::milliseconds(1));
        const nx::stopwatch watch;
        if (parallel)
          compact_parallel(std::span<const T>(in.data(), n),
                           std::span<T>(out.data(), n), keep, pool);
        else
          compact_serial(std::span<const T>(in.data(), n),
                         std::span<T>(out.data(), n), keep);
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

private:
  usize m_crossover = DEFAULT_CROSSOVER;
  nx::vector<u8> m_flags;
  nx::vector<u32> m_counts;
};

} // namespace nx
