
#include "framework/nxtest.h"

#include "core/foundation/containers/radix_sort.h"
#include "core/foundation/core/hash.h"

#include <algorithm>
#include <vector>

namespace {

void sort_pairs(std::vector<u32> &keys, std::vector<u32> &values) {
  std::vector<u32> ks(keys.size()), vs(values.size());
  nx::radix_sort_pairs(std::span<u32>(keys), std::span<u32>(values),
                       std::span<u32>(ks), std::span<u32>(vs));
}

}

TEST_CASE("radix: keys come out ascending") {
  std::vector<u32> keys{5, 1, 4, 1, 9, 0, 7};
  std::vector<u32> values{0, 1, 2, 3, 4, 5, 6};
  sort_pairs(keys, values);

  for (usize i = 1; i < keys.size(); ++i)
    CHECK(keys[i - 1] <= keys[i]);
  CHECK(keys.front() == 0u);
  CHECK(keys.back() == 9u);
}

TEST_CASE("radix: values follow their keys") {
  std::vector<u32> keys{30, 10, 20};
  std::vector<u32> values{300, 100, 200};
  sort_pairs(keys, values);

  CHECK(values[0] == 100u);
  CHECK(values[1] == 200u);
  CHECK(values[2] == 300u);
}

TEST_CASE("radix: equal keys keep their original order") {
  constexpr u32 COUNT = 500;
  std::vector<u32> keys(COUNT, 42u);
  std::vector<u32> values(COUNT);
  for (u32 i = 0; i < COUNT; ++i)
    values[i] = i;

  sort_pairs(keys, values);

  for (u32 i = 0; i < COUNT; ++i)
    CHECK(values[i] == i);
}

TEST_CASE("radix: ties within distinct keys keep their order") {
  std::vector<u32> keys, values;
  for (u32 i = 0; i < 200; ++i) {
    keys.push_back(i % 4);
    values.push_back(i);
  }
  sort_pairs(keys, values);

  u32 last_of[4] = {0, 0, 0, 0};
  bool ok = true;
  for (usize i = 0; i < keys.size(); ++i) {
    const u32 k = keys[i];
    if (last_of[k] != 0 && values[i] < last_of[k])
      ok = false;
    last_of[k] = values[i];
  }
  CHECK(ok);
}

TEST_CASE("radix: it agrees with a comparison sort on random data") {
  constexpr u32 COUNT = 20000;
  std::vector<u32> keys(COUNT), values(COUNT);
  for (u32 i = 0; i < COUNT; ++i) {
    keys[i] = nx::hash_u32(i);
    values[i] = i;
  }

  std::vector<std::pair<u32, u32>> reference(COUNT);
  for (u32 i = 0; i < COUNT; ++i)
    reference[i] = {keys[i], values[i]};
  std::stable_sort(
      reference.begin(), reference.end(),
      [](const auto &a, const auto &b) { return a.first < b.first; });

  sort_pairs(keys, values);

  bool same = true;
  for (u32 i = 0; i < COUNT; ++i)
    if (keys[i] != reference[i].first || values[i] != reference[i].second)
      same = false;
  CHECK(same);
}

TEST_CASE("radix: the full 32-bit range sorts, including the top bit") {
  std::vector<u32> keys{0xFFFFFFFFu, 0u, 0x80000000u, 0x7FFFFFFFu, 0x00400000u};
  std::vector<u32> values{0, 1, 2, 3, 4};
  sort_pairs(keys, values);

  CHECK(keys[0] == 0u);
  CHECK(keys[1] == 0x00400000u);
  CHECK(keys[2] == 0x7FFFFFFFu);
  CHECK(keys[3] == 0x80000000u);
  CHECK(keys[4] == 0xFFFFFFFFu);
}

