#include "framework/nxtest.h"

#include "core/foundation/containers/stable_pool.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
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

using pool_t = nx::stable_pool<int>;
using handle_t = pool_t::handle_type;

std::vector<int> sorted_values(const pool_t &p) {
  std::vector<int> out(p.begin(), p.end());
  std::sort(out.begin(), out.end());
  return out;
}

}

TEST_CASE("stable_pool: a fresh pool is empty") {
  const pool_t p;
  CHECK(p.empty());
  CHECK(p.size() == 0);
  CHECK(p.capacity() == 0);
  CHECK(p.begin() == p.end());
  CHECK(p.max_size() == handle_t::max_index);
  CHECK_FALSE(p.contains(handle_t::null()));
}

TEST_CASE("stable_pool: insert returns a handle that resolves to the value") {
  pool_t p;

  const handle_t a = p.insert(10);
  CHECK(p.size() == 1);
  CHECK_FALSE(p.empty());
  CHECK(p.contains(a));
  CHECK(p[a] == 10);
  CHECK(p.at(a) == 10);
  REQUIRE(p.try_get(a) != nullptr);
  CHECK(*p.try_get(a) == 10);

  const handle_t b = p.insert(20);
  CHECK(a != b);
  CHECK(p[a] == 10);
  CHECK(p[b] == 20);

  int tmp = 30;
  const handle_t c = p.insert(std::move(tmp));
  CHECK(p[c] == 30);
  CHECK(p.size() == 3);

  CHECK(p.capacity() >= 3);
}

TEST_CASE("stable_pool: emplace constructs in place") {
  nx::stable_pool<std::string> p;
  const auto a = p.emplace(3, 'x');
  CHECK(p[a] == "xxx");
  const auto b = p.emplace("hello");
  CHECK(p[b] == "hello");
  CHECK(p.size() == 2);
}

TEST_CASE("stable_pool: element addresses are stable across growth") {
  pool_t p;
  const handle_t first = p.insert(1);
  const int *addr = &p[first];

  std::vector<handle_t> handles;
  std::vector<const int *> addrs;
  for (int i = 0; i < 5000; ++i) {
    const handle_t h = p.insert(i);
    handles.push_back(h);
    addrs.push_back(&p[h]);
  }

  CHECK(&p[first] == addr);
  CHECK(p[first] == 1);
  for (usize i = 0; i < handles.size(); ++i) {
    CHECK(&p[handles[i]] == addrs[i]);
    CHECK(p[handles[i]] == static_cast<int>(i));
  }
}

TEST_CASE("stable_pool: addresses stay valid while other elements are erased") {
  pool_t p;
  std::vector<handle_t> handles;
  for (int i = 0; i < 100; ++i)
    handles.push_back(p.insert(i));

  const int *kept = &p[handles[50]];

  for (int i = 0; i < 100; ++i) {
    if (i == 50)
      continue;
    REQUIRE(p.erase(handles[static_cast<usize>(i)]));
  }
  CHECK(p.size() == 1);
  CHECK(&p[handles[50]] == kept);
  CHECK(p[handles[50]] == 50);
}

TEST_CASE("stable_pool: erase removes the element and invalidates the handle") {
  pool_t p;
  const handle_t a = p.insert(1);
  const handle_t b = p.insert(2);

  CHECK(p.erase(a));
  CHECK(p.size() == 1);
  CHECK_FALSE(p.contains(a));
  CHECK(p.try_get(a) == nullptr);
  CHECK(p.contains(b));
  CHECK(p[b] == 2);

  CHECK_FALSE(p.erase(a));
  CHECK_FALSE(p.erase(handle_t::from_parts(9999, 0)));
  CHECK_FALSE(p.erase(handle_t::null()));
  CHECK(p.size() == 1);

  CHECK(p.erase(b));
  CHECK(p.empty());
}

