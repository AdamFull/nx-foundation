#include "framework/nxtest.h"

#include "core/foundation/containers/flat_map.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// A key made explicitly from a view, counting how often that happens.
struct Name {
  static inline int made = 0;
  std::string text;
  explicit Name(const std::string_view s) : text(s) { ++made; }
  operator std::string_view() const noexcept { return text; }
  bool operator==(const Name &) const = default;
};

struct NameLess {
  using is_transparent = void;
  bool operator()(const std::string_view a,
                  const std::string_view b) const noexcept {
    return a < b;
  }
};

using map_t = nx::flat_map<int, int>;

std::vector<std::pair<int, int>> entries(const map_t &m) {
  std::vector<std::pair<int, int>> out;
  for (auto it = m.begin(); it != m.end(); ++it)
    out.emplace_back(it->first, it->second);
  return out;
}

std::vector<int> keys_of(const map_t &m) {
  std::vector<int> out;
  for (auto it = m.begin(); it != m.end(); ++it)
    out.push_back(it->first);
  return out;
}

struct TransparentLess {
  using is_transparent = void;
  template <typename A, typename B>
  bool operator()(const A &a, const B &b) const {
    return std::string_view(a) < std::string_view(b);
  }
};

}

TEST_CASE("flat_map: a default-constructed map is empty") {
  const map_t m;
  CHECK(m.empty());
  CHECK(m.size() == 0);
  CHECK(m.begin() == m.end());
  CHECK(m.keys().empty());
  CHECK(m.values().empty());
  CHECK(m.max_size() > 0);
  CHECK_FALSE(m.contains(1));
  CHECK(m.find(1) == m.end());
  CHECK(m.count(1) == 0);
}

TEST_CASE("flat_map: keys are stored in sorted order") {
  map_t m;
  for (const int k : {5, 1, 9, 3, 7})
    m[k] = k * 10;

  CHECK(keys_of(m) == std::vector<int>{1, 3, 5, 7, 9});
  CHECK(entries(m) == std::vector<std::pair<int, int>>{
                          {1, 10}, {3, 30}, {5, 50}, {7, 70}, {9, 90}});

  CHECK(std::vector<int>(m.keys().begin(), m.keys().end()) ==
        std::vector<int>{1, 3, 5, 7, 9});
  CHECK(std::vector<int>(m.values().begin(), m.values().end()) ==
        std::vector<int>{10, 30, 50, 70, 90});
}

TEST_CASE("flat_map: initializer-list and range construction sort and dedupe") {
  const map_t a{{3, 30}, {1, 10}, {2, 20}, {1, 99}};
  CHECK(a.size() == 3);
  CHECK(keys_of(a) == std::vector<int>{1, 2, 3});
  CHECK(a.at(1) == 10);

  const std::vector<std::pair<int, int>> src{{5, 50}, {4, 40}, {5, 55}};
  const map_t b(src.begin(), src.end());
  CHECK(b.size() == 2);
  CHECK(b.at(5) == 50);
  CHECK(keys_of(b) == std::vector<int>{4, 5});

  const map_t e{};
  CHECK(e.empty());
  const map_t e2(src.begin(), src.begin());
  CHECK(e2.empty());
}

TEST_CASE("flat_map: container construction sorts and dedupes in parallel") {
  nx::vector<int> keys{3, 1, 2, 1};
  nx::vector<int> values{30, 10, 20, 99};
  const map_t m(std::move(keys), std::move(values));

  CHECK(m.size() == 3);
  CHECK(keys_of(m) == std::vector<int>{1, 2, 3});
  CHECK(m.at(1) == 10);
  CHECK(m.at(2) == 20);
  CHECK(m.at(3) == 30);
}

