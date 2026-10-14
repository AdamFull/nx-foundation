#include "framework/nxtest.h"

#include "core/foundation/containers/fixed_vector.h"

#include <list>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_live = 0;

struct Counted {
  int value;
  Counted() : value(0) { ++g_live; }
  explicit Counted(int v) : value(v) { ++g_live; }
  Counted(const Counted &o) : value(o.value) { ++g_live; }
  Counted(Counted &&o) noexcept : value(o.value) {
    o.value = -1;
    ++g_live;
  }
  Counted &operator=(const Counted &) = default;
  Counted &operator=(Counted &&) = default;
  ~Counted() { --g_live; }
  bool operator==(const Counted &o) const { return value == o.value; }
  auto operator<=>(const Counted &o) const { return value <=> o.value; }
};

using fv5 = nx::fixed_vector<int, 5>;

std::vector<int> to_vec(const fv5 &v) { return {v.begin(), v.end()}; }

}

TEST_CASE("fixed_vector: default-constructed is empty with full capacity") {
  const fv5 v;
  CHECK(v.empty());
  CHECK(v.size() == 0);
  CHECK(v.capacity() == 5);
  CHECK(v.max_size() == 5);
  CHECK(v.begin() == v.end());

  CHECK(sizeof(fv5) >= 5 * sizeof(int));
}

TEST_CASE("fixed_vector: sized and fill construction") {
  const nx::fixed_vector<int, 8> a(3);
  CHECK(a.size() == 3);
  for (const int v : a)
    CHECK(v == 0);

  const nx::fixed_vector<int, 8> b(4, 7);
  CHECK(b.size() == 4);
  for (const int v : b)
    CHECK(v == 7);

  const nx::fixed_vector<int, 8> z(usize(0));
  CHECK(z.empty());
  const nx::fixed_vector<int, 8> zf(0, 7);
  CHECK(zf.empty());

  const nx::fixed_vector<int, 4> full(4, 1);
  CHECK(full.size() == 4);
  CHECK(full.size() == full.capacity());
}

TEST_CASE("fixed_vector: construction from ranges, lists and spans") {
  const std::vector<int> src{1, 2, 3};

  const nx::fixed_vector<int, 8> a(src.begin(), src.end());
  CHECK(to_vec(fv5(a.begin(), a.end())) == src);

  const nx::fixed_vector<int, 8> b{4, 5, 6};
  CHECK(b.size() == 3);
  CHECK(b[0] == 4);
  CHECK(b[2] == 6);

  const nx::fixed_vector<int, 8> c{std::span<const int>(src)};
  CHECK(c.size() == 3);
  CHECK(c[1] == 2);

  const std::list<int> lst{7, 8};
  const nx::fixed_vector<int, 8> d(lst.begin(), lst.end());
  CHECK(d.size() == 2);
  CHECK(d[0] == 7);
  CHECK(d[1] == 8);

  const nx::fixed_vector<int, 8> e(src.begin(), src.begin());
  CHECK(e.empty());
  const nx::fixed_vector<int, 8> f{};
  CHECK(f.empty());
}

TEST_CASE("fixed_vector: push_back, emplace_back and pop_back") {
  fv5 v;

  v.push_back(1);
  CHECK(v.size() == 1);
  CHECK(v.back() == 1);

  int lvalue = 2;
  v.push_back(lvalue);
  CHECK(v.back() == 2);

  int &ref = v.emplace_back(3);
  CHECK(ref == 3);
  CHECK(&ref == &v.back());
  ref = 30;
  CHECK(v.back() == 30);

  CHECK(to_vec(v) == std::vector<int>{1, 2, 30});

  v.pop_back();
  CHECK(v.size() == 2);
  CHECK(v.back() == 2);
  v.pop_back();
  v.pop_back();
  CHECK(v.empty());

  for (int i = 0; i < 5; ++i)
    v.push_back(i);
  CHECK(v.size() == 5);
  CHECK(v.size() == v.capacity());
  CHECK(v.front() == 0);
  CHECK(v.back() == 4);
}

