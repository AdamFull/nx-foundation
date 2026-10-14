
#include "framework/nxtest.h"

#include "core/foundation/containers/stream_compact.h"
#include "core/foundation/core/hash.h"

#include <span>
#include <vector>

namespace {

struct Item {
  u32 value;
  u32 orig;
};

std::vector<Item> reference_filter(const std::vector<Item> &in) {
  std::vector<Item> out;
  for (const Item &it : in)
    if (it.value % 3u == 0u)
      out.push_back(it);
  return out;
}

bool keep_item(const Item &it) { return it.value % 3u == 0u; }

std::vector<Item> make_items(u32 count) {
  std::vector<Item> v(count);
  for (u32 i = 0; i < count; ++i)
    v[i] = {nx::hash_u32(i), i};
  return v;
}

bool same(const std::vector<Item> &a, const std::vector<Item> &b, usize count) {
  if (a.size() < count || b.size() < count)
    return false;
  for (usize i = 0; i < count; ++i)
    if (a[i].value != b[i].value || a[i].orig != b[i].orig)
      return false;
  return true;
}

}

TEST_CASE("compact: serial keeps matching elements in order") {
  std::vector<int> in{1, 3, 4, 6, 7, 9, 10};
  std::vector<int> out(in.size());
  const usize n = nx::compact_context::compact_serial(
      std::span<const int>(in), std::span<int>(out),
      [](int x) { return x % 3 == 0; });
  CHECK(n == 3u);
  CHECK(out[0] == 3);
  CHECK(out[1] == 6);
  CHECK(out[2] == 9);
}

TEST_CASE("compact: the parallel path matches the serial filter exactly") {
  constexpr u32 COUNT = 60000;
  const std::vector<Item> in = make_items(COUNT);
  const std::vector<Item> ref = reference_filter(in);

  nx::thread_pool pool;
  nx::compact_context ctx;
  std::vector<Item> out(COUNT);
  const usize n = ctx.compact_parallel(std::span<const Item>(in),
                                       std::span<Item>(out), keep_item, pool);

  CHECK(n == ref.size());
  CHECK(same(out, ref, n));
}

TEST_CASE("compact: transform_compact run from inside a pool task is exact") {
  constexpr u32 COUNT = 60000;
  const std::vector<Item> in = make_items(COUNT);
  const std::vector<Item> ref = reference_filter(in);

  nx::thread_pool pool;
  nx::compact_context ctx;
  std::vector<Item> scratch(COUNT), out(COUNT);
  usize n = 0;
  nx::task_group group;
  pool.submit(group, [&] {
    n = ctx.transform_compact_parallel(
        COUNT, std::span<Item>(scratch), std::span<Item>(out),
        [&](const usize i, Item &item) {
          item = in[i];
          return keep_item(item);
        },
        pool);
  });
  group.wait();

  CHECK(n == ref.size());
  CHECK(same(out, ref, n));
}

TEST_CASE("compact: parallel is stable across chunk boundaries") {
  constexpr u32 COUNT = 50000;
  std::vector<Item> in(COUNT);
  for (u32 i = 0; i < COUNT; ++i)
    in[i] = {0u, i};

  nx::thread_pool pool;
  nx::compact_context ctx;
  std::vector<Item> out(COUNT);
  const usize n = ctx.compact_parallel(std::span<const Item>(in),
                                       std::span<Item>(out), keep_item, pool);
  REQUIRE(n == COUNT);
  bool ordered = true;
  for (u32 i = 0; i < COUNT; ++i)
    if (out[i].orig != i)
      ordered = false;
  CHECK(ordered);
}

TEST_CASE("compact: all-kept and none-kept extremes") {
  constexpr u32 COUNT = 40000;
  std::vector<Item> all(COUNT), none(COUNT);
  for (u32 i = 0; i < COUNT; ++i) {
    all[i] = {0u, i};
    none[i] = {1u, i};
  }

  nx::thread_pool pool;
  nx::compact_context ctx;
  std::vector<Item> out(COUNT);

  CHECK(ctx.compact_parallel(std::span<const Item>(all), std::span<Item>(out),
                             keep_item, pool) == COUNT);
  CHECK(ctx.compact_parallel(std::span<const Item>(none), std::span<Item>(out),
                             keep_item, pool) == 0u);
}

TEST_CASE("compact: forcing the parallel path on a tiny input is correct") {
  const std::vector<Item> in = make_items(50);
  const std::vector<Item> ref = reference_filter(in);

  nx::thread_pool pool;
  nx::compact_context ctx;
  std::vector<Item> out(50);
  const usize n = ctx.compact_parallel(std::span<const Item>(in),
                                       std::span<Item>(out), keep_item, pool);
  CHECK(n == ref.size());
  CHECK(same(out, ref, n));
}

TEST_CASE("compact: compact() dispatches correctly on both sides of crossover") {
  nx::thread_pool pool;
  nx::compact_context ctx;
  ctx.set_crossover(1000);
  CHECK(ctx.crossover() == 1000u);

  for (u32 count : {500u, 5000u}) {
    const std::vector<Item> in = make_items(count);
    const std::vector<Item> ref = reference_filter(in);
    std::vector<Item> out(count);
    const usize n = ctx.compact(std::span<const Item>(in), std::span<Item>(out),
                                keep_item, pool);
    CHECK(n == ref.size());
    CHECK(same(out, ref, n));
  }
}

TEST_CASE("compact: transform_compact builds and filters in one pass") {
  constexpr u32 COUNT = 60000;
  const auto produce = [](usize i, Item &out) {
    out = {nx::hash_u32(nx::cast<u32>(i)), nx::cast<u32>(i)};
    return out.value % 3u == 0u;
  };

  std::vector<Item> ref;
  for (u32 i = 0; i < COUNT; ++i) {
    Item it{};
    if (produce(i, it))
      ref.push_back(it);
  }

  nx::thread_pool pool;
  nx::compact_context ctx;
  std::vector<Item> scratch(COUNT), out(COUNT);
  const usize n = ctx.transform_compact_parallel(
      COUNT, std::span<Item>(scratch), std::span<Item>(out), produce, pool);

  CHECK(n == ref.size());
  CHECK(same(out, ref, n));

  std::vector<Item> out2(COUNT);
  const usize n2 = nx::compact_context::transform_compact_serial(
      COUNT, std::span<Item>(out2), produce);
  CHECK(n2 == ref.size());
  CHECK(same(out2, ref, n2));
}

TEST_CASE("compact: calibrate sets a sane crossover") {
  nx::thread_pool pool;
  nx::compact_context ctx;
  ctx.calibrate(
      pool, [](usize i) { return Item{nx::hash_u32(nx::cast<u32>(i)), 0u}; },
      keep_item);
  CHECK(ctx.crossover() >= nx::compact_context::MIN_CROSSOVER);
  CHECK(ctx.crossover() <= nx::compact_context::MAX_CROSSOVER);
}