TEST_CASE("flat_map: sorted_unique construction skips the sort") {
  nx::vector<int> keys{1, 2, 3};
  nx::vector<int> values{10, 20, 30};
  const map_t m(nx::sorted_unique, std::move(keys), std::move(values));
  CHECK(m.size() == 3);
  CHECK(m.at(2) == 20);
  CHECK(keys_of(m) == std::vector<int>{1, 2, 3});

  const map_t il(nx::sorted_unique, {{1, 10}, {2, 20}});
  CHECK(il.size() == 2);
  CHECK(il.at(1) == 10);

  const std::vector<std::pair<int, int>> src{{4, 40}, {5, 50}};
  const map_t it(nx::sorted_unique, src.begin(), src.end());
  CHECK(it.size() == 2);
  CHECK(it.at(5) == 50);
}

TEST_CASE("flat_map: a custom comparator changes the ordering") {
  nx::flat_map<int, int, std::greater<int>> m;
  for (const int k : {1, 5, 3})
    m[k] = k;

  std::vector<int> keys;
  for (auto it = m.begin(); it != m.end(); ++it)
    keys.push_back(it->first);
  CHECK(keys == std::vector<int>{5, 3, 1});
  CHECK(m.at(3) == 3);
  CHECK(m.contains(5));
  CHECK_FALSE(m.contains(2));

  CHECK(m.key_comp()(5, 3));
  CHECK_FALSE(m.key_comp()(3, 5));
  const auto vc = m.value_comp();
  CHECK(vc(std::pair<int, int>{5, 0}, std::pair<int, int>{3, 0}));
}

TEST_CASE("flat_map: try_emplace inserts only when the key is absent") {
  map_t m;

  auto [it1, ok1] = m.try_emplace(1, 10);
  CHECK(ok1);
  CHECK(it1->first == 1);
  CHECK(it1->second == 10);

  auto [it2, ok2] = m.try_emplace(1, 99);
  CHECK_FALSE(ok2);
  CHECK(it2->second == 10);
  CHECK(m.size() == 1);

  CHECK(m.try_emplace(0, 0).second);
  CHECK(m.try_emplace(5, 50).second);
  CHECK(m.try_emplace(3, 30).second);
  CHECK(keys_of(m) == std::vector<int>{0, 1, 3, 5});

  nx::flat_map<std::string, int> s;
  std::string key = "k";
  CHECK(s.try_emplace(std::move(key), 1).second);
  CHECK(s.at("k") == 1);
  CHECK(s.try_emplace(s.begin(), "a", 2)->second == 2);
  CHECK(s.try_emplace(s.begin(), std::string("z"), 3)->second == 3);
  CHECK(s.size() == 3);

  map_t z;
  CHECK(z.try_emplace(7).second);
  CHECK(z.at(7) == 0);
}

TEST_CASE("flat_map: insert_or_assign overwrites an existing value") {
  map_t m;

  auto [it1, ok1] = m.insert_or_assign(1, 10);
  CHECK(ok1);
  CHECK(it1->second == 10);

  auto [it2, ok2] = m.insert_or_assign(1, 20);
  CHECK_FALSE(ok2);
  CHECK(it2->second == 20);
  CHECK(m.size() == 1);
  CHECK(m.at(1) == 20);

  nx::flat_map<std::string, int> s;
  std::string key = "k";
  CHECK(s.insert_or_assign(std::move(key), 1).second);
  CHECK_FALSE(s.insert_or_assign(std::string("k"), 2).second);
  CHECK(s.at("k") == 2);
  CHECK(s.insert_or_assign(s.begin(), "a", 3)->second == 3);
  CHECK(s.insert_or_assign(s.begin(), std::string("z"), 4)->second == 4);
}