TEST_CASE("radix: degenerate inputs are left alone") {
  std::vector<u32> empty_keys, empty_values, s1, s2;
  nx::radix_sort_pairs(std::span<u32>(empty_keys), std::span<u32>(empty_values),
                       std::span<u32>(s1), std::span<u32>(s2));
  CHECK(empty_keys.empty());

  std::vector<u32> one_key{7}, one_value{1};
  sort_pairs(one_key, one_value);
  CHECK(one_key[0] == 7u);

  std::vector<u32> keys{3, 1, 2}, values{0, 1, 2}, small(1);
  nx::radix_sort_pairs(std::span<u32>(keys), std::span<u32>(values),
                       std::span<u32>(small), std::span<u32>(small));
  CHECK(keys[0] == 3u);
}

TEST_CASE("radix: 64-bit keys sort across the full range") {
  std::vector<u64> keys{0xFFFFFFFFFFFFFFFFull, 0ull, 0x8000000000000000ull,
                        0x00000000FFFFFFFFull, 0x0000000100000000ull};
  std::vector<u32> values{0, 1, 2, 3, 4};
  std::vector<u64> ks(keys.size());
  std::vector<u32> vs(values.size());
  nx::radix_sort_pairs(std::span<u64>(keys), std::span<u32>(values),
                       std::span<u64>(ks), std::span<u32>(vs));

  CHECK(keys[0] == 0ull);
  CHECK(keys[1] == 0x00000000FFFFFFFFull);
  CHECK(keys[2] == 0x0000000100000000ull);
  CHECK(keys[3] == 0x8000000000000000ull);
  CHECK(keys[4] == 0xFFFFFFFFFFFFFFFFull);
}

TEST_CASE("radix: 64-bit keys are stable too") {
  constexpr u32 COUNT = 300;
  std::vector<u64> keys(COUNT, 0x1234'5678'9ABCull);
  std::vector<u32> values(COUNT);
  for (u32 i = 0; i < COUNT; ++i)
    values[i] = i;

  std::vector<u64> ks(COUNT);
  std::vector<u32> vs(COUNT);
  nx::radix_sort_pairs(std::span<u64>(keys), std::span<u32>(values),
                       std::span<u64>(ks), std::span<u32>(vs));

  for (u32 i = 0; i < COUNT; ++i)
    CHECK(values[i] == i);
}

TEST_CASE("radix: skipping empty digits does not change the result") {
  constexpr u32 COUNT = 5000;
  std::vector<u32> k32(COUNT), v32(COUNT);
  std::vector<u64> k64(COUNT);
  std::vector<u32> v64(COUNT);
  for (u32 i = 0; i < COUNT; ++i) {
    const u32 k = nx::hash_u32(i) & 0x00FFFFFFu;
    k32[i] = k;
    k64[i] = k;
    v32[i] = i;
    v64[i] = i;
  }

  std::vector<u32> s32(COUNT), sv32(COUNT);
  nx::radix_sort_pairs(std::span<u32>(k32), std::span<u32>(v32),
                       std::span<u32>(s32), std::span<u32>(sv32));
  std::vector<u64> s64(COUNT);
  std::vector<u32> sv64(COUNT);
  nx::radix_sort_pairs(std::span<u64>(k64), std::span<u32>(v64),
                       std::span<u64>(s64), std::span<u32>(sv64));

  bool same = true;
  for (u32 i = 0; i < COUNT; ++i)
    if (k32[i] != nx::cast<u32>(k64[i]) || v32[i] != v64[i])
      same = false;
  CHECK(same);
}

TEST_CASE("radix: an all-identical key set is left untouched") {
  std::vector<u32> keys(100, 7u), values(100);
  for (u32 i = 0; i < 100; ++i)
    values[i] = i;
  std::vector<u32> ks(100), vs(100);
  nx::radix_sort_pairs(std::span<u32>(keys), std::span<u32>(values),
                       std::span<u32>(ks), std::span<u32>(vs));

  for (u32 i = 0; i < 100; ++i)
    CHECK(values[i] == i);
}

