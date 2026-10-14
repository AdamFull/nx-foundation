#include "framework/nxtest.h"

#include "core/foundation/containers/hash_set.h"
#include "core/foundation/containers/string_set.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
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

struct CountedHash {
  usize operator()(const Counted &c) const noexcept {
    return std::hash<int>{}(c.value);
  }
};

struct AwfulHash {
  usize operator()(int) const noexcept { return 0; }
};

struct PointeeHash {
  usize operator()(const std::unique_ptr<int> &p) const noexcept {
    return std::hash<int>{}(*p);
  }
};
struct PointeeEq {
  bool operator()(const std::unique_ptr<int> &a,
                  const std::unique_ptr<int> &b) const noexcept {
    return *a == *b;
  }
};

using set_t = nx::hash_set<int>;

std::vector<int> elements(const set_t &s) {
  std::vector<int> out(s.begin(), s.end());
  std::sort(out.begin(), out.end());
  return out;
}

} // namespace

static_assert(std::is_same_v<set_t::reference, const int &>);
static_assert(
    std::is_same_v<decltype(*std::declval<set_t::iterator>()), const int &>);
static_assert(std::is_same_v<decltype(*std::declval<set_t::const_iterator>()),
                             const int &>);

TEST_CASE(
    "hash_set: a default-constructed set is empty and allocates nothing") {
  const set_t s;
  CHECK(s.empty());
  CHECK(s.size() == 0);
  CHECK(s.capacity() == 0);
  CHECK(s.begin() == s.end());
  CHECK_FALSE(s.contains(1));
  CHECK(s.find(1) == s.end());
  CHECK(s.count(1) == 0);
}

TEST_CASE("hash_set: insert adds a new key and reports duplicates") {
  set_t s;

  auto [it1, ok1] = s.insert(7);
  CHECK(ok1);
  CHECK(*it1 == 7);

  auto [it2, ok2] = s.insert(7);
  CHECK_FALSE(ok2);
  CHECK(*it2 == 7);
  CHECK(s.size() == 1);

  const int lvalue = 11;
  CHECK(s.insert(lvalue).second);
  CHECK(s.emplace(13).second);
  CHECK_FALSE(s.emplace(13).second);
  CHECK(elements(s) == std::vector<int>{7, 11, 13});
}

TEST_CASE("hash_set: range and initializer-list construction drop duplicates") {
  const std::vector<int> src{4, 9, 4, 1, 9};
  const set_t from_range(src.begin(), src.end());
  CHECK(elements(from_range) == std::vector<int>{1, 4, 9});

  const set_t from_list{5, 3, 5, 8};
  CHECK(elements(from_list) == std::vector<int>{3, 5, 8});

  set_t s;
  s.insert({2, 2, 6});
  CHECK(elements(s) == std::vector<int>{2, 6});
}

TEST_CASE("hash_set: erase by key, by iterator and by const_iterator") {
  set_t s{1, 2, 3, 4};

  CHECK(s.erase(2) == 1);
  CHECK(s.erase(2) == 0);
  CHECK_FALSE(s.contains(2));

  s.erase(s.find(3));
  CHECK_FALSE(s.contains(3));

  const set_t::const_iterator cit = s.find(4);
  s.erase(cit);
  CHECK(elements(s) == std::vector<int>{1});
}

TEST_CASE("hash_set: erasing every element through iterators leaves it empty") {
  set_t s;
  for (int i = 0; i < 200; ++i)
    s.insert(i);
  for (auto it = s.begin(); it != s.end();)
    it = s.erase(it);
  CHECK(s.empty());
  CHECK(s.begin() == s.end());
  for (int i = 0; i < 200; ++i)
    REQUIRE_FALSE(s.contains(i));
}

TEST_CASE("hash_set: iteration visits every element exactly once") {
  set_t s;
  for (int i = 0; i < 1000; i += 3)
    s.insert(i);
  std::vector<int> seen(s.begin(), s.end());
  std::sort(seen.begin(), seen.end());
  CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
  CHECK(seen.size() == s.size());
  CHECK(seen.front() == 0);
  CHECK(seen.back() == 999);
}

TEST_CASE("hash_set: growth and rehash keep every element reachable") {
  set_t s;
  for (int i = 0; i < 5000; ++i)
    s.insert(i * 31);
  CHECK(s.size() == 5000);
  const usize requested = s.capacity() * 4;
  s.rehash(requested);
  CHECK(s.capacity() >= requested);
  for (int i = 0; i < 5000; ++i)
    REQUIRE(s.contains(i * 31));
  CHECK_FALSE(s.contains(1));
  CHECK(s.load_factor() <= set_t::max_load_factor());
}

TEST_CASE("hash_set: pathological collisions still behave correctly") {
  nx::hash_set<int, AwfulHash> s;
  for (int i = 0; i < 500; ++i)
    s.insert(i);
  for (int i = 0; i < 500; i += 2)
    REQUIRE(s.erase(i) == 1);
  CHECK(s.size() == 250);
  for (int i = 0; i < 500; ++i)
    REQUIRE(s.contains(i) == (i % 2 == 1));
  for (int i = 0; i < 500; i += 2)
    s.insert(i);
  CHECK(s.size() == 500);
}