TEST_CASE("flat_map: insert and emplace") {
  map_t m;

  auto [it1, ok1] = m.insert({2, 20});
  CHECK(ok1);
  CHECK(it1->second == 20);
  CHECK_FALSE(m.insert({2, 99}).second);
  CHECK(m.at(2) == 20);

  std::pair<int, int> tmp{1, 10};
  CHECK(m.insert(std::move(tmp)).second);
  CHECK(keys_of(m) == std::vector<int>{1, 2});

  CHECK(m.emplace(3, 30).second);
  CHECK_FALSE(m.emplace(3, 99).second);
  CHECK(m.at(3) == 30);

  CHECK(m.emplace_hint(m.begin(), 4, 40)->second == 40);
  CHECK(m.insert(m.begin(), std::pair<int, int>{5, 50})->second == 50);
  const std::pair<int, int> lv{6, 60};
  CHECK(m.insert(m.begin(), lv)->second == 60);
  CHECK(keys_of(m) == std::vector<int>{1, 2, 3, 4, 5, 6});
}

TEST_CASE("flat_map: bulk insert merges, sorts and keeps existing values") {
  map_t m{{1, 10}, {3, 30}};

  const std::vector<std::pair<int, int>> src{
      {5, 50}, {2, 20}, {3, 99}, {4, 40}};
  m.insert(src.begin(), src.end());

  CHECK(m.size() == 5);
  CHECK(keys_of(m) == std::vector<int>{1, 2, 3, 4, 5});
  CHECK(m.at(3) == 30);
  CHECK(m.at(2) == 20);
  CHECK(m.at(5) == 50);

  m.insert({{6, 60}, {7, 70}});
  CHECK(m.size() == 7);
  m.insert(nx::sorted_unique, {{8, 80}});
  CHECK(m.at(8) == 80);

  const std::vector<std::pair<int, int>> more{{9, 90}};
  m.insert(nx::sorted_unique, more.begin(), more.end());
  CHECK(keys_of(m) == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9});

  const auto before = entries(m);
  m.insert(src.begin(), src.begin());
  CHECK(entries(m) == before);

  map_t e;
  const std::vector<std::pair<int, int>> dup{{2, 2}, {1, 1}, {2, 22}, {1, 11}};
  e.insert(dup.begin(), dup.end());
  CHECK(e.size() == 2);
  CHECK(e.at(1) == 1);
  CHECK(e.at(2) == 2);
}

TEST_CASE("flat_map: operator[] inserts a value-initialized mapped type") {
  map_t m;
  CHECK(m[1] == 0);
  CHECK(m.size() == 1);
  m[1] = 10;
  CHECK(m.at(1) == 10);
  m[0] = 5;
  CHECK(keys_of(m) == std::vector<int>{0, 1});

  nx::flat_map<std::string, int> s;
  std::string key = "abc";
  s[key] = 1;
  s[std::string("d")] = 2;
  CHECK(s.at("abc") == 1);
  CHECK(s.at("d") == 2);
}