TEST_CASE("radix: the parallel sort matches the serial one exactly") {
  constexpr u32 COUNT = 60000;
  std::vector<u32> ka(COUNT), va(COUNT), kb(COUNT), vb(COUNT);
  for (u32 i = 0; i < COUNT; ++i) {
    const u32 k = nx::hash_u32(i) & 0xFFFFFu;
    ka[i] = kb[i] = k;
    va[i] = vb[i] = i;
  }

  std::vector<u32> s1(COUNT), s2(COUNT);
  nx::radix_sort_pairs(std::span<u32>(ka), std::span<u32>(va),
                       std::span<u32>(s1), std::span<u32>(s2));

  nx::thread_pool pool;
  nx::radix_context ctx;
  std::vector<u32> s3(COUNT), s4(COUNT);
  ctx.sort_parallel(std::span<u32>(kb), std::span<u32>(vb), std::span<u32>(s3),
                    std::span<u32>(s4), pool);

  bool same = true;
  for (u32 i = 0; i < COUNT; ++i)
    if (ka[i] != kb[i] || va[i] != vb[i])
      same = false;
  CHECK(same);
}

TEST_CASE("radix: a parallel sort run from inside a pool task is exact") {
  constexpr u32 COUNT = 60000;
  std::vector<u32> ka(COUNT), va(COUNT), kb(COUNT), vb(COUNT);
  for (u32 i = 0; i < COUNT; ++i) {
    ka[i] = kb[i] = nx::hash_u32(i);
    va[i] = vb[i] = i;
  }
  std::vector<u32> s1(COUNT), s2(COUNT), s3(COUNT), s4(COUNT);
  nx::radix_sort_pairs(std::span<u32>(ka), std::span<u32>(va),
                       std::span<u32>(s1), std::span<u32>(s2));

  nx::thread_pool pool;
  nx::radix_context ctx;
  nx::task_group group;
  pool.submit(group, [&] {
    ctx.sort_parallel(std::span<u32>(kb), std::span<u32>(vb),
                      std::span<u32>(s3), std::span<u32>(s4), pool);
  });
  group.wait();

  CHECK(ka == kb);
  CHECK(va == vb);
}

TEST_CASE("radix: calibrate sets a sane crossover and set_crossover steers") {
  nx::thread_pool pool;
  nx::radix_context ctx;
  CHECK(ctx.crossover() == nx::radix_context::DEFAULT_CROSSOVER);

  ctx.calibrate(pool);
  CHECK(ctx.crossover() >= nx::radix_context::MIN_CROSSOVER);
  CHECK(ctx.crossover() <= nx::radix_context::MAX_CROSSOVER);

  ctx.set_crossover(1000);
  CHECK(ctx.crossover() == 1000u);
  for (u32 count : {500u, 5000u}) {
    std::vector<u32> keys(count), values(count), rk(count), rv(count);
    for (u32 i = 0; i < count; ++i) {
      keys[i] = rk[i] = nx::hash_u32(i);
      values[i] = rv[i] = i;
    }
    std::vector<u32> ks(count), vs(count);
    ctx.sort(std::span<u32>(keys), std::span<u32>(values), std::span<u32>(ks),
             std::span<u32>(vs), pool);
    nx::radix_sort_pairs(std::span<u32>(rk), std::span<u32>(rv),
                         std::span<u32>(ks), std::span<u32>(vs));

    bool same = true;
    for (u32 i = 0; i < count; ++i)
      if (keys[i] != rk[i] || values[i] != rv[i])
        same = false;
    CHECK(same);
  }
}

TEST_CASE("radix: the parallel odd-pass path matches the serial one") {
  constexpr u32 COUNT = 70000;
  std::vector<u32> ka(COUNT), va(COUNT), kb(COUNT), vb(COUNT);
  for (u32 i = 0; i < COUNT; ++i) {
    const u32 k = nx::hash_u32(i);
    ka[i] = kb[i] = k;
    va[i] = vb[i] = i;
  }

  std::vector<u32> s1(COUNT), s2(COUNT);
  nx::radix_sort_pairs(std::span<u32>(ka), std::span<u32>(va),
                       std::span<u32>(s1), std::span<u32>(s2));

  nx::thread_pool pool;
  nx::radix_context ctx;
  std::vector<u32> s3(COUNT), s4(COUNT);
  ctx.sort_parallel(std::span<u32>(kb), std::span<u32>(vb), std::span<u32>(s3),
                    std::span<u32>(s4), pool);

  bool same = true;
  for (u32 i = 0; i < COUNT; ++i)
    if (ka[i] != kb[i] || va[i] != vb[i])
      same = false;
  CHECK(same);
}