TEST_CASE("fixed_vector: emplace_back forwards constructor arguments") {
  nx::fixed_vector<std::string, 4> v;
  v.emplace_back(3, 'x');
  CHECK(v.back() == "xxx");
  v.emplace_back("hello");
  CHECK(v.back() == "hello");

  nx::fixed_vector<std::pair<int, std::string>, 4> p;
  p.emplace_back(1, "a");
  CHECK(p.back().first == 1);
  CHECK(p.back().second == "a");
}

TEST_CASE("fixed_vector: element access and bounds-checked at()") {
  fv5 v{10, 20, 30};

  CHECK(v[0] == 10);
  v[1] = 99;
  CHECK(v[1] == 99);
  CHECK(v.front() == 10);
  CHECK(v.back() == 30);
  CHECK(v.data()[2] == 30);
  CHECK(&v.front() == v.data());
  CHECK(&v.back() == v.data() + 2);

  CHECK(v.at(0) == 10);
  v.at(0) = 1;
  CHECK(v[0] == 1);
  CHECK_THROWS_AS(v.at(3), std::out_of_range);
  CHECK_THROWS_AS(v.at(usize(-1)), std::out_of_range);

  const fv5 &cv = v;
  CHECK(cv[0] == 1);
  CHECK(cv.front() == 1);
  CHECK(cv.back() == 30);
  CHECK(cv.at(2) == 30);
  CHECK_THROWS_AS(cv.at(3), std::out_of_range);

  fv5 e;
  CHECK_THROWS_AS(e.at(0), std::out_of_range);
}

TEST_CASE("fixed_vector: forward and reverse iteration") {
  fv5 v{1, 2, 3, 4};

  CHECK(std::accumulate(v.begin(), v.end(), 0) == 10);
  CHECK(std::distance(v.begin(), v.end()) == 4);
  CHECK(std::vector<int>(v.rbegin(), v.rend()) == std::vector<int>{4, 3, 2, 1});

  const fv5 &cv = v;
  CHECK(std::vector<int>(cv.begin(), cv.end()) == std::vector<int>{1, 2, 3, 4});
  CHECK(std::vector<int>(cv.rbegin(), cv.rend()) ==
        std::vector<int>{4, 3, 2, 1});
  CHECK(std::vector<int>(v.cbegin(), v.cend()) == std::vector<int>{1, 2, 3, 4});
  CHECK(std::vector<int>(v.crbegin(), v.crend()) ==
        std::vector<int>{4, 3, 2, 1});

  for (int &x : v)
    x *= 10;
  CHECK(v[0] == 10);
  CHECK(v[3] == 40);

  const fv5 e;
  CHECK(e.begin() == e.end());
  CHECK(e.rbegin() == e.rend());
}

TEST_CASE("fixed_vector: clear destroys every element") {
  g_live = 0;
  {
    nx::fixed_vector<Counted, 8> v;
    for (int i = 0; i < 5; ++i)
      v.emplace_back(i);
    CHECK(g_live == 5);

    v.clear();
    CHECK(v.empty());
    CHECK(g_live == 0);

    v.emplace_back(1);
    CHECK(v.size() == 1);
    CHECK(g_live == 1);

    v.clear();
    v.clear();
    CHECK(g_live == 0);
  }
  CHECK(g_live == 0);
}

TEST_CASE("fixed_vector: pop_back destroys the removed element") {
  g_live = 0;
  {
    nx::fixed_vector<Counted, 8> v;
    v.emplace_back(1);
    v.emplace_back(2);
    CHECK(g_live == 2);
    v.pop_back();
    CHECK(g_live == 1);
    CHECK(v.back().value == 1);
  }
  CHECK(g_live == 0);
}

