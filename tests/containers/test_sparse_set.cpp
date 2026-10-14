#include "framework/nxtest.h"

#include "core/foundation/containers/sparse_set.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

int g_live = 0;

struct Counted {
  int value;
  explicit Counted(int v = 0) : value(v) { ++g_live; }
  Counted(const Counted &o) : value(o.value) { ++g_live; }
  Counted(Counted &&o) noexcept : value(o.value) {
    o.value = -1;
    ++g_live;
  }
  Counted &operator=(const Counted &) = default;
  Counted &operator=(Counted &&) = default;
  ~Counted() { --g_live; }
};

using H = nx::handle32;
using set_t = nx::sparse_set<int>;
using bare_t = nx::sparse_set<void>;

H h(u32 index, u32 version = 0) { return H::from_parts(index, version); }

std::vector<int> sorted_values(const set_t &s) {
  std::vector<int> out(s.begin(), s.end());
  std::sort(out.begin(), out.end());
  return out;
}

}

TEST_CASE("sparse_set: a fresh set is empty") {
  const set_t s;
  CHECK(s.empty());
  CHECK(s.size() == 0);
  CHECK(s.begin() == s.end());
  CHECK(s.entities().empty());
  CHECK(s.values().empty());
  CHECK(set_t::max_size() == H::max_index);
  CHECK(set_t::page_size == 4096);
}

TEST_CASE("sparse_set: emplace stores a value under a handle") {
  set_t s;

  int &a = s.emplace(h(1), 10);
  CHECK(a == 10);
  CHECK(s.size() == 1);
  CHECK(s.contains(h(1)));
  CHECK(s[h(1)] == 10);
  CHECK(s.at(h(1)) == 10);
  REQUIRE(s.try_get(h(1)) != nullptr);
  CHECK(*s.try_get(h(1)) == 10);

  a = 11;
  CHECK(s[h(1)] == 11);

  s.emplace(h(2), 20);
  s.emplace(h(500), 30);
  CHECK(s.size() == 3);
  CHECK(sorted_values(s) == std::vector<int>{11, 20, 30});
}

TEST_CASE("sparse_set: handles far apart do not allocate a flat array") {
  set_t s;
  s.emplace(h(0), 1);
  s.emplace(h(100000), 2);
  s.emplace(h(1000000), 3);

  CHECK(s.size() == 3);
  CHECK(s[h(0)] == 1);
  CHECK(s[h(100000)] == 2);
  CHECK(s[h(1000000)] == 3);

  CHECK(s.entities().size() == 3);
  CHECK(s.values().size() == 3);
}

TEST_CASE("sparse_set: contains rejects absent, stale and null handles") {
  set_t s;
  s.emplace(h(5, 0), 1);

  CHECK(s.contains(h(5, 0)));
  CHECK_FALSE(s.contains(h(5, 1)));
  CHECK_FALSE(s.contains(h(6, 0)));
  CHECK_FALSE(s.contains(h(999999, 0)));
  CHECK_FALSE(s.contains(H::null()));

  CHECK(s.try_get(h(5, 1)) == nullptr);
  CHECK(s.try_get(h(6, 0)) == nullptr);
  CHECK(s.try_get(H::null()) == nullptr);
}

TEST_CASE("sparse_set: insert is idempotent and reports whether it inserted") {
  set_t s;

  auto [ref1, inserted1] = s.insert(h(1), 10);
  CHECK(inserted1);
  CHECK(ref1 == 10);
  CHECK(s.size() == 1);

  auto [ref2, inserted2] = s.insert(h(1), 99);
  CHECK_FALSE(inserted2);
  CHECK(ref2 == 10);
  CHECK(s[h(1)] == 10);
  CHECK(s.size() == 1);

  auto [ref3, inserted3] = s.insert(h(1, 1), 77);
  CHECK(inserted3);
  CHECK(ref3 == 77);
}

