#include "framework/nxtest.h"

#include "core/foundation/containers/hash_map.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
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
  bool operator==(const Counted &o) const { return value == o.value; }
};

struct AwfulHash {
  usize operator()(int) const noexcept { return 0; }
};

struct TransparentHash {
  using is_transparent = void;
  usize operator()(std::string_view s) const noexcept {
    return std::hash<std::string_view>{}(s);
  }
  usize operator()(const std::string &s) const noexcept {
    return std::hash<std::string_view>{}(s);
  }
  usize operator()(const char *s) const noexcept {
    return std::hash<std::string_view>{}(s);
  }
};
struct TransparentEq {
  using is_transparent = void;
  template <typename A, typename B>
  bool operator()(const A &a, const B &b) const {
    return std::string_view(a) == std::string_view(b);
  }
};

using map_t = nx::hash_map<int, int>;

std::vector<std::pair<int, int>> entries(const map_t &m) {
  std::vector<std::pair<int, int>> out;
  for (const auto &kv : m)
    out.emplace_back(kv.first, kv.second);
  std::sort(out.begin(), out.end());
  return out;
}

}

TEST_CASE(
    "hash_map: a default-constructed map is empty and allocates nothing") {
  const map_t m;
  CHECK(m.empty());
  CHECK(m.size() == 0);
  CHECK(m.capacity() == 0);
  CHECK(m.bucket_count() == 0);
  CHECK(m.begin() == m.end());
  CHECK(m.load_factor() == 0.0f);
  CHECK(map_t::max_load_factor() > 0.5f);
  CHECK_FALSE(m.contains(1));
  CHECK(m.find(1) == m.end());
  CHECK(m.count(1) == 0);
}

TEST_CASE("hash_map: insert adds a new key and reports duplicates") {
  map_t m;

  auto [it1, ok1] = m.insert({1, 10});
  CHECK(ok1);
  CHECK(it1->first == 1);
  CHECK(it1->second == 10);
  CHECK(m.size() == 1);

  auto [it2, ok2] = m.insert({1, 99});
  CHECK_FALSE(ok2);
  CHECK(it2->second == 10);
  CHECK(m.size() == 1);

  auto [it3, ok3] = m.insert({2, 20});
  CHECK(ok3);
  CHECK(it3->second == 20);
  CHECK(m.size() == 2);

  std::pair<const int, int> tmp{3, 30};
  CHECK(m.insert(std::move(tmp)).second);
  CHECK(entries(m) ==
        std::vector<std::pair<int, int>>{{1, 10}, {2, 20}, {3, 30}});
}

TEST_CASE("hash_map: range and initializer-list insertion") {
  const std::vector<std::pair<const int, int>> src{{1, 10}, {2, 20}, {1, 99}};

  map_t a;
  a.insert(src.begin(), src.end());
  CHECK(a.size() == 2);
  CHECK(a.at(1) == 10);

  map_t b;
  b.insert({{3, 30}, {4, 40}});
  CHECK(b.size() == 2);
  CHECK(b.at(4) == 40);

  const map_t c{{5, 50}, {6, 60}};
  CHECK(c.size() == 2);
  CHECK(c.at(5) == 50);

  const map_t d(src.begin(), src.end());
  CHECK(d.size() == 2);

  const map_t e(100);
  CHECK(e.empty());
  CHECK(e.capacity() >= 100);
  const map_t f(0);
  CHECK(f.capacity() == 0);
}

TEST_CASE("hash_map: emplace constructs the pair in place") {
  nx::hash_map<int, std::string> m;

  auto [it, ok] = m.emplace(1, "one");
  CHECK(ok);
  CHECK(it->second == "one");

  CHECK_FALSE(m.emplace(1, "uno").second);
  CHECK(m.at(1) == "one");

  CHECK(m.emplace(std::pair<const int, std::string>{2, "two"}).second);
  CHECK(m.at(2) == "two");
  CHECK(m.size() == 2);

  const auto hint = m.emplace_hint(m.begin(), 3, "three");
  CHECK(hint->second == "three");
  CHECK(m.size() == 3);
}