TEST_CASE("stable_pool: a stale handle to a recycled slot reports absent") {
  pool_t p;
  const handle_t a = p.insert(1);
  const auto index = a.index();

  REQUIRE(p.erase(a));

  const handle_t b = p.insert(2);
  CHECK(b.index() == index);
  CHECK(b.version() == a.version() + 1);

  CHECK_FALSE(p.contains(a));
  CHECK(p.contains(b));
  CHECK(p[b] == 2);
  CHECK(p.try_get(a) == nullptr);
  CHECK_FALSE(p.erase(a));
  CHECK(p.size() == 1);
  CHECK(p[b] == 2);
}

TEST_CASE("stable_pool: a wrong-version handle is rejected") {
  pool_t p;
  const handle_t a = p.insert(1);
  CHECK_FALSE(p.contains(handle_t::from_parts(a.index(), a.version() + 1)));
  CHECK_FALSE(p.contains(handle_t::from_parts(a.index(), 200)));
  CHECK(p.contains(a));

  CHECK_FALSE(p.contains(handle_t::from_parts(1000000, 0)));
  CHECK(p.try_get(handle_t::from_parts(1000000, 0)) == nullptr);
}

TEST_CASE("stable_pool: freed slots are recycled") {
  pool_t p;
  const handle_t a = p.insert(1);
  const handle_t b = p.insert(2);
  const handle_t c = p.insert(3);

  REQUIRE(p.erase(a));
  REQUIRE(p.erase(b));
  CHECK(p.size() == 1);

  const handle_t d = p.insert(4);
  CHECK(d.index() == b.index());
  const handle_t e = p.insert(5);
  CHECK(e.index() == a.index());

  CHECK(p.size() == 3);
  CHECK(p[c] == 3);
  CHECK(p[d] == 4);
  CHECK(p[e] == 5);
  CHECK_FALSE(p.contains(a));
  CHECK_FALSE(p.contains(b));
}

TEST_CASE("stable_pool: at() throws for an absent handle") {
  pool_t p;
  const handle_t a = p.insert(1);
  CHECK(p.at(a) == 1);
  p.at(a) = 9;
  CHECK(p[a] == 9);

  const pool_t &cp = p;
  CHECK(cp.at(a) == 9);
  CHECK(cp[a] == 9);
  REQUIRE(cp.try_get(a) != nullptr);

  REQUIRE(p.erase(a));
  CHECK_THROWS_AS(p.at(a), std::out_of_range);
  CHECK_THROWS_AS(cp.at(a), std::out_of_range);
  CHECK_THROWS_AS(p.at(handle_t::null()), std::out_of_range);
  CHECK(cp.try_get(a) == nullptr);
}