TEST_CASE("sparse_set: insert_or_assign overwrites an existing value") {
  set_t s;

  int &a = s.insert_or_assign(h(1), 10);
  CHECK(a == 10);
  CHECK(s.size() == 1);

  int &b = s.insert_or_assign(h(1), 20);
  CHECK(b == 20);
  CHECK(s[h(1)] == 20);
  CHECK(s.size() == 1);
  CHECK(&a == &b);

  s.insert_or_assign(h(2), 30);
  CHECK(s.size() == 2);
  CHECK(s[h(2)] == 30);
}

TEST_CASE("sparse_set: get_or_emplace only constructs when absent") {
  nx::sparse_set<Counted> s;
  g_live = 0;

  Counted &a = s.get_or_emplace(h(1), 10);
  CHECK(a.value == 10);
  CHECK(g_live == 1);

  Counted &b = s.get_or_emplace(h(1), 99);
  CHECK(b.value == 10);
  CHECK(&a == &b);
  CHECK(g_live == 1);

  s.get_or_emplace(h(2), 20);
  CHECK(g_live == 2);
  CHECK(s.size() == 2);

  s.clear();
  CHECK(g_live == 0);
}

TEST_CASE("sparse_set: erase removes the entry and reports success") {
  set_t s;
  s.emplace(h(1), 10);
  s.emplace(h(2), 20);
  s.emplace(h(3), 30);

  CHECK(s.erase(h(2)));
  CHECK(s.size() == 2);
  CHECK_FALSE(s.contains(h(2)));
  CHECK(s.contains(h(1)));
  CHECK(s.contains(h(3)));
  CHECK(s[h(1)] == 10);
  CHECK(s[h(3)] == 30);

  CHECK_FALSE(s.erase(h(2)));
  CHECK_FALSE(s.erase(h(99)));
  CHECK_FALSE(s.erase(h(999999)));
  CHECK_FALSE(s.erase(H::null()));
  CHECK(s.size() == 2);

  CHECK_FALSE(s.erase(h(1, 1)));
  CHECK(s.contains(h(1)));
}

TEST_CASE(
    "sparse_set: erase swap-and-pop keeps every remaining mapping right") {
  set_t s;
  for (u32 i = 0; i < 10; ++i)
    s.emplace(h(i), static_cast<int>(i * 10));

  REQUIRE(s.erase(h(3)));
  for (u32 i = 0; i < 10; ++i) {
    if (i == 3) {
      CHECK_FALSE(s.contains(h(i)));
      continue;
    }
    REQUIRE(s.contains(h(i)));
    CHECK(s[h(i)] == static_cast<int>(i * 10));
    CHECK(s.values()[s.index_of(h(i))] == static_cast<int>(i * 10));
    CHECK(s.handle_at(s.index_of(h(i))) == h(i));
  }

  const H last = s.handle_at(s.size() - 1);
  REQUIRE(s.erase(last));
  CHECK(s.size() == 8);
  for (usize i = 0; i < s.size(); ++i)
    CHECK(s.handle_at(i) == s.handle_at(s.index_of(s.handle_at(i))));

  REQUIRE(s.erase(s.handle_at(0)));
  CHECK(s.size() == 7);
  for (usize i = 0; i < s.size(); ++i)
    CHECK(s.index_of(s.handle_at(i)) == i);
}

TEST_CASE("sparse_set: at() throws and operator[] gives mutable access") {
  set_t s;
  s.emplace(h(1), 10);

  CHECK(s.at(h(1)) == 10);
  s.at(h(1)) = 11;
  CHECK(s[h(1)] == 11);
  s[h(1)] = 12;
  CHECK(s[h(1)] == 12);

  const set_t &cs = s;
  CHECK(cs.at(h(1)) == 12);
  CHECK(cs[h(1)] == 12);

  CHECK_THROWS_AS(s.at(h(2)), std::out_of_range);
  CHECK_THROWS_AS(cs.at(h(2)), std::out_of_range);
  CHECK_THROWS_AS(s.at(h(1, 1)), std::out_of_range);
  CHECK_THROWS_AS(s.at(h(999999)), std::out_of_range);
  CHECK_THROWS_AS(s.at(H::null()), std::out_of_range);
}