TEST_CASE("hash_map: emplace does not leak when the key already exists") {
  nx::hash_map<int, Counted> m;
  g_live = 0;

  CHECK(m.emplace(1, Counted{5}).second);
  CHECK(g_live == 1);

  CHECK_FALSE(m.emplace(1, Counted{9}).second);
  CHECK(g_live == 1);
  CHECK(m.at(1).value == 5);

  m.clear();
  CHECK(g_live == 0);
}

TEST_CASE("hash_map: try_emplace skips construction for an existing key") {
  nx::hash_map<int, Counted> m;
  g_live = 0;

  auto [it, ok] = m.try_emplace(1, 5);
  CHECK(ok);
  CHECK(it->second.value == 5);
  CHECK(g_live == 1);

  auto [it2, ok2] = m.try_emplace(1, 9);
  CHECK_FALSE(ok2);
  CHECK(it2->second.value == 5);
  CHECK(g_live == 1);

  nx::hash_map<std::string, int> s;
  std::string key = "moved";
  CHECK(s.try_emplace(std::move(key), 1).second);
  CHECK(s.at("moved") == 1);
  CHECK_FALSE(s.try_emplace(std::string("moved"), 2).second);

  map_t z;
  CHECK(z.try_emplace(7).second);
  CHECK(z.at(7) == 0);

  m.clear();
  CHECK(g_live == 0);
}

TEST_CASE("hash_map: insert_or_assign overwrites an existing mapped value") {
  map_t m;

  auto [it1, ok1] = m.insert_or_assign(1, 10);
  CHECK(ok1);
  CHECK(it1->second == 10);

  auto [it2, ok2] = m.insert_or_assign(1, 20);
  CHECK_FALSE(ok2);
  CHECK(it2->second == 20);
  CHECK(m.at(1) == 20);
  CHECK(m.size() == 1);

  nx::hash_map<std::string, int> s;
  std::string key = "k";
  CHECK(s.insert_or_assign(std::move(key), 1).second);
  CHECK_FALSE(s.insert_or_assign(std::string("k"), 2).second);
  CHECK(s.at("k") == 2);
}

TEST_CASE("hash_map: operator[] inserts a value-initialized mapped type") {
  map_t m;

  CHECK(m[1] == 0);
  CHECK(m.size() == 1);

  m[1] = 10;
  CHECK(m.at(1) == 10);
  CHECK(m.size() == 1);

  m[2] = 20;
  CHECK(m.size() == 2);

  CHECK(m[3] == 0);
  CHECK(m.size() == 3);
  CHECK(m.contains(3));

  nx::hash_map<std::string, int> s;
  std::string key = "abc";
  s[key] = 1;
  s[std::string("d")] = 2;
  CHECK(s.at("abc") == 1);
  CHECK(s.at("d") == 2);
}

TEST_CASE("hash_map: at() throws for an absent key") {
  map_t m{{1, 10}};

  CHECK(m.at(1) == 10);
  m.at(1) = 11;
  CHECK(m.at(1) == 11);

  const map_t &cm = m;
  CHECK(cm.at(1) == 11);

  CHECK_THROWS_AS(m.at(2), std::out_of_range);
  CHECK_THROWS_AS(cm.at(2), std::out_of_range);

  const map_t e;
  CHECK_THROWS_AS(e.at(0), std::out_of_range);
}

TEST_CASE("hash_map: find, contains and count") {
  map_t m{{1, 10}, {2, 20}};

  auto it = m.find(1);
  REQUIRE(it != m.end());
  CHECK(it->first == 1);
  CHECK(it->second == 10);
  it->second = 11;
  CHECK(m.at(1) == 11);

  CHECK(m.find(3) == m.end());
  CHECK(m.contains(1));
  CHECK_FALSE(m.contains(3));
  CHECK(m.count(1) == 1);
  CHECK(m.count(3) == 0);

  const map_t &cm = m;
  auto cit = cm.find(2);
  REQUIRE(cit != cm.end());
  CHECK(cit->second == 20);
  CHECK(cm.find(3) == cm.end());
  CHECK(cm.contains(2));
  CHECK(cm.count(2) == 1);
}