TEST_CASE("radix: the parallel sort is stable across chunk boundaries") {
  constexpr u32 COUNT = 50000;
  std::vector<u32> keys(COUNT), values(COUNT);
  for (u32 i = 0; i < COUNT; ++i) {
    keys[i] = i % 8;
    values[i] = i;
  }

  nx::thread_pool pool;
  nx::radix_context ctx;
  std::vector<u32> ks(COUNT), vs(COUNT);
  ctx.sort_parallel(std::span<u32>(keys), std::span<u32>(values),
                    std::span<u32>(ks), std::span<u32>(vs), pool);

  u32 previous[8] = {};
  bool ordered = true;
  for (u32 i = 0; i < COUNT; ++i) {
    const u32 k = keys[i];
    if (previous[k] != 0 && values[i] < previous[k])
      ordered = false;
    previous[k] = values[i];
  }
  CHECK(ordered);
}

TEST_CASE("radix: the parallel path falls back below its threshold") {
  std::vector<u32> keys{9, 3, 7, 1}, values{0, 1, 2, 3};
  nx::thread_pool pool;
  nx::radix_context ctx;
  std::vector<u32> ks(4), vs(4);
  ctx.sort(std::span<u32>(keys), std::span<u32>(values), std::span<u32>(ks),
           std::span<u32>(vs), pool);

  CHECK(keys[0] == 1u);
  CHECK(keys[3] == 9u);
  CHECK(values[0] == 3u);
}

TEST_CASE("radix: forcing the parallel path on a tiny input is still correct") {
  constexpr u32 COUNT = 100;
  std::vector<u32> ka(COUNT), va(COUNT), kb(COUNT), vb(COUNT);
  for (u32 i = 0; i < COUNT; ++i) {
    const u32 k = nx::hash_u32(i) & 0xFFu;
    ka[i] = kb[i] = k;
    va[i] = vb[i] = i;
  }

  std::vector<u32> s1(COUNT), s2(COUNT);
  nx::radix_sort_pairs(std::span<u32>(ka), std::span<u32>(va),
                       std::span<u32>(s1), std::span<u32>(s2));

  nx::thread_pool pool;
  nx::radix_context ctx;
  std::vector<u32> s3(COUNT), s4(COUNT);
  ctx.sort_parallel(std::span<u32>(kb), std::span<u32>(vb), std::span<u32>(s3),
                    std::span<u32>(s4), pool);

  bool same = true;
  for (u32 i = 0; i < COUNT; ++i)
    if (ka[i] != kb[i] || va[i] != vb[i])
      same = false;
  CHECK(same);
}

namespace {

struct Item {
  u32 key = 0;
  u32 id = 0;
};

[[nodiscard]] u32 item_key(const Item &i) noexcept { return i.key; }

[[nodiscard]] nx::vector<Item> items(const std::initializer_list<u32> keys) {
  nx::vector<Item> out;
  u32 id = 0;
  for (const u32 k : keys)
    out.push_back(Item{k, id++});
  return out;
}

[[nodiscard]] bool ordered(const nx::vector<Item> &v, const usize count) {
  for (usize i = 1; i < count; ++i)
    if (v[i - 1].key > v[i].key)
      return false;
  return true;
}

}

TEST_CASE("sort_by_key: sorts in place and carries the payload") {
  nx::sort_scratch<Item> scratch;
  nx::vector<Item> v = items({30, 10, 20});
  nx::sort_by_key(v, scratch, item_key);

  REQUIRE(v.size() == 3u);
  CHECK(ordered(v, 3));
  CHECK(v[0].id == 1u);
  CHECK(v[1].id == 2u);
  CHECK(v[2].id == 0u);
}

