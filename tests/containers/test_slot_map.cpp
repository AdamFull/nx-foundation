#include "framework/nxtest.h"

#include "core/foundation/containers/slot_map.h"

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

using map_t = nx::slot_map<int>;
using handle_t = map_t::handle_type;

std::vector<int> values(const map_t &m) {
  std::vector<int> out(m.begin(), m.end());
  std::sort(out.begin(), out.end());
  return out;
}

}

TEST_CASE("slot_map: a fresh map is empty") {
  const map_t m;
  CHECK(m.empty());
  CHECK(m.size() == 0);
  CHECK(m.begin() == m.end());
  CHECK(m.as_span().empty());
  CHECK(m.max_size() == handle_t::max_index);
}

TEST_CASE("slot_map: insert returns a handle that resolves to the value") {
  map_t m;

  const handle_t a = m.insert(10);
  CHECK(m.size() == 1);
  CHECK_FALSE(m.empty());
  CHECK(a.valid());
  CHECK(m.contains(a));
  CHECK(m[a] == 10);
  CHECK(m.at(a) == 10);
  REQUIRE(m.try_get(a) != nullptr);
  CHECK(*m.try_get(a) == 10);

  const handle_t b = m.insert(20);
  CHECK(m.size() == 2);
  CHECK(a != b);
  CHECK(m[a] == 10);
  CHECK(m[b] == 20);

  int tmp = 30;
  const handle_t c = m.insert(std::move(tmp));
  CHECK(m[c] == 30);

  CHECK(values(m) == std::vector<int>{10, 20, 30});
}

TEST_CASE("slot_map: emplace constructs in place and returns a handle") {
  nx::slot_map<std::string> m;

  const auto a = m.emplace(3, 'x');
  CHECK(m[a] == "xxx");

  const auto b = m.emplace("hello");
  CHECK(m[b] == "hello");
  CHECK(m.size() == 2);

  nx::slot_map<std::pair<int, int>> p;
  const auto h = p.emplace(1, 2);
  CHECK(p[h].first == 1);
  CHECK(p[h].second == 2);
}

TEST_CASE("slot_map: the first handles use consecutive indices from zero") {
  map_t m;
  const handle_t a = m.insert(1);
  const handle_t b = m.insert(2);
  const handle_t c = m.insert(3);

  CHECK(a.index() == 0);
  CHECK(b.index() == 1);
  CHECK(c.index() == 2);
  CHECK(a.version() == 0);
  CHECK(b.version() == 0);
}

TEST_CASE("slot_map: erase removes the element and invalidates the handle") {
  map_t m;
  const handle_t a = m.insert(1);
  const handle_t b = m.insert(2);

  CHECK(m.erase(a));
  CHECK(m.size() == 1);
  CHECK_FALSE(m.contains(a));
  CHECK(m.try_get(a) == nullptr);
  CHECK(m.contains(b));
  CHECK(m[b] == 2);

  CHECK_FALSE(m.erase(a));
  CHECK(m.size() == 1);

  CHECK_FALSE(m.erase(handle_t::from_parts(999, 0)));
  CHECK_FALSE(m.erase(handle_t::null()));
  CHECK(m.size() == 1);

  CHECK(m.erase(b));
  CHECK(m.empty());
}

TEST_CASE("slot_map: erase uses swap-and-pop but keeps every handle correct") {
  map_t m;
  std::vector<handle_t> handles;
  for (int i = 0; i < 10; ++i)
    handles.push_back(m.insert(i));

  REQUIRE(m.erase(handles[3]));
  CHECK(m.size() == 9);
  CHECK_FALSE(m.contains(handles[3]));

  for (int i = 0; i < 10; ++i) {
    if (i == 3)
      continue;
    REQUIRE(m.contains(handles[static_cast<usize>(i)]));
    CHECK(m[handles[static_cast<usize>(i)]] == i);
  }

  REQUIRE(m.erase(handles[9]));
  CHECK(m.size() == 8);
  for (int i = 0; i < 10; ++i) {
    if (i == 3 || i == 9)
      continue;
    CHECK(m[handles[static_cast<usize>(i)]] == i);
  }

  REQUIRE(m.erase(handles[0]));
  CHECK(m.size() == 7);
  CHECK(m[handles[5]] == 5);
}