TEST_CASE("sparse_set: find returns an iterator into the dense values") {
  set_t s;
  s.emplace(h(1), 10);
  s.emplace(h(2), 20);

  auto it = s.find(h(2));
  REQUIRE(it != s.end());
  CHECK(*it == 20);
  *it = 21;
  CHECK(s[h(2)] == 21);

  CHECK(s.find(h(3)) == s.end());
  CHECK(s.find(h(1, 1)) == s.end());
  CHECK(s.find(H::null()) == s.end());

  const set_t &cs = s;
  auto cit = cs.find(h(1));
  REQUIRE(cit != cs.end());
  CHECK(*cit == 10);
  CHECK(cs.find(h(3)) == cs.end());
}

TEST_CASE("sparse_set: iteration walks the dense values") {
  set_t s;
  for (u32 i = 0; i < 5; ++i)
    s.emplace(h(i), static_cast<int>(i));

  CHECK(std::vector<int>(s.begin(), s.end()) ==
        std::vector<int>{0, 1, 2, 3, 4});
  CHECK(std::distance(s.begin(), s.end()) == 5);

  const set_t &cs = s;
  CHECK(std::vector<int>(cs.begin(), cs.end()) ==
        std::vector<int>{0, 1, 2, 3, 4});
  CHECK(std::vector<int>(s.cbegin(), s.cend()) ==
        std::vector<int>{0, 1, 2, 3, 4});
  CHECK(std::vector<int>(s.rbegin(), s.rend()) ==
        std::vector<int>{4, 3, 2, 1, 0});
  CHECK(std::vector<int>(cs.rbegin(), cs.rend()) ==
        std::vector<int>{4, 3, 2, 1, 0});
  CHECK(std::vector<int>(s.crbegin(), s.crend()) ==
        std::vector<int>{4, 3, 2, 1, 0});

  for (int &v : s)
    v *= 10;
  CHECK(s[h(3)] == 30);
}

TEST_CASE("sparse_set: entities, values and data expose the dense arrays") {
  set_t s;
  s.emplace(h(7), 70);
  s.emplace(h(8), 80);

  const auto ents = s.entities();
  REQUIRE(ents.size() == 2);
  CHECK(ents[0] == h(7));
  CHECK(ents[1] == h(8));

  REQUIRE(s.values().size() == 2);
  CHECK(s.values()[0] == 70);
  CHECK(s.data() == s.values().data());

  const set_t &cs = s;
  CHECK(cs.values().size() == 2);
  CHECK(cs.data() == s.data());

  s.values()[0] = 71;
  CHECK(s[h(7)] == 71);
}

TEST_CASE("sparse_set: each() yields (handle, value) pairs in dense order") {
  set_t s;
  s.emplace(h(1), 10);
  s.emplace(h(5), 50);
  s.emplace(h(9), 90);

  std::vector<std::pair<u32, int>> seen;
  for (auto [handle, value] : s.each())
    seen.emplace_back(handle.index(), value);

  REQUIRE(seen.size() == 3);
  CHECK(seen[0].first == 1u);
  CHECK(seen[0].second == 10);
  CHECK(seen[2].first == 9u);
  CHECK(seen[2].second == 90);

  for (auto [handle, value] : s.each())
    value += 1;
  CHECK(s[h(1)] == 11);
  CHECK(s[h(9)] == 91);

  const set_t &cs = s;
  int sum = 0;
  for (auto [handle, value] : cs.each()) {
    (void)handle;
    sum += value;
  }
  CHECK(sum == 11 + 51 + 91);

  auto it = s.each().begin();
  CHECK(it->first == h(1));
  CHECK(it->second == 11);

  auto post = it++;
  CHECK(post->first == h(1));
  CHECK(it->first == h(5));
  CHECK(it != s.each().begin());

  const set_t e;
  int count = 0;
  for (auto pair : e.each()) {
    (void)pair;
    ++count;
  }
  CHECK(count == 0);
}