TEST_CASE("fixed_vector: assign replaces the contents") {
  fv5 v{1, 2, 3};

  v.assign(2, 9);
  CHECK(to_vec(v) == std::vector<int>{9, 9});

  const std::vector<int> src{4, 5, 6, 7};
  v.assign(src.begin(), src.end());
  CHECK(to_vec(v) == src);

  v.assign({1, 2});
  CHECK(to_vec(v) == std::vector<int>{1, 2});

  v.assign(0, 1);
  CHECK(v.empty());
  v.assign(src.begin(), src.begin());
  CHECK(v.empty());

  v = {8, 9, 10};
  CHECK(to_vec(v) == std::vector<int>{8, 9, 10});

  g_live = 0;
  {
    nx::fixed_vector<Counted, 8> c;
    c.emplace_back(1);
    c.emplace_back(2);
    CHECK(g_live == 2);
    c.assign(3, Counted{5});
    CHECK(g_live == 3);
  }
  CHECK(g_live == 0);
}

TEST_CASE("fixed_vector: insert places a single element at any position") {
  fv5 v{1, 2, 4};

  auto it = v.insert(v.begin() + 2, 3);
  CHECK(*it == 3);
  CHECK(to_vec(v) == std::vector<int>{1, 2, 3, 4});

  it = v.insert(v.begin(), 0);
  CHECK(*it == 0);
  CHECK(to_vec(v) == std::vector<int>{0, 1, 2, 3, 4});

  fv5 w{1, 2};
  it = w.insert(w.end(), 3);
  CHECK(*it == 3);
  CHECK(to_vec(w) == std::vector<int>{1, 2, 3});

  fv5 e;
  it = e.insert(e.begin(), 42);
  CHECK(*it == 42);
  CHECK(e.size() == 1);

  nx::fixed_vector<std::string, 4> s{"b"};
  s.insert(s.begin(), std::string("a"));
  CHECK(s[0] == "a");
  CHECK(s[1] == "b");
}

TEST_CASE("fixed_vector: insert of a repeated value") {
  fv5 v{1, 4};

  auto it = v.insert(v.begin() + 1, 2, 9);
  CHECK(*it == 9);
  CHECK(to_vec(v) == std::vector<int>{1, 9, 9, 4});

  const auto before = to_vec(v);
  it = v.insert(v.begin() + 1, 0, 5);
  CHECK(to_vec(v) == before);
  CHECK(it == v.begin() + 1);

  fv5 w{1};
  w.insert(w.end(), 3, 7);
  CHECK(to_vec(w) == std::vector<int>{1, 7, 7, 7});

  fv5 x{9};
  x.insert(x.begin(), 4, 0);
  CHECK(x.size() == 5);
  CHECK(x.front() == 0);
  CHECK(x.back() == 9);
}