TEST_CASE("slot_map: a stale handle to a recycled slot reports absent") {
  map_t m;
  const handle_t a = m.insert(1);
  const auto index = a.index();
  const auto version = a.version();

  REQUIRE(m.erase(a));

  const handle_t b = m.insert(2);
  CHECK(b.index() == index);
  CHECK(b.version() == version + 1);

  CHECK_FALSE(m.contains(a));
  CHECK(m.contains(b));
  CHECK(m[b] == 2);
  CHECK(m.try_get(a) == nullptr);
  CHECK_FALSE(m.erase(a));

  CHECK(m.size() == 1);
  CHECK(m[b] == 2);
}

TEST_CASE(
    "slot_map: a handle with the right index but wrong version is absent") {
  map_t m;
  const handle_t a = m.insert(1);

  CHECK_FALSE(m.contains(handle_t::from_parts(a.index(), a.version() + 1)));
  CHECK_FALSE(m.contains(handle_t::from_parts(a.index(), a.version() + 7)));
  CHECK(m.contains(a));

  CHECK_FALSE(m.contains(handle_t::from_parts(10000, 0)));
  CHECK_FALSE(m.contains(handle_t::null()));
  CHECK(m.try_get(handle_t::from_parts(10000, 0)) == nullptr);
}

TEST_CASE("slot_map: freed slots are recycled in LIFO order") {
  map_t m;
  const handle_t a = m.insert(1);
  const handle_t b = m.insert(2);
  const handle_t c = m.insert(3);

  REQUIRE(m.erase(a));
  REQUIRE(m.erase(b));

  const handle_t d = m.insert(4);
  CHECK(d.index() == b.index());
  const handle_t e = m.insert(5);
  CHECK(e.index() == a.index());

  const handle_t f = m.insert(6);
  CHECK(f.index() == 3);

  CHECK(m.size() == 4);
  CHECK(m[c] == 3);
  CHECK(m[d] == 4);
  CHECK(m[e] == 5);
  CHECK(m[f] == 6);
}

TEST_CASE("slot_map: at() throws for an absent handle") {
  map_t m;
  const handle_t a = m.insert(1);
  CHECK(m.at(a) == 1);
  m.at(a) = 9;
  CHECK(m[a] == 9);

  const map_t &cm = m;
  CHECK(cm.at(a) == 9);

  REQUIRE(m.erase(a));
  CHECK_THROWS_AS(m.at(a), std::out_of_range);
  CHECK_THROWS_AS(cm.at(a), std::out_of_range);
  CHECK_THROWS_AS(m.at(handle_t::null()), std::out_of_range);
}

TEST_CASE("slot_map: operator[] and try_get give mutable access") {
  map_t m;
  const handle_t a = m.insert(1);

  m[a] = 42;
  CHECK(m[a] == 42);

  *m.try_get(a) = 43;
  CHECK(m[a] == 43);

  const map_t &cm = m;
  CHECK(cm[a] == 43);
  REQUIRE(cm.try_get(a) != nullptr);
  CHECK(*cm.try_get(a) == 43);
  CHECK(cm.try_get(handle_t::null()) == nullptr);
}

TEST_CASE("slot_map: iteration walks the dense array in insertion order") {
  map_t m;
  for (int i = 0; i < 5; ++i)
    (void)m.insert(i);

  CHECK(std::vector<int>(m.begin(), m.end()) ==
        std::vector<int>{0, 1, 2, 3, 4});
  CHECK(std::distance(m.begin(), m.end()) == 5);

  const map_t &cm = m;
  CHECK(std::vector<int>(cm.begin(), cm.end()) ==
        std::vector<int>{0, 1, 2, 3, 4});
  CHECK(std::vector<int>(m.cbegin(), m.cend()) ==
        std::vector<int>{0, 1, 2, 3, 4});
  CHECK(std::vector<int>(m.rbegin(), m.rend()) ==
        std::vector<int>{4, 3, 2, 1, 0});
  CHECK(std::vector<int>(cm.rbegin(), cm.rend()) ==
        std::vector<int>{4, 3, 2, 1, 0});
  CHECK(std::vector<int>(m.crbegin(), m.crend()) ==
        std::vector<int>{4, 3, 2, 1, 0});

  for (int &v : m)
    v *= 10;
  CHECK(std::vector<int>(m.begin(), m.end()) ==
        std::vector<int>{0, 10, 20, 30, 40});
}

TEST_CASE("slot_map: data and as_span expose the packed elements") {
  map_t m;
  const handle_t a = m.insert(1);
  (void)m.insert(2);

  REQUIRE(m.data() != nullptr);
  CHECK(m.data()[0] == 1);
  CHECK(m.as_span().size() == 2);
  CHECK(m.as_span().data() == m.data());

  const map_t &cm = m;
  CHECK(cm.data() == m.data());
  CHECK(cm.as_span().size() == 2);

  m.as_span()[0] = 99;
  CHECK(m[a] == 99);

  const map_t e;
  CHECK(e.as_span().empty());
}