TEST_CASE("sparse_set: a mutable each_iterator converts to a const one") {
  set_t s;
  s.emplace(h(1), 10);

  const auto mutable_it = s.each().begin();
  const set_t::each_iterator<true> const_it = mutable_it;
  CHECK(const_it->first == h(1));
  CHECK(const_it->second == 10);
  CHECK((*const_it).second == 10);
}

TEST_CASE("sparse_set: clear empties both sides") {
  set_t s;
  for (u32 i = 0; i < 20; ++i)
    s.emplace(h(i), static_cast<int>(i));

  s.clear();
  CHECK(s.empty());
  CHECK(s.size() == 0);
  CHECK(s.begin() == s.end());
  for (u32 i = 0; i < 20; ++i)
    CHECK_FALSE(s.contains(h(i)));

  s.emplace(h(3), 300);
  CHECK(s.size() == 1);
  CHECK(s[h(3)] == 300);
  CHECK_FALSE(s.contains(h(4)));

  s.clear();
  s.clear();
  CHECK(s.empty());
}

TEST_CASE("sparse_set: clear destroys every value exactly once") {
  g_live = 0;
  {
    nx::sparse_set<Counted> s;
    for (u32 i = 0; i < 50; ++i)
      s.emplace(h(i), static_cast<int>(i));
    CHECK(g_live == 50);
    s.clear();
    CHECK(g_live == 0);
  }
  CHECK(g_live == 0);
}

TEST_CASE("sparse_set: erase destroys exactly the removed value") {
  g_live = 0;
  {
    nx::sparse_set<Counted> s;
    for (u32 i = 0; i < 10; ++i)
      s.emplace(h(i), static_cast<int>(i));
    CHECK(g_live == 10);

    REQUIRE(s.erase(h(4)));
    CHECK(g_live == 9);
    REQUIRE(s.erase(h(9)));
    CHECK(g_live == 8);
    CHECK_FALSE(s.erase(h(4)));
    CHECK(g_live == 8);
  }
  CHECK(g_live == 0);
}

TEST_CASE("sparse_set: reserve and shrink_to_fit keep the contents") {
  set_t s;
  s.emplace(h(1), 10);
  s.reserve(1000);
  CHECK(s.capacity() >= 1000);
  CHECK(s.size() == 1);
  CHECK(s[h(1)] == 10);

  for (u32 i = 2; i < 100; ++i)
    s.emplace(h(i), static_cast<int>(i));
  CHECK(s.size() == 99);

  s.shrink_to_fit();
  CHECK(s.size() == 99);
  for (u32 i = 2; i < 100; ++i)
    CHECK(s[h(i)] == static_cast<int>(i));

  set_t e;
  e.shrink_to_fit();
  CHECK(e.empty());
}

TEST_CASE("sparse_set: sort reorders values and keeps handles consistent") {
  set_t s;
  s.emplace(h(1), 30);
  s.emplace(h(2), 10);
  s.emplace(h(3), 20);

  s.sort([](int a, int b) { return a < b; });

  CHECK(std::vector<int>(s.begin(), s.end()) == std::vector<int>{10, 20, 30});
  CHECK(s[h(1)] == 30);
  CHECK(s[h(2)] == 10);
  CHECK(s[h(3)] == 20);
  CHECK(s.index_of(h(2)) == 0);
  CHECK(s.index_of(h(3)) == 1);
  CHECK(s.index_of(h(1)) == 2);
  CHECK(s.handle_at(0) == h(2));
  CHECK(s.handle_at(2) == h(1));

  s.sort([](int a, int b) { return a > b; });
  CHECK(std::vector<int>(s.begin(), s.end()) == std::vector<int>{30, 20, 10});
  CHECK(s[h(2)] == 10);
  CHECK(s.handle_at(0) == h(1));

  set_t e;
  e.sort([](int a, int b) { return a < b; });
  CHECK(e.empty());
  e.emplace(h(1), 1);
  e.sort([](int a, int b) { return a < b; });
  CHECK(e.size() == 1);
}

