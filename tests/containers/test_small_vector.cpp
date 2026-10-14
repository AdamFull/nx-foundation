#include "framework/nxtest.h"

#include "core/foundation/containers/small_vector.h"

#include <list>
#include <memory>
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

using sv4 = nx::small_vector<int, 4>;

std::vector<int> to_vec(const sv4 &v) { return {v.begin(), v.end()}; }

}

TEST_CASE("small_vector: starts inline, empty, with N capacity") {
  const sv4 v;
  CHECK(v.empty());
  CHECK(v.size() == 0);
  CHECK(v.capacity() == 4);
  CHECK(v.begin() == v.end());

  const auto addr = reinterpret_cast<uintptr_t>(v.data());
  const auto self = reinterpret_cast<uintptr_t>(&v);
  CHECK(addr >= self);
  CHECK(addr < self + sizeof(sv4));
}

TEST_CASE("small_vector: stays inline until it exceeds N, then spills") {
  sv4 v;
  const int *inline_data = v.data();

  for (int i = 0; i < 4; ++i) {
    v.push_back(i);
    CHECK(v.data() == inline_data);
    CHECK(v.capacity() == 4);
  }

  v.push_back(4);
  CHECK(v.data() != inline_data);
  CHECK(v.capacity() > 4);
  CHECK(v.size() == 5);
  CHECK(to_vec(v) == std::vector<int>{0, 1, 2, 3, 4});

  for (int i = 5; i < 1000; ++i)
    v.push_back(i);
  CHECK(v.size() == 1000);
  for (int i = 0; i < 1000; ++i)
    CHECK_FALSE(v[static_cast<usize>(i)] != i);
}

TEST_CASE("small_vector: sized and fill construction") {
  const sv4 a(3);
  CHECK(a.size() == 3);
  for (const int v : a)
    CHECK(v == 0);

  const sv4 b(10);
  CHECK(b.size() == 10);
  CHECK(b.capacity() >= 10);

  const sv4 c(3, 7);
  CHECK(to_vec(c) == std::vector<int>{7, 7, 7});

  const sv4 d(10, 9);
  CHECK(d.size() == 10);
  CHECK(d.front() == 9);
  CHECK(d.back() == 9);

  const sv4 z(usize(0));
  CHECK(z.empty());
  const sv4 zf(0, 1);
  CHECK(zf.empty());
}

TEST_CASE("small_vector: construction from ranges, lists and spans") {
  const std::vector<int> src{1, 2, 3, 4, 5, 6};

  const sv4 a(src.begin(), src.end());
  CHECK(to_vec(a) == src);

  const sv4 b{1, 2, 3};
  CHECK(to_vec(b) == std::vector<int>{1, 2, 3});

  const sv4 c{std::span<const int>(src)};
  CHECK(to_vec(c) == src);

  const std::list<int> lst{9, 8, 7, 6, 5};
  const sv4 d(lst.begin(), lst.end());
  CHECK(to_vec(d) == std::vector<int>{9, 8, 7, 6, 5});

  const sv4 e(src.begin(), src.begin());
  CHECK(e.empty());
}

TEST_CASE("small_vector: push_back, emplace_back and pop_back") {
  sv4 v;
  v.push_back(1);
  int lvalue = 2;
  v.push_back(lvalue);
  int &ref = v.emplace_back(3);
  CHECK(ref == 3);
  CHECK(&ref == &v.back());
  CHECK(to_vec(v) == std::vector<int>{1, 2, 3});

  v.pop_back();
  CHECK(to_vec(v) == std::vector<int>{1, 2});

  nx::small_vector<std::string, 2> s;
  s.emplace_back(3, 'z');
  CHECK(s.back() == "zzz");
  s.emplace_back("abc");
  CHECK(s.back() == "abc");
  s.emplace_back("spill");
  CHECK(s.size() == 3);
  CHECK(s[0] == "zzz");
  CHECK(s[2] == "spill");
}