TEST_CASE("slot_map: handle_at recovers the handle for a dense position") {
  map_t m;
  std::vector<handle_t> handles;
  for (int i = 0; i < 6; ++i)
    handles.push_back(m.insert(i));

  for (usize i = 0; i < m.size(); ++i) {
    const handle_t h = m.handle_at(i);
    CHECK(m.contains(h));
    CHECK(m[h] == m.data()[i]);
    CHECK(h == handles[i]);
  }

  REQUIRE(m.erase(handles[1]));
  for (usize i = 0; i < m.size(); ++i) {
    const handle_t h = m.handle_at(i);
    REQUIRE(m.contains(h));
    CHECK(&m[h] == m.data() + i);
  }
}

TEST_CASE("slot_map: clear removes everything and invalidates every handle") {
  map_t m;
  std::vector<handle_t> handles;
  for (int i = 0; i < 5; ++i)
    handles.push_back(m.insert(i));

  m.clear();
  CHECK(m.empty());
  CHECK(m.size() == 0);
  CHECK(m.begin() == m.end());
  for (const handle_t h : handles) {
    CHECK_FALSE(m.contains(h));
    CHECK(m.try_get(h) == nullptr);
  }

  const handle_t a = m.insert(100);
  CHECK(m.size() == 1);
  CHECK(m[a] == 100);
  CHECK_FALSE(m.contains(handles[0]));

  m.clear();
  m.clear();
  CHECK(m.empty());
}

TEST_CASE("slot_map: clear destroys every element exactly once") {
  g_live = 0;
  {
    nx::slot_map<Counted> m;
    for (int i = 0; i < 20; ++i)
      (void)m.emplace(i);
    CHECK(g_live == 20);

    m.clear();
    CHECK(g_live == 0);

    for (int i = 0; i < 5; ++i)
      (void)m.emplace(i);
    CHECK(g_live == 5);
  }
  CHECK(g_live == 0);
}

TEST_CASE("slot_map: erase destroys exactly the removed element") {
  g_live = 0;
  {
    nx::slot_map<Counted> m;
    std::vector<nx::slot_map<Counted>::handle_type> handles;
    for (int i = 0; i < 10; ++i)
      handles.push_back(m.emplace(i));
    CHECK(g_live == 10);

    REQUIRE(m.erase(handles[4]));
    CHECK(g_live == 9);
    REQUIRE(m.erase(handles[9]));
    CHECK(g_live == 8);
    CHECK_FALSE(m.erase(handles[4]));
    CHECK(g_live == 8);
  }
  CHECK(g_live == 0);
}

TEST_CASE("slot_map: reserve does not disturb the contents") {
  map_t m;
  const handle_t a = m.insert(1);
  const handle_t b = m.insert(2);

  m.reserve(1000);
  CHECK(m.capacity() >= 1000);
  CHECK(m.size() == 2);
  CHECK(m[a] == 1);
  CHECK(m[b] == 2);

  for (int i = 0; i < 500; ++i)
    (void)m.insert(i);
  CHECK(m[a] == 1);
  CHECK(m[b] == 2);
  CHECK(m.size() == 502);
}

TEST_CASE("slot_map: handles survive growth across many reallocations") {
  map_t m;
  std::vector<handle_t> handles;
  for (int i = 0; i < 5000; ++i)
    handles.push_back(m.insert(i));

  CHECK(m.size() == 5000);
  for (int i = 0; i < 5000; ++i) {
    REQUIRE(m.contains(handles[static_cast<usize>(i)]));
    CHECK(m[handles[static_cast<usize>(i)]] == i);
  }
}

TEST_CASE("slot_map: handles are unique across a long insert/erase workload") {
  map_t m;
  std::unordered_map<u32, int> expected;
  std::vector<handle_t> live;
  std::set<u32> ever_issued;

  u64 state = 0xDEADBEEFCAFEBABEull;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  for (int step = 0; step < 12000; ++step) {
    const u64 r = next();
    if (live.empty() || r % 3 != 0) {
      const int value = static_cast<int>(r >> 32);
      const handle_t h = m.insert(value);
      REQUIRE(expected.find(h.raw()) == expected.end());
      expected[h.raw()] = value;
      live.push_back(h);
      ever_issued.insert(h.raw());
    } else {
      const usize pick = static_cast<usize>(r >> 8) % live.size();
      const handle_t h = live[pick];
      REQUIRE(m.erase(h));
      expected.erase(h.raw());
      live[pick] = live.back();
      live.pop_back();
    }

    REQUIRE(m.size() == live.size());
  }

  for (const handle_t h : live) {
    REQUIRE(m.contains(h));
    CHECK(m[h] == expected[h.raw()]);
  }

  std::vector<int> from_iteration(m.begin(), m.end());
  std::vector<int> from_handles;
  for (const handle_t h : live)
    from_handles.push_back(expected[h.raw()]);
  std::sort(from_iteration.begin(), from_iteration.end());
  std::sort(from_handles.begin(), from_handles.end());
  CHECK(from_iteration == from_handles);
}