TEST_CASE("sparse_set: sort_handles reorders by key") {
  set_t s;
  s.emplace(h(30), 1);
  s.emplace(h(10), 2);
  s.emplace(h(20), 3);

  s.sort_handles([](H a, H b) { return a.index() < b.index(); });

  CHECK(s.handle_at(0) == h(10));
  CHECK(s.handle_at(1) == h(20));
  CHECK(s.handle_at(2) == h(30));
  CHECK(std::vector<int>(s.begin(), s.end()) == std::vector<int>{2, 3, 1});
  CHECK(s[h(10)] == 2);
  CHECK(s[h(30)] == 1);
  for (usize i = 0; i < s.size(); ++i)
    CHECK(s.index_of(s.handle_at(i)) == i);
}

TEST_CASE("sparse_set: sort is stable and survives a later erase") {
  set_t s;
  for (u32 i = 0; i < 20; ++i)
    s.emplace(h(i), static_cast<int>(i % 5));

  s.sort([](int a, int b) { return a < b; });
  for (usize i = 1; i < s.size(); ++i)
    CHECK(s.values()[i - 1] <= s.values()[i]);

  REQUIRE(s.erase(s.handle_at(3)));
  for (usize i = 0; i < s.size(); ++i)
    CHECK(s.index_of(s.handle_at(i)) == i);
}

TEST_CASE("sparse_set: swap exchanges two sets") {
  set_t a, b;
  a.emplace(h(1), 10);
  a.emplace(h(2), 20);
  b.emplace(h(3), 30);

  a.swap(b);
  CHECK(a.size() == 1);
  CHECK(a.contains(h(3)));
  CHECK_FALSE(a.contains(h(1)));
  CHECK(b.size() == 2);
  CHECK(b.contains(h(1)));

  swap(a, b);
  CHECK(a.size() == 2);
  CHECK(a[h(1)] == 10);

  set_t e;
  a.swap(e);
  CHECK(a.empty());
  CHECK(e.size() == 2);
}

TEST_CASE("sparse_set<void>: acts as a plain set of handles") {
  bare_t s;
  CHECK(s.empty());

  CHECK(s.insert(h(1)));
  CHECK(s.insert(h(5)));
  CHECK(s.size() == 2);
  CHECK(s.contains(h(1)));
  CHECK(s.contains(h(5)));
  CHECK_FALSE(s.contains(h(2)));
  CHECK_FALSE(s.contains(h(1, 1)));

  CHECK_FALSE(s.insert(h(1)));
  CHECK(s.size() == 2);

  const std::vector<H> seen(s.begin(), s.end());
  REQUIRE(seen.size() == 2);
  CHECK(seen[0] == h(1));
  CHECK(seen[1] == h(5));
  CHECK(s.entities().size() == 2);

  CHECK(s.erase(h(1)));
  CHECK(s.size() == 1);
  CHECK_FALSE(s.contains(h(1)));
  CHECK_FALSE(s.erase(h(1)));
  CHECK(s.contains(h(5)));

  CHECK(s.find(h(5)) != s.end());
  CHECK(s.find(h(1)) == s.end());
  CHECK(s.handle_at(0) == h(5));
  CHECK(s.index_of(h(5)) == 0);

  s.clear();
  CHECK(s.empty());

  CHECK(sizeof(bare_t) < sizeof(set_t));
}