TEST_CASE("small_vector: element access and bounds-checked at()") {
  sv4 v{10, 20, 30};

  CHECK(v[0] == 10);
  v[1] = 99;
  CHECK(v[1] == 99);
  CHECK(v.front() == 10);
  CHECK(v.back() == 30);
  CHECK(&v.front() == v.data());
  CHECK(&v.back() == v.data() + 2);

  CHECK(v.at(2) == 30);
  v.at(2) = 3;
  CHECK(v[2] == 3);
  CHECK_THROWS_AS(v.at(3), std::out_of_range);
  CHECK_THROWS_AS(v.at(usize(-1)), std::out_of_range);

  const sv4 &cv = v;
  CHECK(cv[0] == 10);
  CHECK(cv.at(0) == 10);
  CHECK(cv.front() == 10);
  CHECK(cv.back() == 3);
  CHECK_THROWS_AS(cv.at(3), std::out_of_range);

  sv4 e;
  CHECK_THROWS_AS(e.at(0), std::out_of_range);
}

TEST_CASE("small_vector: forward and reverse iteration") {
  sv4 v{1, 2, 3, 4, 5};

  CHECK(std::accumulate(v.begin(), v.end(), 0) == 15);
  CHECK(std::vector<int>(v.rbegin(), v.rend()) ==
        std::vector<int>{5, 4, 3, 2, 1});

  const sv4 &cv = v;
  CHECK(std::vector<int>(cv.begin(), cv.end()) == to_vec(v));
  CHECK(std::vector<int>(cv.rbegin(), cv.rend()) ==
        std::vector<int>{5, 4, 3, 2, 1});
  CHECK(std::vector<int>(v.cbegin(), v.cend()) == to_vec(v));
  CHECK(std::vector<int>(v.crbegin(), v.crend()) ==
        std::vector<int>{5, 4, 3, 2, 1});

  for (int &x : v)
    x *= 2;
  CHECK(v[0] == 2);

  const sv4 e;
  CHECK(e.begin() == e.end());
  CHECK(e.rbegin() == e.rend());
}

TEST_CASE("small_vector: reserve grows capacity without changing contents") {
  sv4 v{1, 2};
  CHECK(v.capacity() == 4);

  v.reserve(3);
  CHECK(v.capacity() == 4);

  v.reserve(100);
  CHECK(v.capacity() >= 100);
  CHECK(to_vec(v) == std::vector<int>{1, 2});

  const usize cap = v.capacity();
  v.reserve(1);
  CHECK(v.capacity() == cap);
}

TEST_CASE(
    "small_vector: shrink_to_fit returns to the inline buffer when it fits") {
  sv4 v;
  for (int i = 0; i < 100; ++i)
    v.push_back(i);
  CHECK(v.capacity() >= 100);
  CHECK(v.data() != reinterpret_cast<const int *>(&v));

  v.resize(50);
  v.shrink_to_fit();
  CHECK(v.capacity() == 50);
  CHECK(v.size() == 50);
  CHECK(v[49] == 49);

  v.resize(3);
  v.shrink_to_fit();
  CHECK(v.capacity() == 4);
  CHECK(to_vec(v) == std::vector<int>{0, 1, 2});
  const auto addr = reinterpret_cast<uintptr_t>(v.data());
  const auto self = reinterpret_cast<uintptr_t>(&v);
  CHECK(addr >= self);
  CHECK(addr < self + sizeof(sv4));

  v.shrink_to_fit();
  CHECK(v.capacity() == 4);
  CHECK(to_vec(v) == std::vector<int>{0, 1, 2});

  g_live = 0;
  {
    nx::small_vector<Counted, 4> c;
    for (int i = 0; i < 20; ++i)
      c.emplace_back(i);
    CHECK(g_live == 20);
    c.resize(2);
    CHECK(g_live == 2);
    c.shrink_to_fit();
    CHECK(g_live == 2);
    CHECK(c[0].value == 0);
    CHECK(c[1].value == 1);
  }
  CHECK(g_live == 0);
}

TEST_CASE("small_vector: clear destroys elements and keeps the buffer") {
  g_live = 0;
  {
    nx::small_vector<Counted, 4> v;
    for (int i = 0; i < 10; ++i)
      v.emplace_back(i);
    CHECK(g_live == 10);
    const usize cap = v.capacity();

    v.clear();
    CHECK(v.empty());
    CHECK(g_live == 0);
    CHECK(v.capacity() == cap);

    v.emplace_back(1);
    CHECK(g_live == 1);
    v.clear();
    v.clear();
    CHECK(g_live == 0);
  }
  CHECK(g_live == 0);
}