TEST_CASE("hash_map: erase by key, by iterator and by const_iterator") {
  map_t m;
  for (int i = 0; i < 10; ++i)
    m[i] = i * 10;

  CHECK(m.erase(3) == 1);
  CHECK(m.size() == 9);
  CHECK_FALSE(m.contains(3));

  CHECK(m.erase(3) == 0);
  CHECK(m.erase(100) == 0);
  CHECK(m.size() == 9);

  auto it = m.find(5);
  REQUIRE(it != m.end());
  const auto next = m.erase(it);
  CHECK(m.size() == 8);
  CHECK_FALSE(m.contains(5));
  if (next != m.end())
    CHECK(m.contains(next->first));

  const auto cit = m.find(7);
  REQUIRE(cit != m.end());
  m.erase(map_t::const_iterator(cit));
  CHECK(m.size() == 7);
  CHECK_FALSE(m.contains(7));

  for (int i = 0; i < 10; ++i) {
    if (i == 3 || i == 5 || i == 7)
      continue;
    REQUIRE(m.contains(i));
    CHECK(m.at(i) == i * 10);
  }
}

TEST_CASE("hash_map: erasing every element through iterators leaves it empty") {
  map_t m;
  for (int i = 0; i < 200; ++i)
    m[i] = i;

  auto it = m.begin();
  while (it != m.end())
    it = m.erase(it);

  CHECK(m.empty());
  CHECK(m.size() == 0);
  CHECK(m.begin() == m.end());

  m[1] = 1;
  CHECK(m.size() == 1);
  CHECK(m.at(1) == 1);
}

TEST_CASE("hash_map: erase then reinsert reuses tombstoned slots") {
  map_t m;
  for (int i = 0; i < 100; ++i)
    m[i] = i;
  for (int i = 0; i < 100; ++i)
    CHECK(m.erase(i) == 1);
  CHECK(m.empty());

  const usize cap_after_first = m.capacity();
  for (int round = 0; round < 50; ++round) {
    for (int i = 0; i < 100; ++i)
      m[i] = i;
    for (int i = 0; i < 100; ++i)
      CHECK(m.erase(i) == 1);
  }
  CHECK(m.empty());
  CHECK(m.capacity() <= cap_after_first * 4);

  for (int i = 0; i < 100; ++i)
    m[i] = i * 2;
  CHECK(m.size() == 100);
  for (int i = 0; i < 100; ++i)
    CHECK(m.at(i) == i * 2);
}

TEST_CASE("hash_map: clear empties the map but keeps the storage") {
  map_t m;
  for (int i = 0; i < 100; ++i)
    m[i] = i;
  const usize cap = m.capacity();

  m.clear();
  CHECK(m.empty());
  CHECK(m.size() == 0);
  CHECK(m.begin() == m.end());
  CHECK(m.capacity() == cap);
  for (int i = 0; i < 100; ++i)
    CHECK_FALSE(m.contains(i));

  m[1] = 1;
  CHECK(m.size() == 1);

  m.clear();
  m.clear();
  CHECK(m.empty());

  map_t e;
  e.clear();
  CHECK(e.empty());
}

TEST_CASE("hash_map: clear and erase destroy values exactly once") {
  g_live = 0;
  {
    nx::hash_map<int, Counted> m;
    for (int i = 0; i < 100; ++i)
      m.try_emplace(i, i);
    CHECK(g_live == 100);

    CHECK(m.erase(50) == 1);
    CHECK(g_live == 99);
    CHECK(m.erase(50) == 0);
    CHECK(g_live == 99);

    m.clear();
    CHECK(g_live == 0);

    for (int i = 0; i < 10; ++i)
      m.try_emplace(i, i);
    CHECK(g_live == 10);
  }
  CHECK(g_live == 0);
}

TEST_CASE("hash_map: the destructor releases every element") {
  g_live = 0;
  {
    nx::hash_map<int, Counted> m;
    for (int i = 0; i < 5000; ++i)
      m.try_emplace(i, i);
    CHECK(g_live == 5000);
  }
  CHECK(g_live == 0);
}