TEST_CASE("slot_map: dense storage stays compact through churn") {
  nx::slot_map<Counted> m;
  std::vector<nx::slot_map<Counted>::handle_type> live;
  g_live = 0;

  for (int round = 0; round < 50; ++round) {
    for (int i = 0; i < 20; ++i)
      live.push_back(m.emplace(i));
    std::vector<nx::slot_map<Counted>::handle_type> keep;
    for (usize i = 0; i < live.size(); ++i) {
      if (i % 2 == 0)
        REQUIRE(m.erase(live[i]));
      else
        keep.push_back(live[i]);
    }
    live = keep;
    REQUIRE(m.size() == live.size());
    REQUIRE(g_live == static_cast<int>(live.size()));
  }

  for (const auto h : live)
    CHECK(m.contains(h));

  m.clear();
  CHECK(g_live == 0);
}

TEST_CASE("slot_map: holds non-copyable element types") {
  nx::slot_map<std::unique_ptr<int>> m;
  const auto a = m.emplace(std::make_unique<int>(5));
  const auto b = m.emplace(std::make_unique<int>(6));

  REQUIRE(m.contains(a));
  CHECK(*m[a] == 5);
  CHECK(*m[b] == 6);

  REQUIRE(m.erase(a));
  CHECK(*m[b] == 6);
  CHECK(m.size() == 1);
}

TEST_CASE("slot_map: works with a 64-bit handle type") {
  nx::slot_map<int, nx::handle64> m;
  const auto a = m.insert(1);
  const auto b = m.insert(2);

  CHECK(a.index() == 0);
  CHECK(b.index() == 1);
  CHECK(m[a] == 1);
  CHECK(m.max_size() == nx::handle64::max_index);

  REQUIRE(m.erase(a));
  const auto c = m.insert(3);
  CHECK(c.index() == 0);
  CHECK(c.version() == 1);
  CHECK_FALSE(m.contains(a));
  CHECK(m[c] == 3);
}

TEST_CASE("slot_map: works with a tagged handle type") {
  struct Tag {};
  nx::slot_map<int, nx::typed_handle32<Tag>> m;
  const auto h = m.insert(7);
  CHECK(m[h] == 7);
  CHECK(m.contains(h));
  REQUIRE(m.erase(h));
  CHECK(m.empty());
}

TEST_CASE("slot_map: a slot retires once its version saturates") {
  using tiny = nx::basic_handle<u16, 15>;
  nx::slot_map<int, tiny> m;

  const auto a = m.insert(1);
  CHECK(a.version() == 0);
  REQUIRE(m.erase(a));

  const auto b = m.insert(2);
  CHECK(b.index() == a.index());
  CHECK(b.version() == 1);
  CHECK_FALSE(m.contains(a));

  REQUIRE(m.erase(b));
  const auto c = m.insert(3);
  CHECK(c.index() != a.index());
  CHECK_FALSE(m.contains(a));
  CHECK_FALSE(m.contains(b));
  CHECK(m.contains(c));
  CHECK(m[c] == 3);
}

TEST_CASE("slot_map: clear is genuinely noexcept, not just declared so") {
  static_assert(noexcept(std::declval<map_t &>().clear()));

  map_t m;
  for (int i = 0; i < 1000; ++i)
    (void)m.insert(i);
  CHECK(m.size() == 1000);

  m.clear();
  CHECK(m.empty());

  for (int round = 0; round < 20; ++round) {
    std::vector<handle_t> handles;
    for (int i = 0; i < 1000; ++i)
      handles.push_back(m.insert(i));
    REQUIRE(m.size() == 1000);
    for (const handle_t h : handles)
      REQUIRE(m.contains(h));
    m.clear();
    REQUIRE(m.empty());
  }

  const handle_t h = m.insert(42);
  CHECK(m[h] == 42);
}

TEST_CASE("slot_map: get_allocator is callable") {
  const map_t m;
  const auto a1 = m.get_allocator();
  const auto a2 = m.get_allocator();
  CHECK(a1 == a2);
}