TEST_CASE("sort_by_key: equal keys keep their order") {
  nx::sort_scratch<Item> scratch;
  nx::vector<Item> v = items({5, 5, 1, 5, 1});
  nx::sort_by_key(v, scratch, item_key);

  REQUIRE(v.size() == 5u);
  CHECK(ordered(v, 5));
  CHECK(v[0].id == 2u);
  CHECK(v[1].id == 4u);
  CHECK(v[2].id == 0u);
  CHECK(v[3].id == 1u);
  CHECK(v[4].id == 3u);
}

TEST_CASE("sort_by_key: ordered input is not touched at all") {
  nx::sort_scratch<Item> scratch;
  nx::vector<Item> v = items({1, 1, 2, 3});
  const Item *const before = v.data();
  nx::sort_by_key(v, scratch, item_key);

  CHECK(v.data() == before);
  for (u32 i = 0; i < 4; ++i)
    CHECK(v[i].id == i);
}

TEST_CASE("sort_by_key: fewer than two elements") {
  nx::sort_scratch<Item> scratch;
  nx::vector<Item> empty;
  nx::sort_by_key(empty, scratch, item_key);
  CHECK(empty.empty());

  nx::vector<Item> one = items({7});
  nx::sort_by_key(one, scratch, item_key);
  REQUIRE(one.size() == 1u);
  CHECK(one[0].id == 0u);
}

TEST_CASE("sort_by_key_into: dst holds the result, ordered or not") {
  nx::sort_scratch<Item> scratch;
  nx::vector<Item> dst;

  const nx::vector<Item> scrambled = items({9, 4, 7});
  nx::sort_by_key_into(std::span<const Item>(scrambled.data(), 3), dst, scratch,
                       item_key);
  REQUIRE(dst.size() >= 3u);
  CHECK(ordered(dst, 3));
  CHECK(dst[0].id == 1u);

  const nx::vector<Item> sorted = items({1, 2, 3});
  nx::sort_by_key_into(std::span<const Item>(sorted.data(), 3), dst, scratch,
                       item_key);
  CHECK(ordered(dst, 3));
  CHECK(dst[0].id == 0u);
  CHECK(dst[2].id == 2u);
}

TEST_CASE("sort_by_key_into: sorts a prefix, leaving the rest of src alone") {
  nx::sort_scratch<Item> scratch;
  const nx::vector<Item> src = items({30, 10, 20, 999, 998});
  nx::vector<Item> dst;

  nx::sort_by_key_into(std::span<const Item>(src.data(), 3), dst, scratch,
                       item_key);
  REQUIRE(dst.size() >= 3u);
  CHECK(dst[0].key == 10u);
  CHECK(dst[1].key == 20u);
  CHECK(dst[2].key == 30u);
}

TEST_CASE("sort_by_key: a scrambled thousand, every element once") {
  nx::sort_scratch<Item> scratch;
  nx::vector<Item> v;
  for (u32 i = 0; i < 1000; ++i)
    v.push_back(Item{nx::hash_u32(i), i});
  nx::sort_by_key(v, scratch, item_key);

  REQUIRE(v.size() == 1000u);
  CHECK(ordered(v, 1000));
  nx::vector<bool> seen(1000, false);
  for (const Item &item : v) {
    REQUIRE(item.id < 1000u);
    CHECK(!seen[item.id]);
    seen[item.id] = true;
  }
}