TEST_CASE("hash_map: iteration visits every element exactly once") {
  map_t m;
  for (int i = 0; i < 500; ++i)
    m[i] = i * 3;

  std::vector<int> seen;
  for (const auto &kv : m) {
    CHECK(kv.second == kv.first * 3);
    seen.push_back(kv.first);
  }
  REQUIRE(seen.size() == 500);
  std::sort(seen.begin(), seen.end());
  for (int i = 0; i < 500; ++i)
    REQUIRE(seen[static_cast<usize>(i)] == i);

  CHECK(std::distance(m.begin(), m.end()) == 500);

  for (auto &kv : m)
    kv.second = 0;
  for (const auto &kv : m)
    CHECK(kv.second == 0);

  const map_t &cm = m;
  CHECK(std::distance(cm.begin(), cm.end()) == 500);
  CHECK(std::distance(m.cbegin(), m.cend()) == 500);
}

TEST_CASE("hash_map: iteration over a sparse table skips holes") {
  map_t m;
  for (int i = 0; i < 1000; ++i)
    m[i] = i;
  for (int i = 0; i < 1000; ++i)
    if (i % 137 != 0)
      m.erase(i);

  std::vector<int> seen;
  for (const auto &kv : m)
    seen.push_back(kv.first);
  std::sort(seen.begin(), seen.end());

  std::vector<int> expected;
  for (int i = 0; i < 1000; i += 137)
    expected.push_back(i);
  CHECK(seen == expected);
  CHECK(m.size() == expected.size());
}

TEST_CASE("hash_map: a mutable iterator converts to a const one") {
  map_t m{{1, 10}};
  const map_t::iterator it = m.begin();
  const map_t::const_iterator cit = it;
  CHECK(cit->first == 1);
  CHECK(cit->second == 10);
  CHECK(cit == it);

  map_t::iterator copy = it;
  const auto post = copy++;
  CHECK(post == it);
  CHECK(copy == m.end());
}

TEST_CASE("hash_map: reserve pre-allocates without rehashing later") {
  map_t m;
  m.reserve(1000);
  const usize cap = m.capacity();
  CHECK(cap >= 1000);
  CHECK(m.empty());

  for (int i = 0; i < 1000; ++i)
    m[i] = i;
  CHECK(m.size() == 1000);
  CHECK(m.capacity() == cap);

  for (int i = 0; i < 1000; ++i)
    CHECK(m.at(i) == i);

  m.reserve(0);
  CHECK(m.capacity() == cap);
  m.reserve(10);
  CHECK(m.capacity() == cap);
}

TEST_CASE("hash_map: rehash keeps every element reachable") {
  map_t m;
  for (int i = 0; i < 300; ++i)
    m[i] = i * 2;

  m.rehash(2000);
  CHECK(m.capacity() >= 2000);
  CHECK(m.size() == 300);
  for (int i = 0; i < 300; ++i)
    CHECK(m.at(i) == i * 2);

  m.rehash(0);
  CHECK(m.size() == 300);
  for (int i = 0; i < 300; ++i)
    CHECK(m.at(i) == i * 2);

  map_t e;
  e.rehash(0);
  CHECK(e.empty());
  e.rehash(64);
  CHECK(e.empty());
  e[1] = 1;
  CHECK(e.at(1) == 1);
}

TEST_CASE("hash_map: growth preserves every element across many rehashes") {
  map_t m;
  for (int i = 0; i < 20000; ++i) {
    m[i] = i;
    REQUIRE(m.at(i / 2) == i / 2);
  }
  CHECK(m.size() == 20000);
  for (int i = 0; i < 20000; ++i)
    REQUIRE(m.at(i) == i);

  CHECK(m.load_factor() > 0.0f);
  CHECK(m.load_factor() <= map_t::max_load_factor());
}

TEST_CASE("hash_map: pathological collisions still behave correctly") {
  nx::hash_map<int, int, AwfulHash> m;
  for (int i = 0; i < 500; ++i)
    m[i] = i * 7;

  CHECK(m.size() == 500);
  for (int i = 0; i < 500; ++i)
    REQUIRE(m.at(i) == i * 7);
  CHECK_FALSE(m.contains(500));

  for (int i = 0; i < 500; i += 2)
    REQUIRE(m.erase(i) == 1);
  CHECK(m.size() == 250);
  for (int i = 0; i < 500; ++i) {
    if (i % 2 == 0)
      CHECK_FALSE(m.contains(i));
    else
      CHECK(m.at(i) == i * 7);
  }

  for (int i = 0; i < 500; i += 2)
    m[i] = i;
  CHECK(m.size() == 500);
  for (int i = 0; i < 500; ++i)
    REQUIRE(m.contains(i));
}