TEST_CASE("hash_set: clear, erase and destruction destroy keys exactly once") {
  g_live = 0;
  {
    nx::hash_set<Counted, CountedHash> s;
    for (int i = 0; i < 100; ++i)
      s.emplace(i);
    CHECK(g_live == 100);
    CHECK_FALSE(s.emplace(5).second);
    CHECK(g_live == 100);

    CHECK(s.erase(Counted{3}) == 1);
    CHECK(g_live == 99);

    s.clear();
    CHECK(g_live == 0);
    CHECK(s.empty());

    for (int i = 0; i < 40; ++i)
      s.emplace(i);
    CHECK(g_live == 40);
  }
  CHECK(g_live == 0);
}

TEST_CASE("hash_set: copies are deep and moves steal the storage") {
  g_live = 0;
  {
    nx::hash_set<Counted, CountedHash> a;
    for (int i = 0; i < 50; ++i)
      a.emplace(i);

    const nx::hash_set<Counted, CountedHash> b = a;
    CHECK(b.size() == 50);
    CHECK(g_live == 100);
    CHECK(b.contains(Counted{49}));

    nx::hash_set<Counted, CountedHash> c;
    c.emplace(999);
    c = a;
    CHECK(c.size() == 50);
    CHECK_FALSE(c.contains(Counted{999}));

    nx::hash_set<Counted, CountedHash> d = std::move(a);
    CHECK(d.size() == 50);
    CHECK(a.empty());
    CHECK(a.capacity() == 0);
    CHECK(g_live == 150);

    c = std::move(d);
    CHECK(c.size() == 50);
    CHECK(d.empty());
    CHECK(g_live == 100);
  }
  CHECK(g_live == 0);
}

TEST_CASE("hash_set: swap exchanges contents") {
  set_t a{1, 2};
  set_t b{3};
  a.swap(b);
  CHECK(elements(a) == std::vector<int>{3});
  CHECK(elements(b) == std::vector<int>{1, 2});
}

TEST_CASE("hash_set: equality compares membership regardless of order") {
  const set_t a{1, 2, 3};
  const set_t b{3, 1, 2};
  const set_t c{1, 2, 4};
  const set_t shorter{1, 2};
  const set_t e1, e2;

  CHECK(a == b);
  CHECK_FALSE(a != b);
  CHECK(a != c);
  CHECK(a != shorter);
  CHECK(e1 == e2);
  CHECK(a != e1);
}

TEST_CASE("hash_set: holds move-only keys across rehashes") {
  nx::hash_set<std::unique_ptr<int>, PointeeHash, PointeeEq> s;
  for (int i = 0; i < 2000; ++i)
    REQUIRE(s.insert(std::make_unique<int>(i)).second);
  CHECK_FALSE(s.insert(std::make_unique<int>(7)).second);
  CHECK(s.size() == 2000);

  const auto probe = std::make_unique<int>(1234);
  REQUIRE(s.contains(probe));
  CHECK(**s.find(probe) == 1234);

  auto moved = std::move(s);
  CHECK(moved.size() == 2000);
  CHECK(moved.erase(probe) == 1);
  CHECK_FALSE(moved.contains(probe));
}

TEST_CASE("hash_set: string_set looks strings up by string_view") {
  nx::string_set s;
  s.insert(nx::string("alpha"));
  s.emplace("beta");

  const nx::string_view alpha = "alpha";
  CHECK(s.contains(alpha));
  CHECK(s.count(nx::string_view("beta")) == 1);
  REQUIRE(s.find(alpha) != s.end());
  CHECK(*s.find(alpha) == nx::string("alpha"));
  CHECK_FALSE(s.contains(nx::string_view("gamma")));

  CHECK(s.erase(nx::string_view("beta")) == 1);
  CHECK(s.size() == 1);
}

TEST_CASE("hash_set: mirrors std::unordered_set under a long mixed workload") {
  set_t s;
  std::unordered_set<int> model;

  u64 state = 0x9E3779B97F4A7C15ull;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  for (int step = 0; step < 30000; ++step) {
    const u64 r = next();
    const int key = static_cast<int>(r % 3000);

    switch (r % 4) {
    case 0:
    case 1:
      CHECK(s.insert(key).second == model.insert(key).second);
      break;
    case 2:
      CHECK(s.emplace(key).second == model.emplace(key).second);
      break;
    default:
      CHECK(s.erase(key) == model.erase(key));
      break;
    }
    REQUIRE(s.size() == model.size());
  }

  for (const int key : model)
    REQUIRE(s.contains(key));
  usize seen = 0;
  for (const int key : s) {
    REQUIRE(model.count(key) == 1);
    ++seen;
  }
  CHECK(seen == model.size());
}