TEST_CASE("sort_by_key: a pool gives the same answer as no pool") {
  nx::thread_pool pool;

  nx::vector<Item> a, b;
  for (u32 i = 0; i < 5000; ++i) {
    a.push_back(Item{nx::hash_u32(i), i});
    b.push_back(Item{nx::hash_u32(i), i});
  }

  nx::sort_scratch<Item> serial;
  nx::sort_by_key(a, serial, item_key);

  nx::sort_scratch<Item> parallel;
  parallel.pool = &pool;
  parallel.context.set_crossover(1);
  nx::sort_by_key(b, parallel, item_key);

  REQUIRE(a.size() == b.size());
  bool same = true;
  for (usize i = 0; i < a.size(); ++i)
    if (a[i].key != b[i].key || a[i].id != b[i].id)
      same = false;
  CHECK(same);
}

TEST_CASE("sort_by_key: large inputs take the parallel passes and agree") {
  constexpr u32 COUNT = 50000;
  nx::thread_pool pool;
  const auto check = [&](const nx::vector<Item> &input) {
    nx::vector<Item> a = input, b = input;
    nx::sort_scratch<Item> serial;
    nx::sort_by_key(a, serial, item_key);
    nx::sort_scratch<Item> parallel;
    parallel.pool = &pool;
    nx::sort_by_key(b, parallel, item_key);

    nx::vector<Item> into;
    nx::sort_scratch<Item> parallel_into;
    parallel_into.pool = &pool;
    nx::sort_by_key_into(std::span<const Item>(input.data(), input.size()),
                         into, parallel_into, item_key);

    usize mismatched = 0;
    for (usize i = 0; i < a.size(); ++i)
      if (a[i].key != b[i].key || a[i].id != b[i].id ||
          a[i].key != into[i].key || a[i].id != into[i].id)
        ++mismatched;
    return mismatched;
  };

  nx::vector<Item> scrambled, ordered, last_out_of_place;
  for (u32 i = 0; i < COUNT; ++i) {
    scrambled.push_back(Item{nx::hash_u32(i), i});
    ordered.push_back(Item{i, i});
    last_out_of_place.push_back(Item{i + 1, i});
  }
  last_out_of_place.back().key = 0;

  CHECK(check(scrambled) == 0u);
  CHECK(check(ordered) == 0u);
  CHECK(check(last_out_of_place) == 0u);
}

namespace {

// A frame's worth of keys with plenty of ties, so tie order is exercised.
[[nodiscard]] nx::vector<Item> frame_items(const u32 count, const u32 seed) {
  nx::vector<Item> out;
  for (u32 i = 0; i < count; ++i)
    out.push_back(Item{nx::hash_u32(i * 31u + seed) % 700u, i});
  return out;
}

// Whether @p got matches what a fresh stable radix sort of @p input gives.
[[nodiscard]] bool matches_fresh_sort(const nx::vector<Item> &input,
                                      const nx::vector<Item> &got) {
  nx::sort_scratch<Item> fresh;
  nx::vector<Item> expected;
  nx::sort_by_key_into(std::span<const Item>(input.data(), input.size()),
                       expected, fresh, item_key);
  for (usize i = 0; i < input.size(); ++i)
    if (expected[i].key != got[i].key || expected[i].id != got[i].id)
      return false;
  return true;
}

} // namespace

TEST_CASE("coherent sort: a frame that barely moved finishes from the last "
          "order") {
  nx::sort_scratch<Item> scratch;
  nx::vector<Item> frame = frame_items(5000, 1);
  nx::vector<Item> out;
  nx::coherent_sort_by_key_into(std::span<const Item>(frame.data(), 5000), out,
                                scratch, item_key);
  CHECK_FALSE(scratch.reused);
  CHECK(matches_fresh_sort(frame, out));

  // A few elements move a step or two, some onto a neighbour's key.
  for (u32 i = 0; i < 5000; i += 250)
    frame[i].key += 1u + i % 3u;
  nx::coherent_sort_by_key_into(std::span<const Item>(frame.data(), 5000), out,
                                scratch, item_key);
  CHECK(scratch.reused);
  CHECK(matches_fresh_sort(frame, out));

  // Unchanged: the last order is already right.
  nx::coherent_sort_by_key_into(std::span<const Item>(frame.data(), 5000), out,
                                scratch, item_key);
  CHECK(scratch.reused);
  CHECK(matches_fresh_sort(frame, out));
}