TEST_CASE("hash_map: copy construction and assignment are deep") {
  g_live = 0;
  {
    nx::hash_map<int, Counted> a;
    for (int i = 0; i < 50; ++i)
      a.try_emplace(i, i);
    CHECK(g_live == 50);

    const nx::hash_map<int, Counted> b = a;
    CHECK(b.size() == 50);
    CHECK(g_live == 100);
    for (int i = 0; i < 50; ++i) {
      REQUIRE(b.contains(i));
      CHECK(b.at(i).value == i);
    }

    nx::hash_map<int, Counted> c;
    c.try_emplace(999, 999);
    c = a;
    CHECK(c.size() == 50);
    CHECK_FALSE(c.contains(999));
    CHECK(c.at(0).value == 0);

    c.at(0).value = 42;
    CHECK(a.at(0).value == 0);
  }
  CHECK(g_live == 0);

  const map_t e;
  const map_t ec = e;
  CHECK(ec.empty());
  map_t ea{{1, 1}};
  ea = e;
  CHECK(ea.empty());
}

TEST_CASE("hash_map: move construction and assignment steal the storage") {
  g_live = 0;
  {
    nx::hash_map<int, Counted> a;
    for (int i = 0; i < 50; ++i)
      a.try_emplace(i, i);

    nx::hash_map<int, Counted> b = std::move(a);
    CHECK(b.size() == 50);
    CHECK(a.empty());
    CHECK(a.capacity() == 0);
    CHECK(a.begin() == a.end());
    CHECK(g_live == 50);
    CHECK(b.at(10).value == 10);

    nx::hash_map<int, Counted> c;
    c.try_emplace(999, 999);
    CHECK(g_live == 51);
    c = std::move(b);
    CHECK(c.size() == 50);
    CHECK(b.empty());
    CHECK(g_live == 50);
    CHECK_FALSE(c.contains(999));

    b[1] = Counted{1};
    CHECK(b.size() == 1);
  }
  CHECK(g_live == 0);
}

TEST_CASE("hash_map: self-assignment leaves the map intact") {
  map_t m;
  for (int i = 0; i < 20; ++i)
    m[i] = i;

  const map_t *alias = &m;
  m = *alias;
  CHECK(m.size() == 20);
  CHECK(m.at(5) == 5);

  map_t *malias = &m;
  m = std::move(*malias);
  CHECK(m.size() == 20);
  CHECK(m.at(5) == 5);
}

TEST_CASE("hash_map: swap exchanges contents") {
  map_t a{{1, 10}, {2, 20}};
  map_t b{{3, 30}};

  a.swap(b);
  CHECK(a.size() == 1);
  CHECK(a.at(3) == 30);
  CHECK(b.size() == 2);
  CHECK(b.at(1) == 10);

  map_t e;
  a.swap(e);
  CHECK(a.empty());
  CHECK(e.size() == 1);
  CHECK(e.at(3) == 30);

  map_t x, y;
  x.swap(y);
  CHECK(x.empty());
  CHECK(y.empty());
}

TEST_CASE("hash_map: equality compares keys and mapped values") {
  const map_t a{{1, 10}, {2, 20}};
  const map_t b{{2, 20}, {1, 10}};
  const map_t c{{1, 10}, {2, 21}};
  const map_t d{{1, 10}, {3, 20}};
  const map_t shorter{{1, 10}};
  const map_t e1, e2;

  CHECK(a == b);
  CHECK_FALSE(a != b);
  CHECK(a != c);
  CHECK(a != d);
  CHECK(a != shorter);
  CHECK(e1 == e2);
  CHECK(a != e1);
}