TEST_CASE("flat_map: at() throws for an absent key") {
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

TEST_CASE("flat_map: find, contains and count") {
  map_t m{{1, 10}, {3, 30}, {5, 50}};

  auto it = m.find(3);
  REQUIRE(it != m.end());
  CHECK(it->first == 3);
  CHECK(it->second == 30);
  it->second = 33;
  CHECK(m.at(3) == 33);

  CHECK(m.find(2) == m.end());
  CHECK(m.find(0) == m.end());
  CHECK(m.find(6) == m.end());
  CHECK(m.contains(5));
  CHECK_FALSE(m.contains(4));
  CHECK(m.count(1) == 1);
  CHECK(m.count(2) == 0);

  const map_t &cm = m;
  auto cit = cm.find(5);
  REQUIRE(cit != cm.end());
  CHECK(cit->second == 50);
  CHECK(cm.find(2) == cm.end());
  CHECK(cm.contains(1));
  CHECK(cm.count(1) == 1);
}

TEST_CASE("flat_map: lower_bound, upper_bound and equal_range") {
  map_t m{{1, 10}, {3, 30}, {5, 50}};

  CHECK(m.lower_bound(3)->first == 3);
  CHECK(m.lower_bound(2)->first == 3);
  CHECK(m.lower_bound(0)->first == 1);
  CHECK(m.lower_bound(6) == m.end());

  CHECK(m.upper_bound(3)->first == 5);
  CHECK(m.upper_bound(2)->first == 3);
  CHECK(m.upper_bound(0)->first == 1);
  CHECK(m.upper_bound(5) == m.end());

  auto [lo, hi] = m.equal_range(3);
  CHECK(hi - lo == 1);
  CHECK(lo->first == 3);

  auto [lo2, hi2] = m.equal_range(4);
  CHECK(lo2 == hi2);
  CHECK(lo2->first == 5);

  auto [lo3, hi3] = m.equal_range(9);
  CHECK(lo3 == hi3);
  CHECK(lo3 == m.end());

  const map_t &cm = m;
  CHECK(cm.lower_bound(3)->first == 3);
  CHECK(cm.upper_bound(3)->first == 5);
  auto [clo, chi] = cm.equal_range(1);
  CHECK(chi - clo == 1);
  CHECK(clo->second == 10);
}

TEST_CASE("flat_map: erase by key, iterator and range") {
  map_t m;
  for (int i = 0; i < 10; ++i)
    m[i] = i * 10;

  CHECK(m.erase(3) == 1);
  CHECK(m.size() == 9);
  CHECK_FALSE(m.contains(3));
  CHECK(m.erase(3) == 0);
  CHECK(m.erase(100) == 0);

  auto it = m.erase(m.find(5));
  CHECK(it->first == 6);
  CHECK(m.size() == 8);

  it = m.erase(map_t::const_iterator(m.find(7)));
  CHECK(it->first == 8);

  it = m.erase(m.find(0), m.find(4));
  CHECK(it->first == 4);
  CHECK(keys_of(m) == std::vector<int>{4, 6, 8, 9});

  const auto before = keys_of(m);
  it = m.erase(m.begin(), m.begin());
  CHECK(keys_of(m) == before);
  CHECK(it == m.begin());

  it = m.erase(m.end() - 1);
  CHECK(it == m.end());
  CHECK(keys_of(m) == std::vector<int>{4, 6, 8});

  it = m.erase(m.begin(), m.end());
  CHECK(m.empty());
  CHECK(it == m.begin());
  CHECK(it == m.end());
}

TEST_CASE("flat_map: clear, reserve, capacity and shrink_to_fit") {
  map_t m;
  for (int i = 0; i < 100; ++i)
    m[i] = i;

  m.reserve(1000);
  CHECK(m.capacity() >= 1000);
  CHECK(m.size() == 100);
  CHECK(m.at(50) == 50);

  m.clear();
  CHECK(m.empty());
  CHECK(m.size() == 0);
  CHECK(m.begin() == m.end());
  CHECK(m.keys().empty());
  CHECK(m.values().empty());

  m.shrink_to_fit();
  CHECK(m.empty());

  m[1] = 1;
  CHECK(m.size() == 1);
  m.clear();
  m.clear();
  CHECK(m.empty());
}

TEST_CASE("flat_map: forward and reverse iteration are ordered") {
  map_t m;
  for (const int k : {5, 1, 3})
    m[k] = k;

  std::vector<int> fwd;
  for (auto it = m.begin(); it != m.end(); ++it)
    fwd.push_back(it->first);
  CHECK(fwd == std::vector<int>{1, 3, 5});

  std::vector<int> rev;
  for (auto it = m.rbegin(); it != m.rend(); ++it)
    rev.push_back(it->first);
  CHECK(rev == std::vector<int>{5, 3, 1});

  const map_t &cm = m;
  std::vector<int> crev;
  for (auto it = cm.rbegin(); it != cm.rend(); ++it)
    crev.push_back(it->first);
  CHECK(crev == std::vector<int>{5, 3, 1});

  std::vector<int> crev2;
  for (auto it = m.crbegin(); it != m.crend(); ++it)
    crev2.push_back(it->first);
  CHECK(crev2 == std::vector<int>{5, 3, 1});

  CHECK(std::distance(m.begin(), m.end()) == 3);
  CHECK(std::distance(m.cbegin(), m.cend()) == 3);

  for (auto it = m.begin(); it != m.end(); ++it)
    it->second *= 100;
  CHECK(m.at(3) == 300);

  const map_t e;
  CHECK(e.begin() == e.end());
  CHECK(e.rbegin() == e.rend());
}

TEST_CASE("flat_map: the iterator is random access") {
  map_t m;
  for (int i = 0; i < 10; ++i)
    m[i] = i * 10;

  auto it = m.begin();
  CHECK(it->first == 0);

  it += 3;
  CHECK(it->first == 3);
  it -= 2;
  CHECK(it->first == 1);

  CHECK((it + 4)->first == 5);
  CHECK((4 + it)->first == 5);
  CHECK((it - 1)->first == 0);
  CHECK(m.end() - m.begin() == 10);
  CHECK((m.begin() + 5) - m.begin() == 5);

  CHECK(it[0].first == 1);
  CHECK(it[2].second == 30);

  CHECK(m.begin() < m.end());
  CHECK(m.end() > m.begin());
  CHECK(m.begin() <= m.begin());
  CHECK(m.begin() >= m.begin());
  CHECK(m.begin() == m.begin());
  CHECK(m.begin() != m.end());

  const auto post = it++;
  CHECK(post->first == 1);
  CHECK(it->first == 2);
  const auto postdec = it--;
  CHECK(postdec->first == 2);
  CHECK(it->first == 1);
  --it;
  CHECK(it == m.begin());
  ++it;
  CHECK(it->first == 1);
}

TEST_CASE("flat_map: a mutable iterator converts to a const one") {
  map_t m{{1, 10}};
  const map_t::iterator it = m.begin();
  const map_t::const_iterator cit = it;
  CHECK(cit->first == 1);
  CHECK(cit->second == 10);
  CHECK(cit == it);
  CHECK((*cit).second == 10);
}

TEST_CASE("flat_map: copy, move and assignment") {
  map_t a{{1, 10}, {2, 20}};

  const map_t b = a;
  CHECK(b.size() == 2);
  CHECK(b.at(1) == 10);

  map_t c;
  c = a;
  CHECK(c.size() == 2);
  c.at(1) = 99;
  CHECK(a.at(1) == 10);

  map_t d = std::move(c);
  CHECK(d.size() == 2);
  CHECK(d.at(1) == 99);

  map_t e{{9, 9}};
  e = std::move(d);
  CHECK(e.size() == 2);
  CHECK(e.at(2) == 20);
  CHECK_FALSE(e.contains(9));

  e = {{7, 70}, {6, 60}};
  CHECK(e.size() == 2);
  CHECK(keys_of(e) == std::vector<int>{6, 7});

  const map_t *alias = &a;
  a = *alias;
  CHECK(a.size() == 2);
  CHECK(a.at(1) == 10);
}

TEST_CASE("flat_map: swap exchanges contents") {
  map_t a{{1, 10}, {2, 20}};
  map_t b{{3, 30}};

  a.swap(b);
  CHECK(a.size() == 1);
  CHECK(a.at(3) == 30);
  CHECK(b.size() == 2);
  CHECK(b.at(1) == 10);

  swap(a, b);
  CHECK(a.size() == 2);
  CHECK(a.at(2) == 20);

  map_t e;
  a.swap(e);
  CHECK(a.empty());
  CHECK(e.size() == 2);
}

TEST_CASE("flat_map: extract and replace move the backing containers") {
  map_t m{{1, 10}, {2, 20}, {3, 30}};

  auto c = std::move(m).extract();
  CHECK(c.keys.size() == 3);
  CHECK(c.values.size() == 3);
  CHECK(c.keys[0] == 1);
  CHECK(c.values[2] == 30);
  CHECK(m.empty());

  m.replace(std::move(c.keys), std::move(c.values));
  CHECK(m.size() == 3);
  CHECK(m.at(2) == 20);
  CHECK(keys_of(m) == std::vector<int>{1, 2, 3});

  m.replace(nx::vector<int>{}, nx::vector<int>{});
  CHECK(m.empty());
}

TEST_CASE("flat_map: equality and ordering compare keys then values") {
  const map_t a{{1, 10}, {2, 20}};
  const map_t b{{2, 20}, {1, 10}};
  const map_t c{{1, 10}, {2, 21}};
  const map_t d{{1, 10}, {3, 20}};
  const map_t shorter{{1, 10}};
  const map_t e1, e2;

  CHECK(a == b);
  CHECK(a != c);
  CHECK(a != d);
  CHECK(a != shorter);
  CHECK(e1 == e2);

  CHECK(a < c);
  CHECK(a < d);
  CHECK(shorter < a);
  CHECK(e1 < shorter);
  CHECK(c > a);
  CHECK(a <= b);
  CHECK(a >= b);
  CHECK((a <=> b) == std::strong_ordering::equal);
}

TEST_CASE("flat_map: heterogeneous lookup with a transparent comparator") {
  nx::flat_map<std::string, int, TransparentLess> m;
  m["alpha"] = 1;
  m["beta"] = 2;
  m["gamma"] = 3;

  const std::string_view sv = "beta";
  auto it = m.find(sv);
  REQUIRE(it != m.end());
  CHECK(it->second == 2);
  CHECK(m.contains(sv));
  CHECK(m.count(sv) == 1);
  CHECK(m.find(std::string_view("missing")) == m.end());
  CHECK(m.count(std::string_view("missing")) == 0);

  CHECK(m.lower_bound(std::string_view("b"))->first == "beta");
  CHECK(m.upper_bound(std::string_view("beta"))->first == "gamma");
  auto [lo, hi] = m.equal_range(std::string_view("beta"));
  CHECK(hi - lo == 1);

  const nx::flat_map<std::string, int, TransparentLess> &cm = m;
  CHECK(cm.find(sv) != cm.end());
  CHECK(cm.contains(sv));
  CHECK(cm.count(sv) == 1);
  CHECK(cm.lower_bound(std::string_view("b"))->first == "beta");
  CHECK(cm.upper_bound(std::string_view("beta"))->first == "gamma");
  auto [clo, chi] = cm.equal_range(std::string_view("alpha"));
  CHECK(chi - clo == 1);

  CHECK(m.erase(std::string_view("beta")) == 1);
  CHECK(m.size() == 2);
  CHECK(m.erase(std::string_view("beta")) == 0);
}

TEST_CASE("flat_map: erase_if removes exactly the matching entries") {
  map_t m;
  for (int i = 0; i < 10; ++i)
    m[i] = i * 10;

  const usize removed =
      nx::erase_if(m, [](const auto &kv) { return kv.first % 2 == 0; });
  CHECK(removed == 5);
  CHECK(keys_of(m) == std::vector<int>{1, 3, 5, 7, 9});
  CHECK(m.at(9) == 90);

  CHECK(nx::erase_if(m, [](const auto &kv) { return kv.second > 50; }) == 2);
  CHECK(keys_of(m) == std::vector<int>{1, 3, 5});

  CHECK(nx::erase_if(m, [](const auto &) { return false; }) == 0);
  CHECK(m.size() == 3);
  CHECK(nx::erase_if(m, [](const auto &) { return true; }) == 3);
  CHECK(m.empty());

  CHECK(nx::erase_if(m, [](const auto &) { return true; }) == 0);
  CHECK(m.empty());

  for (int i = 0; i < 100; ++i)
    m[i] = i;
  CHECK(nx::erase_if(m, [](const auto &kv) { return kv.first % 3 != 0; }) ==
        66);
  CHECK(m.size() == 34);
  for (int i = 0; i < 100; i += 3) {
    REQUIRE(m.contains(i));
    CHECK(m.at(i) == i);
  }
  auto prev = m.begin();
  for (auto it = m.begin() + 1; it != m.end(); ++it, ++prev)
    CHECK(prev->first < it->first);
}

TEST_CASE("flat_map: holds move-only mapped types") {
  nx::flat_map<int, std::unique_ptr<int>> m;
  m.try_emplace(2, std::make_unique<int>(20));
  m.try_emplace(1, std::make_unique<int>(10));
  m.emplace(3, std::make_unique<int>(30));

  REQUIRE(m.size() == 3);
  CHECK(*m.at(1) == 10);
  CHECK(*m.at(3) == 30);
  CHECK(m.begin()->first == 1);

  CHECK(m.erase(2) == 1);
  CHECK(m.size() == 2);
  CHECK(*m.at(3) == 30);

  nx::flat_map<int, std::unique_ptr<int>> moved = std::move(m);
  CHECK(moved.size() == 2);
  CHECK(*moved.at(1) == 10);
}

TEST_CASE("flat_map: string keys work end to end") {
  nx::flat_map<std::string, std::string> m;
  m["banana"] = "yellow";
  m["apple"] = "red";
  m["cherry"] = "dark";

  std::vector<std::string> keys;
  for (auto it = m.begin(); it != m.end(); ++it)
    keys.push_back(it->first);
  CHECK(keys == std::vector<std::string>{"apple", "banana", "cherry"});
  CHECK(m.at("banana") == "yellow");
  CHECK(m.erase("banana") == 1);
  CHECK(m.size() == 2);
  CHECK_FALSE(m.contains("banana"));
}

TEST_CASE("flat_map: mirrors std::map under a long mixed workload") {
  map_t m;
  std::map<int, int> model;

  u64 state = 0x1F2E3D4C5B6A7988ull;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  for (int step = 0; step < 15000; ++step) {
    const u64 r = next();
    const int key = static_cast<int>(r % 500);
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

  std::vector<std::pair<int, int>> expected(model.begin(), model.end());
  CHECK(entries(m) == expected);

  for (int key = 0; key < 500; ++key) {
    const auto it = model.find(key);
    if (it == model.end()) {
      CHECK_FALSE(m.contains(key));
      CHECK(m.find(key) == m.end());
    } else {
      REQUIRE(m.contains(key));
      CHECK(m.at(key) == it->second);
    }
    CHECK((m.lower_bound(key) - m.begin()) ==
          std::distance(model.begin(), model.lower_bound(key)));
    CHECK((m.upper_bound(key) - m.begin()) ==
          std::distance(model.begin(), model.upper_bound(key)));
  }
}

TEST_CASE("flat_map: a transparent key inserts without making a Key it finds") {
  using namespace std::string_view_literals;
  nx::flat_map<Name, int, NameLess> m;
  Name::made = 0;

  CHECK(m.try_emplace("a"sv, 1).second);
  CHECK(Name::made == 1);
  CHECK_FALSE(m.try_emplace("a"sv, 2).second);
  CHECK(Name::made == 1);
  CHECK(m.at("a"sv) == 1);

  m["b"sv] = 5;
  CHECK(Name::made == 2);
  m["b"sv] += 1;
  CHECK(Name::made == 2);
  CHECK(m.at("b"sv) == 6);

  CHECK_FALSE(m.insert_or_assign("a"sv, 9).second);
  CHECK(Name::made == 2);
  CHECK(m.at("a"sv) == 9);
  CHECK(m.insert_or_assign("d"sv, 7).second);
  CHECK(Name::made == 3);

  CHECK(m.try_emplace(m.cbegin(), "c"sv, 3)->second == 3);
  CHECK(Name::made == 4);
  CHECK(m.insert_or_assign(m.cend(), "c"sv, 4)->second == 4);
  CHECK(m.try_emplace(m.cend(), "c"sv, 8)->second == 4);
  CHECK(Name::made == 4);

  const auto &cm = m;
  CHECK(cm.at("c"sv) == 4);
  CHECK_THROWS_AS(m.at("zz"sv), std::out_of_range);
  CHECK_THROWS_AS(cm.at("zz"sv), std::out_of_range);
  CHECK(m.size() == usize{4});
}