TEST_CASE("coherent sort: a reshuffled frame falls back and is still exact") {
  nx::sort_scratch<Item> scratch;
  const nx::vector<Item> first = frame_items(4000, 1);
  nx::vector<Item> out;
  nx::coherent_sort_by_key_into(std::span<const Item>(first.data(), 4000), out,
                                scratch, item_key);

  const nx::vector<Item> second = frame_items(4000, 2);
  nx::coherent_sort_by_key_into(std::span<const Item>(second.data(), 4000), out,
                                scratch, item_key);
  CHECK_FALSE(scratch.reused);
  CHECK(matches_fresh_sort(second, out));

  // And the fallback leaves an order the next frame can start from.
  nx::coherent_sort_by_key_into(std::span<const Item>(second.data(), 4000), out,
                                scratch, item_key);
  CHECK(scratch.reused);
  CHECK(matches_fresh_sort(second, out));
}

TEST_CASE("coherent sort: a different count or an in-place sort starts over") {
  nx::sort_scratch<Item> scratch;
  nx::vector<Item> frame = frame_items(3000, 1);
  nx::vector<Item> out;
  nx::coherent_sort_by_key_into(std::span<const Item>(frame.data(), 3000), out,
                                scratch, item_key);
  nx::coherent_sort_by_key_into(std::span<const Item>(frame.data(), 2999), out,
                                scratch, item_key);
  CHECK_FALSE(scratch.reused);
  frame.resize(2999);
  CHECK(matches_fresh_sort(frame, out));

  // Sorting in place leaves no permutation of anything the caller still has.
  nx::vector<Item> other = frame_items(2999, 5);
  nx::sort_by_key(other, scratch, item_key);
  nx::coherent_sort_by_key_into(std::span<const Item>(frame.data(), 2999), out,
                                scratch, item_key);
  CHECK_FALSE(scratch.reused);
  CHECK(matches_fresh_sort(frame, out));

  // One element, and none.
  nx::coherent_sort_by_key_into(std::span<const Item>(frame.data(), 1), out,
                                scratch, item_key);
  CHECK(out[0].id == frame[0].id);
  nx::coherent_sort_by_key_into(std::span<const Item>(), out, scratch,
                                item_key);
  CHECK_FALSE(scratch.reused);
}

TEST_CASE("coherent sort: a pool gives the same order, reused or not") {
  nx::thread_pool pool;
  nx::sort_scratch<Item> scratch;
  scratch.pool = &pool;
  nx::vector<Item> frame = frame_items(20000, 3);
  nx::vector<Item> out;
  nx::coherent_sort_by_key_into(std::span<const Item>(frame.data(), 20000), out,
                                scratch, item_key);
  CHECK(matches_fresh_sort(frame, out));

  for (u32 i = 0; i < 20000; i += 97)
    frame[i].key += 2u;
  nx::coherent_sort_by_key_into(std::span<const Item>(frame.data(), 20000), out,
                                scratch, item_key);
  CHECK(scratch.reused);
  CHECK(matches_fresh_sort(frame, out));
}

TEST_CASE("coherent sort: far jumps both ways are set aside and merged back") {
  nx::sort_scratch<Item> scratch;
  nx::vector<Item> frame = frame_items(8000, 4);
  nx::vector<Item> out;
  nx::coherent_sort_by_key_into(std::span<const Item>(frame.data(), 8000), out,
                                scratch, item_key);

  // One in sixteen lands anywhere at all, and a run of them all on one key so
  // the set-aside ties have to come out in index order.
  for (u32 i = 0; i < 8000; i += 16)
    frame[i].key = nx::hash_u32(i + 99u) % 900u;
  for (u32 i = 8; i < 8000; i += 64)
    frame[i].key = 350u;
  nx::coherent_sort_by_key_into(std::span<const Item>(frame.data(), 8000), out,
                                scratch, item_key);
  CHECK(scratch.reused);
  CHECK(matches_fresh_sort(frame, out));
}