TEST_CASE("fixed_vector: insert of a range, random access and single pass") {
  const std::vector<int> src{7, 8};

  fv5 v{1, 4};
  auto it = v.insert(v.begin() + 1, src.begin(), src.end());
  CHECK(*it == 7);
  CHECK(to_vec(v) == std::vector<int>{1, 7, 8, 4});

  const auto before = to_vec(v);
  it = v.insert(v.begin() + 1, src.begin(), src.begin());
  CHECK(to_vec(v) == before);
  CHECK(it == v.begin() + 1);

  const std::list<int> lst{5, 6};
  fv5 w{1, 4};
  it = w.insert(w.begin() + 1, lst.begin(), lst.end());
  CHECK(*it == 5);
  CHECK(to_vec(w) == std::vector<int>{1, 5, 6, 4});

  fv5 x{1};
  x.insert(x.end(), lst.begin(), lst.end());
  CHECK(to_vec(x) == std::vector<int>{1, 5, 6});
  fv5 y{1};
  y.insert(y.begin(), lst.begin(), lst.end());
  CHECK(to_vec(y) == std::vector<int>{5, 6, 1});

  fv5 z{1, 4};
  z.insert(z.begin() + 1, {2, 3});
  CHECK(to_vec(z) == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE("fixed_vector: insert shifts correctly when count exceeds the tail") {
  fv5 few{1, 2};
  few.insert(few.begin(), 3, 9);
  CHECK(to_vec(few) == std::vector<int>{9, 9, 9, 1, 2});

  nx::fixed_vector<int, 10> many{1, 2, 3, 4, 5};
  many.insert(many.begin(), 2, 9);
  CHECK(std::vector<int>(many.begin(), many.end()) ==
        std::vector<int>{9, 9, 1, 2, 3, 4, 5});

  nx::fixed_vector<int, 10> eq{1, 2, 3};
  eq.insert(eq.begin(), 3, 0);
  CHECK(std::vector<int>(eq.begin(), eq.end()) ==
        std::vector<int>{0, 0, 0, 1, 2, 3});
}

TEST_CASE("fixed_vector: insert of non-trivial types leaves no leaks") {
  g_live = 0;
  {
    nx::fixed_vector<Counted, 16> v;
    for (int i = 0; i < 4; ++i)
      v.emplace_back(i);
    CHECK(g_live == 4);

    v.insert(v.begin() + 2, Counted{99});
    CHECK(v.size() == 5);
    CHECK(v[2].value == 99);
    CHECK(g_live == 5);

    v.insert(v.begin(), 3, Counted{7});
    CHECK(v.size() == 8);
    CHECK(g_live == 8);
    CHECK(v[0].value == 7);
    CHECK(v[3].value == 0);
  }
  CHECK(g_live == 0);
}

TEST_CASE("fixed_vector: emplace constructs in place at a position") {
  nx::fixed_vector<std::string, 8> v{"a", "d"};
  const auto it = v.emplace(v.begin() + 1, 3, 'b');
  CHECK(*it == "bbb");
  CHECK(v.size() == 3);
  CHECK(v[0] == "a");
  CHECK(v[1] == "bbb");
  CHECK(v[2] == "d");
}

TEST_CASE("fixed_vector: erase removes one element and returns the successor") {
  fv5 v{1, 2, 3, 4};

  auto it = v.erase(v.begin() + 1);
  CHECK(*it == 3);
  CHECK(to_vec(v) == std::vector<int>{1, 3, 4});

  it = v.erase(v.end() - 1);
  CHECK(it == v.end());
  CHECK(to_vec(v) == std::vector<int>{1, 3});

  it = v.erase(v.begin());
  CHECK(*it == 3);
  CHECK(to_vec(v) == std::vector<int>{3});

  it = v.erase(v.begin());
  CHECK(v.empty());
  CHECK(it == v.end());
}

TEST_CASE("fixed_vector: erase removes a range") {
  fv5 v{1, 2, 3, 4, 5};

  auto it = v.erase(v.begin() + 1, v.begin() + 3);
  CHECK(*it == 4);
  CHECK(to_vec(v) == std::vector<int>{1, 4, 5});

  it = v.erase(v.begin() + 1, v.begin() + 1);
  CHECK(to_vec(v) == std::vector<int>{1, 4, 5});
  CHECK(it == v.begin() + 1);

  it = v.erase(v.begin(), v.end());
  CHECK(v.empty());
  CHECK(it == v.begin());

  fv5 w{1, 2, 3, 4};
  w.erase(w.begin() + 2, w.end());
  CHECK(to_vec(w) == std::vector<int>{1, 2});
}

TEST_CASE("fixed_vector: erase destroys exactly the removed elements") {
  g_live = 0;
  {
    nx::fixed_vector<Counted, 16> v;
    for (int i = 0; i < 6; ++i)
      v.emplace_back(i);
    CHECK(g_live == 6);

    v.erase(v.begin() + 1);
    CHECK(g_live == 5);
    CHECK(v[1].value == 2);

    v.erase(v.begin(), v.begin() + 2);
    CHECK(g_live == 3);
    CHECK(v[0].value == 3);
  }
  CHECK(g_live == 0);
}

TEST_CASE(
    "fixed_vector: resize grows with defaults and shrinks by destroying") {
  fv5 v{1, 2, 3};

  v.resize(5);
  CHECK(v.size() == 5);
  CHECK(v[3] == 0);
  CHECK(v[4] == 0);
  CHECK(v[0] == 1);

  v.resize(2);
  CHECK(to_vec(v) == std::vector<int>{1, 2});

  v.resize(4, 7);
  CHECK(to_vec(v) == std::vector<int>{1, 2, 7, 7});

  v.resize(0);
  CHECK(v.empty());

  fv5 w{1, 2};
  w.resize(2);
  CHECK(to_vec(w) == std::vector<int>{1, 2});
  w.resize(2, 9);
  CHECK(to_vec(w) == std::vector<int>{1, 2});

  g_live = 0;
  {
    nx::fixed_vector<Counted, 8> c(6);
    CHECK(g_live == 6);
    c.resize(2);
    CHECK(g_live == 2);
    c.resize(5);
    CHECK(g_live == 5);
  }
  CHECK(g_live == 0);
}

TEST_CASE("fixed_vector: copy construction and assignment are deep") {
  g_live = 0;
  {
    nx::fixed_vector<Counted, 8> a;
    for (int i = 0; i < 3; ++i)
      a.emplace_back(i);

    const nx::fixed_vector<Counted, 8> b = a;
    CHECK(b.size() == 3);
    CHECK(b[1].value == 1);
    CHECK(g_live == 6);
    CHECK(b.data() != a.data());

    nx::fixed_vector<Counted, 8> c;
    c.emplace_back(99);
    c = a;
    CHECK(c.size() == 3);
    CHECK(c[0].value == 0);
    CHECK(g_live == 9);

    c[0].value = 42;
    CHECK(a[0].value == 0);
  }
  CHECK(g_live == 0);
}

TEST_CASE("fixed_vector: move construction and assignment empty the source") {
  g_live = 0;
  {
    nx::fixed_vector<Counted, 8> a;
    for (int i = 0; i < 3; ++i)
      a.emplace_back(i);

    const nx::fixed_vector<Counted, 8> b = std::move(a);
    CHECK(b.size() == 3);
    CHECK(b[2].value == 2);
    CHECK(a.empty());
    CHECK(g_live == 3);

    nx::fixed_vector<Counted, 8> c;
    c.emplace_back(9);
    nx::fixed_vector<Counted, 8> d;
    d.emplace_back(1);
    d.emplace_back(2);
    c = std::move(d);
    CHECK(c.size() == 2);
    CHECK(d.empty());
    CHECK(g_live == 5);
  }
  CHECK(g_live == 0);
}

TEST_CASE("fixed_vector: self-assignment leaves the contents intact") {
  fv5 v{1, 2, 3};
  const fv5 *alias = &v;
  v = *alias;
  CHECK(to_vec(v) == std::vector<int>{1, 2, 3});

  fv5 *malias = &v;
  v = std::move(*malias);
  CHECK(to_vec(v) == std::vector<int>{1, 2, 3});
}

TEST_CASE("fixed_vector: swap exchanges contents of different sizes") {
  fv5 a{1, 2, 3};
  fv5 b{9};

  a.swap(b);
  CHECK(to_vec(a) == std::vector<int>{9});
  CHECK(to_vec(b) == std::vector<int>{1, 2, 3});

  swap(a, b);
  CHECK(to_vec(a) == std::vector<int>{1, 2, 3});
  CHECK(to_vec(b) == std::vector<int>{9});

  fv5 e;
  a.swap(e);
  CHECK(a.empty());
  CHECK(to_vec(e) == std::vector<int>{1, 2, 3});
  a.swap(e);
  CHECK(to_vec(a) == std::vector<int>{1, 2, 3});

  a.swap(a);
  CHECK(to_vec(a) == std::vector<int>{1, 2, 3});

  g_live = 0;
  {
    nx::fixed_vector<Counted, 8> x, y;
    for (int i = 0; i < 4; ++i)
      x.emplace_back(i);
    y.emplace_back(100);
    CHECK(g_live == 5);
    x.swap(y);
    CHECK(x.size() == 1);
    CHECK(y.size() == 4);
    CHECK(x[0].value == 100);
    CHECK(y[3].value == 3);
    CHECK(g_live == 5);
  }
  CHECK(g_live == 0);
}

TEST_CASE("fixed_vector: converts to spans that alias the storage") {
  fv5 v{1, 2, 3};

  const std::span<const int> cs = v;
  CHECK(cs.size() == 3);
  CHECK(cs.data() == v.data());

  const auto ms = static_cast<std::span<int>>(v);
  ms[0] = 99;
  CHECK(v[0] == 99);

  const fv5 e;
  const std::span<const int> es = e;
  CHECK(es.empty());
}

TEST_CASE("fixed_vector: equality and ordering") {
  const fv5 a{1, 2, 3};
  const fv5 b{1, 2, 3};
  const fv5 c{1, 2, 4};
  const fv5 shorter{1, 2};
  const fv5 e;

  CHECK(a == b);
  CHECK(a != c);
  CHECK(a != shorter);
  CHECK_FALSE(a == e);

  CHECK(a < c);
  CHECK(c > a);
  CHECK(shorter < a);
  CHECK(e < shorter);
  CHECK(a <= b);
  CHECK(a >= b);
  CHECK((a <=> b) == std::strong_ordering::equal);
}

TEST_CASE("fixed_vector: erase and erase_if free functions") {
  fv5 v{1, 2, 3, 2, 5};

  CHECK(nx::erase(v, 2) == 2);
  CHECK(to_vec(v) == std::vector<int>{1, 3, 5});

  CHECK(nx::erase(v, 42) == 0);
  CHECK(to_vec(v) == std::vector<int>{1, 3, 5});

  CHECK(nx::erase_if(v, [](int x) { return x > 2; }) == 2);
  CHECK(to_vec(v) == std::vector<int>{1});

  CHECK(nx::erase_if(v, [](int) { return false; }) == 0);
  CHECK(v.size() == 1);
  CHECK(nx::erase_if(v, [](int) { return true; }) == 1);
  CHECK(v.empty());

  CHECK(nx::erase(v, 1) == 0);
  CHECK(nx::erase_if(v, [](int) { return true; }) == 0);
}

TEST_CASE("fixed_vector: destructor releases all elements") {
  g_live = 0;
  {
    nx::fixed_vector<Counted, 32> v;
    for (int i = 0; i < 32; ++i)
      v.emplace_back(i);
    CHECK(g_live == 32);
  }
  CHECK(g_live == 0);
}

TEST_CASE("fixed_vector: mirrors std::vector under a long mixed workload") {
  constexpr usize cap = 64;
  nx::fixed_vector<int, cap> v;
  std::vector<int> model;

  u64 state = 0x9E3779B97F4A7C15ull;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  for (int step = 0; step < 15000; ++step) {
    const u64 r = next();
    const int op = static_cast<int>(r % 6);
    const usize pos =
        model.empty() ? 0 : static_cast<usize>(r >> 8) % model.size();
    const int value = static_cast<int>(r >> 32);

    switch (op) {
    case 0:
    case 1:
      if (model.size() < cap) {
        v.push_back(value);
        model.push_back(value);
      }
      break;
    case 2:
      if (!model.empty()) {
        v.pop_back();
        model.pop_back();
      }
      break;
    case 3:
      if (model.size() < cap) {
        v.insert(v.begin() + static_cast<isize>(pos), value);
        model.insert(model.begin() + static_cast<isize>(pos), value);
      }
      break;
    case 4:
      if (!model.empty()) {
        v.erase(v.begin() + static_cast<isize>(pos));
        model.erase(model.begin() + static_cast<isize>(pos));
      }
      break;
    default: {
      const usize n = static_cast<usize>(r >> 16) % (cap + 1);
      v.resize(n);
      model.resize(n);
      break;
    }
    }

    REQUIRE(v.size() == model.size());
  }

  REQUIRE(v.size() == model.size());
  CHECK(std::vector<int>(v.begin(), v.end()) == model);
}