TEST_CASE("stable_pool: iteration visits every live element and skips holes") {
  pool_t p;
  std::vector<handle_t> handles;
  for (int i = 0; i < 10; ++i)
    handles.push_back(p.insert(i));

  CHECK(sorted_values(p) == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
  CHECK(std::distance(p.begin(), p.end()) == 10);

  REQUIRE(p.erase(handles[0]));
  REQUIRE(p.erase(handles[4]));
  REQUIRE(p.erase(handles[9]));

  CHECK(sorted_values(p) == std::vector<int>{1, 2, 3, 5, 6, 7, 8});
  CHECK(std::distance(p.begin(), p.end()) == 7);

  const pool_t &cp = p;
  CHECK(std::distance(cp.begin(), cp.end()) == 7);
  CHECK(std::distance(p.cbegin(), p.cend()) == 7);

  for (const handle_t h : handles)
    p.erase(h);
  CHECK(p.begin() == p.end());
  CHECK(std::distance(p.begin(), p.end()) == 0);
}

TEST_CASE("stable_pool: iteration is writable and exposes the handle") {
  pool_t p;
  std::vector<handle_t> handles;
  for (int i = 0; i < 5; ++i)
    handles.push_back(p.insert(i));
  REQUIRE(p.erase(handles[2]));

  for (auto it = p.begin(); it != p.end(); ++it) {
    CHECK(p.contains(it.handle()));
    CHECK(&p[it.handle()] == &*it);
    *it *= 10;
  }

  CHECK(p[handles[0]] == 0);
  CHECK(p[handles[1]] == 10);
  CHECK(p[handles[3]] == 30);
  CHECK(p[handles[4]] == 40);
}

TEST_CASE("stable_pool: the iterator supports post-increment and decrement") {
  pool_t p;
  std::vector<handle_t> handles;
  for (int i = 0; i < 4; ++i)
    handles.push_back(p.insert(i));

  auto it = p.begin();
  CHECK(*it == 0);

  const auto post = it++;
  CHECK(*post == 0);
  CHECK(*it == 1);

  ++it;
  CHECK(*it == 2);

  const auto postdec = it--;
  CHECK(*postdec == 2);
  CHECK(*it == 1);

  --it;
  CHECK(*it == 0);
  CHECK(it == p.begin());

  auto back = p.end();
  --back;
  CHECK(*back == 3);

  nx::stable_pool<std::string> sp;
  (void)sp.emplace("hello");
  CHECK(sp.begin()->size() == 5);
}

TEST_CASE("stable_pool: a mutable iterator converts to a const one") {
  pool_t p;
  (void)p.insert(7);

  const pool_t::iterator mit = p.begin();
  const pool_t::const_iterator cit = mit;
  CHECK(*cit == 7);
  CHECK(cit == mit);
  CHECK(cit != p.end());
}

TEST_CASE("stable_pool: reserve allocates pages up front") {
  pool_t p;
  p.reserve(1000);
  CHECK(p.capacity() >= 1000);
  CHECK(p.empty());

  CHECK(p.begin() == p.end());

  const handle_t a = p.insert(1);
  CHECK(p.size() == 1);
  CHECK(p[a] == 1);

  const usize cap = p.capacity();
  p.reserve(10);
  CHECK(p.capacity() == cap);
}

TEST_CASE("stable_pool: clear destroys everything and invalidates handles") {
  pool_t p;
  std::vector<handle_t> handles;
  for (int i = 0; i < 20; ++i)
    handles.push_back(p.insert(i));

  const usize cap = p.capacity();
  p.clear();
  CHECK(p.empty());
  CHECK(p.size() == 0);
  CHECK(p.begin() == p.end());
  CHECK(p.capacity() == cap);
  for (const handle_t h : handles) {
    CHECK_FALSE(p.contains(h));
    CHECK(p.try_get(h) == nullptr);
  }

  const handle_t a = p.insert(99);
  CHECK(p.size() == 1);
  CHECK(p[a] == 99);
  CHECK_FALSE(p.contains(handles[0]));

  p.clear();
  p.clear();
  CHECK(p.empty());
}

TEST_CASE("stable_pool: clear and erase destroy elements exactly once") {
  g_live = 0;
  {
    nx::stable_pool<Counted> p;
    std::vector<nx::stable_pool<Counted>::handle_type> handles;
    for (int i = 0; i < 50; ++i)
      handles.push_back(p.emplace(i));
    CHECK(g_live == 50);

    REQUIRE(p.erase(handles[10]));
    CHECK(g_live == 49);
    CHECK_FALSE(p.erase(handles[10]));
    CHECK(g_live == 49);

    p.clear();
    CHECK(g_live == 0);

    for (int i = 0; i < 5; ++i)
      (void)p.emplace(i);
    CHECK(g_live == 5);
  }
  CHECK(g_live == 0);
}

TEST_CASE("stable_pool: the destructor releases every live element") {
  g_live = 0;
  {
    nx::stable_pool<Counted> p;
    for (int i = 0; i < 1000; ++i)
      (void)p.emplace(i);
    CHECK(g_live == 1000);
  }
  CHECK(g_live == 0);
}

TEST_CASE("stable_pool: copy construction and assignment are deep") {
  g_live = 0;
  {
    nx::stable_pool<Counted> a;
    std::vector<nx::stable_pool<Counted>::handle_type> handles;
    for (int i = 0; i < 10; ++i)
      handles.push_back(a.emplace(i));
    REQUIRE(a.erase(handles[3]));
    CHECK(g_live == 9);

    const nx::stable_pool<Counted> b = a;
    CHECK(b.size() == 9);
    CHECK(g_live == 18);

    for (int i = 0; i < 10; ++i) {
      const auto h = handles[static_cast<usize>(i)];
      CHECK(b.contains(h) == a.contains(h));
      if (a.contains(h)) {
        CHECK(b[h].value == a[h].value);
        CHECK(&b[h] != &a[h]);
      }
    }

    nx::stable_pool<Counted> c;
    (void)c.emplace(99);
    c = a;
    CHECK(c.size() == 9);
    CHECK(c[handles[0]].value == 0);
    CHECK_FALSE(c.contains(handles[3]));

    const auto reused = c.emplace(1234);
    CHECK(reused.index() == handles[3].index());
    CHECK(a.size() == 9);
  }
  CHECK(g_live == 0);
}

TEST_CASE("stable_pool: move construction and assignment steal the pages") {
  g_live = 0;
  {
    nx::stable_pool<Counted> a;
    std::vector<nx::stable_pool<Counted>::handle_type> handles;
    for (int i = 0; i < 10; ++i)
      handles.push_back(a.emplace(i));
    const Counted *addr = &a[handles[0]];

    nx::stable_pool<Counted> b = std::move(a);
    CHECK(b.size() == 10);
    CHECK(&b[handles[0]] == addr);
    CHECK(a.empty());
    CHECK(a.capacity() == 0);
    CHECK_FALSE(a.contains(handles[0]));
    CHECK(g_live == 10);

    nx::stable_pool<Counted> c;
    (void)c.emplace(99);
    CHECK(g_live == 11);
    c = std::move(b);
    CHECK(c.size() == 10);
    CHECK(&c[handles[0]] == addr);
    CHECK(b.empty());
    CHECK(g_live == 10);
  }
  CHECK(g_live == 0);
}

TEST_CASE("stable_pool: self-assignment leaves the pool intact") {
  pool_t p;
  const handle_t a = p.insert(1);
  const handle_t b = p.insert(2);

  const pool_t *alias = &p;
  p = *alias;
  CHECK(p.size() == 2);
  CHECK(p[a] == 1);
  CHECK(p[b] == 2);

  pool_t *malias = &p;
  p = std::move(*malias);
  CHECK(p.size() == 2);
  CHECK(p[a] == 1);
}

TEST_CASE("stable_pool: a slot retires once its version saturates") {
  using tiny = nx::basic_handle<u16, 15>;
  nx::stable_pool<int, tiny> p;

  const auto a = p.insert(1);
  CHECK(a.version() == 0);
  REQUIRE(p.erase(a));

  const auto b = p.insert(2);
  CHECK(b.index() == a.index());
  CHECK(b.version() == 1);
  CHECK_FALSE(p.contains(a));

  REQUIRE(p.erase(b));
  const auto c = p.insert(3);
  CHECK(c.index() != a.index());
  CHECK_FALSE(p.contains(a));
  CHECK_FALSE(p.contains(b));
  CHECK(p[c] == 3);
  CHECK(p.size() == 1);

  p.clear();
  const auto d = p.insert(4);
  CHECK(d.index() != a.index());
  CHECK_FALSE(p.contains(a));
  CHECK_FALSE(p.contains(b));
}

TEST_CASE("stable_pool: clear bumps versions so old handles cannot resolve") {
  pool_t p;
  const handle_t a = p.insert(1);
  p.clear();

  const handle_t b = p.insert(2);
  CHECK(b.index() == a.index());
  CHECK(b.version() != a.version());
  CHECK_FALSE(p.contains(a));
  CHECK(p.contains(b));
}

TEST_CASE("stable_pool: holds move-only element types") {
  nx::stable_pool<std::unique_ptr<int>> p;
  const auto a = p.emplace(std::make_unique<int>(5));
  const auto b = p.emplace(std::make_unique<int>(6));

  CHECK(*p[a] == 5);
  CHECK(*p[b] == 6);
  REQUIRE(p.erase(a));
  CHECK(*p[b] == 6);
  CHECK(p.size() == 1);
}

TEST_CASE("stable_pool: works with a 64-bit handle type") {
  nx::stable_pool<int, nx::handle64> p;
  const auto a = p.insert(1);
  CHECK(p[a] == 1);
  CHECK(p.max_size() == nx::handle64::max_index);
  REQUIRE(p.erase(a));
  const auto b = p.insert(2);
  CHECK(b.index() == a.index());
  CHECK(b.version() == 1);
  CHECK_FALSE(p.contains(a));
}

TEST_CASE("stable_pool: get_allocator is callable") {
  const pool_t p;
  CHECK(p.get_allocator() == p.get_allocator());
}

TEST_CASE("stable_pool: survives a long insert/erase workload") {
  nx::stable_pool<Counted, nx::handle64> p;
  std::vector<std::pair<nx::handle64, int>> live;
  g_live = 0;

  u64 state = 0xFEEDFACEDEADBEEFull;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  for (int step = 0; step < 15000; ++step) {
    const u64 r = next();
    if (live.empty() || r % 3 != 0) {
      const int value = static_cast<int>(r >> 32);
      live.emplace_back(p.emplace(value), value);
    } else {
      const usize pick = static_cast<usize>(r >> 8) % live.size();
      REQUIRE(p.erase(live[pick].first));
      live[pick] = live.back();
      live.pop_back();
    }
    REQUIRE(p.size() == live.size());
    REQUIRE(g_live == static_cast<int>(live.size()));
  }

  for (const auto &[handle, value] : live) {
    REQUIRE(p.contains(handle));
    CHECK(p[handle].value == value);
  }
  CHECK(std::distance(p.begin(), p.end()) == static_cast<isize>(live.size()));

  p.clear();
  CHECK(g_live == 0);
}

namespace {

// Throws from the copy or move that brings the shared countdown to zero.
struct Brittle {
  static inline int countdown = -1;
  static inline int alive = 0;
  int value = 0;

  explicit Brittle(const int v) : value(v) { ++alive; }
  Brittle(const Brittle &o) : value(o.value) {
    tick();
    ++alive;
  }
  Brittle(Brittle &&o) : value(o.value) {
    tick();
    ++alive;
  }
  Brittle &operator=(const Brittle &) = default;
  Brittle &operator=(Brittle &&) = default;
  ~Brittle() { --alive; }

  static void tick() {
    if (countdown > 0 && --countdown == 0)
      throw std::runtime_error("brittle");
  }
};

} // namespace

TEST_CASE(
    "stable_pool: a throwing copy leaves a valid pool and leaks nothing") {
  Brittle::alive = 0;
  {
    nx::stable_pool<Brittle> source;
    for (int i = 0; i < 5; ++i)
      (void)source.emplace(i);
    Brittle::countdown = 3;
    CHECK_THROWS_AS((nx::stable_pool<Brittle>(source)), std::runtime_error);
    Brittle::countdown = -1;
    CHECK(Brittle::alive == 5);

    nx::stable_pool<Brittle> target;
    (void)target.emplace(42);
    Brittle::countdown = 2;
    CHECK_THROWS_AS(target = source, std::runtime_error);
    Brittle::countdown = -1;
    usize seen = 0;
    for (const Brittle &b : target) {
      (void)b;
      ++seen;
    }
    CHECK(seen == target.size());
    CHECK(Brittle::alive == 5 + nx::cast<int>(target.size()));
  }
  CHECK(Brittle::alive == 0);
}