TEST_CASE("hash_map: heterogeneous lookup avoids building a temporary key") {
  nx::hash_map<std::string, int, TransparentHash, TransparentEq> m;
  m["alpha"] = 1;
  m["beta"] = 2;

  const std::string_view sv = "alpha";
  auto it = m.find(sv);
  REQUIRE(it != m.end());
  CHECK(it->second == 1);
  CHECK(m.contains(sv));
  CHECK(m.count(sv) == 1);
  CHECK(m.find(std::string_view("missing")) == m.end());
  CHECK(m.count(std::string_view("missing")) == 0);

  const nx::hash_map<std::string, int, TransparentHash, TransparentEq> &cm = m;
  auto cit = cm.find(sv);
  REQUIRE(cit != cm.end());
  CHECK(cit->second == 1);
  CHECK(cm.contains(std::string_view("beta")));

  CHECK(m.erase(std::string_view("beta")) == 1);
  CHECK(m.size() == 1);
  CHECK(m.erase(std::string_view("beta")) == 0);
}

TEST_CASE("hash_map: string keys work end to end") {
  nx::hash_map<std::string, std::string> m;
  m["one"] = "1";
  m["two"] = "2";
  m.try_emplace("three", "3");
  m.insert_or_assign("one", "uno");

  CHECK(m.size() == 3);
  CHECK(m.at("one") == "uno");
  CHECK(m.at("three") == "3");
  CHECK(m.erase("two") == 1);
  CHECK(m.size() == 2);
  CHECK_FALSE(m.contains("two"));

  for (int i = 0; i < 500; ++i)
    m[std::string("key_prefix_") + std::to_string(i)] = std::to_string(i);
  CHECK(m.size() == 502);
  for (int i = 0; i < 500; ++i)
    REQUIRE(m.at(std::string("key_prefix_") + std::to_string(i)) ==
            std::to_string(i));
}

TEST_CASE("hash_map: holds move-only mapped types") {
  nx::hash_map<int, std::unique_ptr<int>> m;
  m.try_emplace(1, std::make_unique<int>(10));
  m.emplace(2, std::make_unique<int>(20));

  REQUIRE(m.contains(1));
  CHECK(*m.at(1) == 10);
  CHECK(*m.at(2) == 20);

  nx::hash_map<int, std::unique_ptr<int>> moved = std::move(m);
  CHECK(moved.size() == 2);
  CHECK(*moved.at(1) == 10);

  CHECK(moved.erase(1) == 1);
  CHECK(moved.size() == 1);

  for (int i = 100; i < 2000; ++i)
    moved.try_emplace(i, std::make_unique<int>(i));
  CHECK(moved.size() == 1901);
  for (int i = 100; i < 2000; ++i)
    REQUIRE(*moved.at(i) == i);
}

TEST_CASE("hash_map: hash_function and key_eq are retrievable") {
  const map_t m;
  const auto h = m.hash_function();
  const auto eq = m.key_eq();
  CHECK(h(5) == std::hash<int>{}(5));
  CHECK(eq(5, 5));
  CHECK_FALSE(eq(5, 6));
  CHECK(m.get_allocator() == m.get_allocator());
}

TEST_CASE("hash_map: mirrors std::unordered_map under a long mixed workload") {
  map_t m;
  std::unordered_map<int, int> model;

  u64 state = 0xB5AD4ECEDA1CE2A9ull;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  for (int step = 0; step < 30000; ++step) {
    const u64 r = next();
    const int key = static_cast<int>(r % 3000);
    const int value = static_cast<int>(r >> 32);

    switch (r % 6) {
    case 0:
    case 1: {
      const bool expected = model.find(key) == model.end();
      CHECK(m.insert({key, value}).second == expected);
      if (expected)
        model[key] = value;
      break;
    }
    case 2:
      m.insert_or_assign(key, value);
      model[key] = value;
      break;
    case 3:
      m[key] = value;
      model[key] = value;
      break;
    case 4: {
      const bool expected = model.find(key) == model.end();
      CHECK(m.try_emplace(key, value).second == expected);
      if (expected)
        model[key] = value;
      break;
    }
    default: {
      const usize expected = model.erase(key);
      CHECK(m.erase(key) == expected);
      break;
    }
    }

    REQUIRE(m.size() == model.size());
  }

  for (const auto &[key, value] : model) {
    REQUIRE(m.contains(key));
    CHECK(m.at(key) == value);
  }
  usize seen = 0;
  for (const auto &kv : m) {
    const auto it = model.find(kv.first);
    REQUIRE(it != model.end());
    CHECK(kv.second == it->second);
    ++seen;
  }
  CHECK(seen == model.size());
}