TEST_CASE("sparse_set<void>: sort_handles reorders the dense handles") {
  bare_t s;
  CHECK(s.insert(h(30)));
  CHECK(s.insert(h(10)));
  CHECK(s.insert(h(20)));

  s.sort_handles([](H a, H b) { return a.index() < b.index(); });
  CHECK(s.handle_at(0) == h(10));
  CHECK(s.handle_at(2) == h(30));
  for (usize i = 0; i < s.size(); ++i)
    CHECK(s.index_of(s.handle_at(i)) == i);
}

TEST_CASE("sparse_set: a small page size still maps handles correctly") {
  nx::sparse_set<int, H, 8> s;
  for (u32 i = 0; i < 100; i += 3)
    s.emplace(h(i), static_cast<int>(i));

  for (u32 i = 0; i < 100; ++i) {
    if (i % 3 == 0) {
      REQUIRE(s.contains(h(i)));
      CHECK(s[h(i)] == static_cast<int>(i));
    } else {
      CHECK_FALSE(s.contains(h(i)));
    }
  }

  CHECK_FALSE(s.contains(h(1000000)));
  s.emplace(h(1000000), 7);
  CHECK(s[h(1000000)] == 7);

  for (u32 i = 0; i < 100; i += 3)
    CHECK(s.erase(h(i)));
  CHECK(s.size() == 1);
  CHECK(s[h(1000000)] == 7);
}

TEST_CASE("sparse_set: works with a 64-bit handle type") {
  nx::sparse_set<int, nx::handle64> s;
  const auto k = nx::handle64::from_parts(123456789, 3);
  s.emplace(k, 42);
  CHECK(s.contains(k));
  CHECK(s[k] == 42);
  CHECK_FALSE(s.contains(nx::handle64::from_parts(123456789, 4)));
  CHECK(s.erase(k));
  CHECK(s.empty());
}

TEST_CASE("sparse_set: holds move-only value types") {
  nx::sparse_set<std::unique_ptr<int>> s;
  s.emplace(h(1), std::make_unique<int>(5));
  s.emplace(h(2), std::make_unique<int>(6));
  REQUIRE(s.contains(h(1)));
  CHECK(*s[h(1)] == 5);

  CHECK(s.erase(h(1)));
  CHECK(*s[h(2)] == 6);
  CHECK(s.size() == 1);
}

TEST_CASE("sparse_set: get_allocator is callable") {
  const set_t s;
  CHECK(s.get_allocator() == s.get_allocator());
}

TEST_CASE("sparse_set: mirrors a reference map under a long mixed workload") {
  nx::sparse_set<int, H, 64> s;
  std::unordered_map<u32, int> model;

  u64 state = 0x0123456789ABCDEFull;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  for (int step = 0; step < 20000; ++step) {
    const u64 r = next();
    const u32 index = static_cast<u32>(r % 2000);
    const int value = static_cast<int>(r >> 32);

    switch (r % 4) {
    case 0:
    case 1: {
      const auto [ref, inserted] = s.insert(h(index), value);
      const bool expected = model.find(index) == model.end();
      CHECK(inserted == expected);
      if (expected)
        model[index] = value;
      CHECK(ref == model[index]);
      break;
    }
    case 2:
      s.insert_or_assign(h(index), value);
      model[index] = value;
      break;
    default: {
      const bool expected = model.erase(index) != 0;
      CHECK(s.erase(h(index)) == expected);
      break;
    }
    }

    REQUIRE(s.size() == model.size());
  }

  for (const auto &[index, value] : model) {
    REQUIRE(s.contains(h(index)));
    CHECK(s[h(index)] == value);
    CHECK(s.values()[s.index_of(h(index))] == value);
  }
  for (usize i = 0; i < s.size(); ++i) {
    const H handle = s.handle_at(i);
    const auto it = model.find(handle.index());
    REQUIRE(it != model.end());
    CHECK(s.values()[i] == it->second);
  }
}