TEST_CASE("small_vector: assign replaces the contents") {
  sv4 v{1, 2, 3};

  v.assign(2, 9);
  CHECK(to_vec(v) == std::vector<int>{9, 9});

  v.assign(10, 5);
  CHECK(v.size() == 10);
  CHECK(v.capacity() >= 10);
  CHECK(v.front() == 5);
  CHECK(v.back() == 5);

  const std::vector<int> src{1, 2, 3, 4, 5, 6, 7};
  v.assign(src.begin(), src.end());
  CHECK(to_vec(v) == src);

  v.assign({1, 2});
  CHECK(to_vec(v) == std::vector<int>{1, 2});

  const std::list<int> lst{7, 8, 9};
  v.assign(lst.begin(), lst.end());
  CHECK(to_vec(v) == std::vector<int>{7, 8, 9});

  v.assign(0, 1);
  CHECK(v.empty());

  v = {4, 5};
  CHECK(to_vec(v) == std::vector<int>{4, 5});
}

TEST_CASE("small_vector: insert a single element, inline and spilled") {
  sv4 v{1, 2, 4};

  auto it = v.insert(v.begin() + 2, 3);
  CHECK(*it == 3);
  CHECK(to_vec(v) == std::vector<int>{1, 2, 3, 4});

  it = v.insert(v.begin(), 0);
  CHECK(*it == 0);
  CHECK(it == v.begin());
  CHECK(to_vec(v) == std::vector<int>{0, 1, 2, 3, 4});

  it = v.insert(v.end(), 5);
  CHECK(*it == 5);
  CHECK(v.back() == 5);

  sv4 e;
  it = e.insert(e.begin(), 42);
  CHECK(*it == 42);
  CHECK(e.size() == 1);

  nx::small_vector<std::string, 2> s{"b"};
  s.insert(s.begin(), std::string("a"));
  CHECK(s[0] == "a");
  CHECK(s[1] == "b");
}

TEST_CASE("small_vector: insert a repeated value") {
  sv4 v{1, 4};
  auto it = v.insert(v.begin() + 1, 2, 9);
  CHECK(*it == 9);
  CHECK(to_vec(v) == std::vector<int>{1, 9, 9, 4});

  const auto before = to_vec(v);
  it = v.insert(v.begin() + 1, 0, 5);
  CHECK(to_vec(v) == before);
  CHECK(it == v.begin() + 1);

  v.insert(v.begin(), 20, 7);
  CHECK(v.size() == 24);
  CHECK(v.front() == 7);
  CHECK(v[20] == 1);
  CHECK(v.back() == 4);
}

TEST_CASE("small_vector: insert a range, random access and single pass") {
  const std::vector<int> src{7, 8};

  sv4 v{1, 4};
  auto it = v.insert(v.begin() + 1, src.begin(), src.end());
  CHECK(*it == 7);
  CHECK(to_vec(v) == std::vector<int>{1, 7, 8, 4});

  const auto before = to_vec(v);
  it = v.insert(v.begin() + 1, src.begin(), src.begin());
  CHECK(to_vec(v) == before);

  const std::list<int> lst{5, 6};
  sv4 w{1, 4};
  it = w.insert(w.begin() + 1, lst.begin(), lst.end());
  CHECK(*it == 5);
  CHECK(to_vec(w) == std::vector<int>{1, 5, 6, 4});

  const std::list<int> big{1, 2, 3, 4, 5, 6, 7, 8};
  sv4 x{0, 9};
  x.insert(x.begin() + 1, big.begin(), big.end());
  CHECK(to_vec(x) == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

  sv4 z{1, 4};
  z.insert(z.begin() + 1, {2, 3});
  CHECK(to_vec(z) == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE(
    "small_vector: insert shifts correctly on both shift_right branches") {
  sv4 few{1, 2};
  few.insert(few.begin(), 3, 9);
  CHECK(to_vec(few) == std::vector<int>{9, 9, 9, 1, 2});

  nx::small_vector<int, 16> many{1, 2, 3, 4, 5};
  many.insert(many.begin(), 2, 9);
  CHECK(std::vector<int>(many.begin(), many.end()) ==
        std::vector<int>{9, 9, 1, 2, 3, 4, 5});

  nx::small_vector<int, 16> eq{1, 2, 3};
  eq.insert(eq.begin(), 3, 0);
  CHECK(std::vector<int>(eq.begin(), eq.end()) ==
        std::vector<int>{0, 0, 0, 1, 2, 3});
}

TEST_CASE("small_vector: emplace constructs in place at a position") {
  nx::small_vector<std::string, 4> v{"a", "d"};
  const auto it = v.emplace(v.begin() + 1, 3, 'b');
  CHECK(*it == "bbb");
  CHECK(v.size() == 3);
  CHECK(v[2] == "d");
}

TEST_CASE("small_vector: erase one element and a range") {
  sv4 v{1, 2, 3, 4, 5};

  auto it = v.erase(v.begin() + 1);
  CHECK(*it == 3);
  CHECK(to_vec(v) == std::vector<int>{1, 3, 4, 5});

  it = v.erase(v.end() - 1);
  CHECK(it == v.end());
  CHECK(to_vec(v) == std::vector<int>{1, 3, 4});

  it = v.erase(v.begin(), v.begin() + 2);
  CHECK(*it == 4);
  CHECK(to_vec(v) == std::vector<int>{4});

  it = v.erase(v.begin(), v.begin());
  CHECK(to_vec(v) == std::vector<int>{4});
  CHECK(it == v.begin());

  it = v.erase(v.begin(), v.end());
  CHECK(v.empty());
  CHECK(it == v.begin());
}

TEST_CASE("small_vector: erase destroys exactly the removed elements") {
  g_live = 0;
  {
    nx::small_vector<Counted, 4> v;
    for (int i = 0; i < 10; ++i)
      v.emplace_back(i);
    CHECK(g_live == 10);

    v.erase(v.begin() + 2);
    CHECK(g_live == 9);
    CHECK(v[2].value == 3);

    v.erase(v.begin(), v.begin() + 3);
    CHECK(g_live == 6);
    CHECK(v[0].value == 4);
  }
  CHECK(g_live == 0);
}

TEST_CASE("small_vector: resize grows, shrinks and spills as needed") {
  sv4 v{1, 2, 3};

  v.resize(2);
  CHECK(to_vec(v) == std::vector<int>{1, 2});

  v.resize(4);
  CHECK(to_vec(v) == std::vector<int>{1, 2, 0, 0});
  CHECK(v.capacity() == 4);

  v.resize(10, 7);
  CHECK(v.size() == 10);
  CHECK(v.capacity() >= 10);
  CHECK(v[3] == 0);
  CHECK(v[4] == 7);
  CHECK(v[9] == 7);

  v.resize(0);
  CHECK(v.empty());

  sv4 w{1, 2};
  w.resize(2);
  CHECK(to_vec(w) == std::vector<int>{1, 2});
  w.resize(2, 9);
  CHECK(to_vec(w) == std::vector<int>{1, 2});

  g_live = 0;
  {
    nx::small_vector<Counted, 4> c(10);
    CHECK(g_live == 10);
    c.resize(3);
    CHECK(g_live == 3);
    c.resize(8);
    CHECK(g_live == 8);
  }
  CHECK(g_live == 0);
}

TEST_CASE("small_vector: copy construction and assignment are deep") {
  g_live = 0;
  {
    nx::small_vector<Counted, 4> a;
    for (int i = 0; i < 3; ++i)
      a.emplace_back(i);
    const nx::small_vector<Counted, 4> b = a;
    CHECK(b.size() == 3);
    CHECK(b[1].value == 1);
    CHECK(b.data() != a.data());
    CHECK(g_live == 6);

    nx::small_vector<Counted, 4> c;
    for (int i = 0; i < 20; ++i)
      c.emplace_back(i);
    const nx::small_vector<Counted, 4> d = c;
    CHECK(d.size() == 20);
    CHECK(d[19].value == 19);
    CHECK(d.data() != c.data());

    nx::small_vector<Counted, 4> e;
    e.emplace_back(99);
    e = c;
    CHECK(e.size() == 20);
    CHECK(e[0].value == 0);
  }
  CHECK(g_live == 0);
}

TEST_CASE("small_vector: move steals the heap buffer but copies inline data") {
  sv4 heap;
  for (int i = 0; i < 20; ++i)
    heap.push_back(i);
  const int *raw = heap.data();
  const sv4 moved = std::move(heap);
  CHECK(moved.data() == raw);
  CHECK(moved.size() == 20);
  CHECK(heap.empty());
  CHECK(heap.capacity() == 4);

  sv4 small{1, 2, 3};
  const sv4 moved_small = std::move(small);
  CHECK(moved_small.size() == 3);
  CHECK(to_vec(moved_small) == std::vector<int>{1, 2, 3});
  CHECK(small.empty());

  g_live = 0;
  {
    nx::small_vector<Counted, 4> a;
    for (int i = 0; i < 3; ++i)
      a.emplace_back(i);
    CHECK(g_live == 3);
    const nx::small_vector<Counted, 4> b = std::move(a);
    CHECK(b.size() == 3);
    CHECK(a.empty());
    CHECK(g_live == 3);
  }
  CHECK(g_live == 0);
}

TEST_CASE("small_vector: move assignment across inline and heap states") {
  g_live = 0;
  {
    nx::small_vector<Counted, 2> a, b;
    for (int i = 0; i < 10; ++i)
      a.emplace_back(i);
    for (int i = 0; i < 20; ++i)
      b.emplace_back(i + 100);
    a = std::move(b);
    CHECK(a.size() == 20);
    CHECK(a[0].value == 100);
    CHECK(b.empty());
    CHECK(g_live == 20);

    nx::small_vector<Counted, 2> c;
    c.emplace_back(1);
    a = std::move(c);
    CHECK(a.size() == 1);
    CHECK(a[0].value == 1);
    CHECK(c.empty());
    CHECK(g_live == 1);

    nx::small_vector<Counted, 2> d;
    for (int i = 0; i < 10; ++i)
      d.emplace_back(i);
    a = std::move(d);
    CHECK(a.size() == 10);
    CHECK(d.empty());
    CHECK(g_live == 10);
  }
  CHECK(g_live == 0);
}

TEST_CASE("small_vector: self-assignment leaves the contents intact") {
  sv4 v{1, 2, 3, 4, 5};
  const sv4 *alias = &v;
  v = *alias;
  CHECK(to_vec(v) == std::vector<int>{1, 2, 3, 4, 5});

  sv4 *malias = &v;
  v = std::move(*malias);
  CHECK(to_vec(v) == std::vector<int>{1, 2, 3, 4, 5});
}

TEST_CASE("small_vector: swap covers every inline/heap combination") {
  const auto make = [](int first, int count) {
    sv4 v;
    for (int i = 0; i < count; ++i)
      v.push_back(first + i);
    return v;
  };

  SUBCASE("inline <-> inline") {
    sv4 a = make(0, 2), b = make(100, 3);
    a.swap(b);
    CHECK(to_vec(a) == std::vector<int>{100, 101, 102});
    CHECK(to_vec(b) == std::vector<int>{0, 1});
  }
  SUBCASE("heap <-> heap") {
    sv4 a = make(0, 10), b = make(100, 20);
    a.swap(b);
    CHECK(a.size() == 20);
    CHECK(a[0] == 100);
    CHECK(b.size() == 10);
    CHECK(b[0] == 0);
  }
  SUBCASE("inline <-> heap") {
    sv4 a = make(0, 2), b = make(100, 20);
    a.swap(b);
    CHECK(a.size() == 20);
    CHECK(a[0] == 100);
    CHECK(to_vec(b) == std::vector<int>{0, 1});
  }
  SUBCASE("heap <-> inline") {
    sv4 a = make(0, 20), b = make(100, 2);
    a.swap(b);
    CHECK(to_vec(a) == std::vector<int>{100, 101});
    CHECK(b.size() == 20);
  }
  SUBCASE("empty and self") {
    sv4 a = make(0, 3), e;
    a.swap(e);
    CHECK(a.empty());
    CHECK(to_vec(e) == std::vector<int>{0, 1, 2});
    e.swap(e);
    CHECK(to_vec(e) == std::vector<int>{0, 1, 2});
    swap(a, e);
    CHECK(to_vec(a) == std::vector<int>{0, 1, 2});
  }
  SUBCASE("non-trivial elements are neither leaked nor duplicated") {
    g_live = 0;
    {
      nx::small_vector<Counted, 4> x, y;
      for (int i = 0; i < 10; ++i)
        x.emplace_back(i);
      y.emplace_back(99);
      CHECK(g_live == 11);
      x.swap(y);
      CHECK(x.size() == 1);
      CHECK(x[0].value == 99);
      CHECK(y.size() == 10);
      CHECK(g_live == 11);
    }
    CHECK(g_live == 0);
  }
}

TEST_CASE("small_vector: converts to spans that alias the storage") {
  sv4 v{1, 2, 3};
  const std::span<const int> cs = v;
  CHECK(cs.size() == 3);
  CHECK(cs.data() == v.data());

  const auto ms = static_cast<std::span<int>>(v);
  ms[0] = 99;
  CHECK(v[0] == 99);

  const sv4 e;
  const std::span<const int> es = e;
  CHECK(es.empty());
}

TEST_CASE("small_vector: equality and ordering") {
  const sv4 a{1, 2, 3};
  const sv4 b{1, 2, 3};
  const sv4 c{1, 2, 4};
  const sv4 shorter{1, 2};
  const sv4 e;

  CHECK(a == b);
  CHECK(a != c);
  CHECK(a != shorter);
  CHECK(a < c);
  CHECK(shorter < a);
  CHECK(e < shorter);
  CHECK(a <= b);
  CHECK((a <=> b) == std::strong_ordering::equal);

  sv4 inl{1, 2, 3};
  sv4 spilled{1, 2, 3};
  spilled.reserve(100);
  CHECK(inl == spilled);
}

TEST_CASE("small_vector: erase and erase_if free functions") {
  sv4 v{1, 2, 3, 2, 5};
  CHECK(nx::erase(v, 2) == 2);
  CHECK(to_vec(v) == std::vector<int>{1, 3, 5});
  CHECK(nx::erase(v, 42) == 0);

  CHECK(nx::erase_if(v, [](int x) { return x > 2; }) == 2);
  CHECK(to_vec(v) == std::vector<int>{1});
  CHECK(nx::erase_if(v, [](int) { return false; }) == 0);
  CHECK(nx::erase_if(v, [](int) { return true; }) == 1);
  CHECK(v.empty());
  CHECK(nx::erase(v, 1) == 0);
}

TEST_CASE("small_vector: holds move-only element types") {
  nx::small_vector<std::unique_ptr<int>, 2> v;
  v.push_back(std::make_unique<int>(1));
  v.push_back(std::make_unique<int>(2));
  v.push_back(std::make_unique<int>(3));
  REQUIRE(v.size() == 3);
  CHECK(*v[0] == 1);
  CHECK(*v[2] == 3);

  nx::small_vector<std::unique_ptr<int>, 2> moved = std::move(v);
  CHECK(moved.size() == 3);
  CHECK(*moved[1] == 2);
}

TEST_CASE("small_vector: destructor releases elements and the heap buffer") {
  g_live = 0;
  {
    nx::small_vector<Counted, 4> v;
    for (int i = 0; i < 500; ++i)
      v.emplace_back(i);
    CHECK(g_live == 500);
  }
  CHECK(g_live == 0);

  g_live = 0;
  {
    nx::small_vector<Counted, 4> v;
    v.emplace_back(1);
  }
  CHECK(g_live == 0);
}

TEST_CASE("small_vector: get_allocator is callable") {
  const sv4 v;
  CHECK(v.get_allocator() == v.get_allocator());
  CHECK(v.max_size() > 0);
}

TEST_CASE("small_vector: mirrors std::vector under a long mixed workload") {
  nx::small_vector<int, 8> v;
  std::vector<int> model;

  u64 state = 0xA5A5A5A5DEADBEEFull;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  for (int step = 0; step < 15000; ++step) {
    const u64 r = next();
    const int value = static_cast<int>(r >> 32);
    const usize pos =
        model.empty() ? 0 : static_cast<usize>(r >> 8) % model.size();

    switch (r % 7) {
    case 0:
    case 1:
    case 2:
      v.push_back(value);
      model.push_back(value);
      break;
    case 3:
      if (!model.empty()) {
        v.pop_back();
        model.pop_back();
      }
      break;
    case 4:
      v.insert(v.begin() + static_cast<isize>(pos), value);
      model.insert(model.begin() + static_cast<isize>(pos), value);
      break;
    case 5:
      if (!model.empty()) {
        v.erase(v.begin() + static_cast<isize>(pos));
        model.erase(model.begin() + static_cast<isize>(pos));
      }
      break;
    default: {
      const usize n = static_cast<usize>(r >> 16) % 40;
      v.resize(n);
      model.resize(n);
      break;
    }
    }

    REQUIRE(v.size() == model.size());
  }

  CHECK(std::vector<int>(v.begin(), v.end()) == model);
}
