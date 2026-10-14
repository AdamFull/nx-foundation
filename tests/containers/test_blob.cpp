#include "framework/nxtest.h"

#include "core/foundation/containers/blob.h"

#include <algorithm>
#include <list>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_live = 0;
int g_ctor_budget = -1;

struct Counted {
  int value;

  Counted() : value(0) { born(); }
  explicit Counted(int v) : value(v) { born(); }
  Counted(const Counted &o) : value(o.value) { born(); }
  Counted(Counted &&o) noexcept : value(o.value) {
    o.value = -1;
    ++g_live;
  }
  Counted &operator=(const Counted &) = default;
  Counted &operator=(Counted &&) = default;
  ~Counted() { --g_live; }

  bool operator==(const Counted &o) const { return value == o.value; }
  auto operator<=>(const Counted &o) const { return value <=> o.value; }

private:
  void born() {
    if (g_ctor_budget == 0)
      throw std::runtime_error("ctor budget exhausted");
    if (g_ctor_budget > 0)
      --g_ctor_budget;
    ++g_live;
  }
};

void reset_tracking() {
  g_live = 0;
  g_ctor_budget = -1;
}

}

TEST_CASE("blob: default-constructed is empty and owns nothing") {
  const nx::blob<int> b;
  CHECK(b.empty());
  CHECK(b.size() == 0);
  CHECK(b.bytes_size() == 0);
  CHECK(b.data() == nullptr);
  CHECK(b.begin() == b.end());
  CHECK(b.cbegin() == b.cend());
}

TEST_CASE("blob: sized construction value-initializes every element") {
  const nx::blob<int> b(10);
  CHECK(b.size() == 10);
  CHECK_FALSE(b.empty());
  CHECK(b.bytes_size() == 10 * sizeof(int));
  REQUIRE(b.data() != nullptr);
  for (const int v : b)
    CHECK(v == 0);

  const nx::blob<int> z(0);
  CHECK(z.empty());
  CHECK(z.data() == nullptr);
}

TEST_CASE("blob: fill construction copies the prototype into every slot") {
  const nx::blob<int> b(5, 42);
  REQUIRE(b.size() == 5);
  for (const int v : b)
    CHECK(v == 42);

  const nx::blob<std::string> s(3, std::string("abc"));
  REQUIRE(s.size() == 3);
  for (const std::string &v : s)
    CHECK(v == "abc");

  const nx::blob<int> z(0, 7);
  CHECK(z.empty());
}

TEST_CASE("blob: iterator-pair construction from a forward range") {
  const std::vector<int> src{1, 2, 3, 4, 5};
  const nx::blob<int> b(src.begin(), src.end());
  REQUIRE(b.size() == 5);
  for (usize i = 0; i < 5; ++i)
    CHECK(b[i] == src[i]);

  const nx::blob<int> e(src.begin(), src.begin());
  CHECK(e.empty());
}

TEST_CASE("blob: iterator-pair construction from a single-pass range") {
  const std::list<int> src{9, 8, 7, 6};
  const nx::blob<int> b(src.begin(), src.end());
  REQUIRE(b.size() == 4);
  CHECK(b[0] == 9);
  CHECK(b[1] == 8);
  CHECK(b[2] == 7);
  CHECK(b[3] == 6);

  const std::list<int> empty;
  const nx::blob<int> e(empty.begin(), empty.end());
  CHECK(e.empty());
}

TEST_CASE("blob: initializer-list and span construction") {
  const nx::blob<int> b{1, 2, 3};
  REQUIRE(b.size() == 3);
  CHECK(b[0] == 1);
  CHECK(b[2] == 3);

  const nx::blob<int> empty_il{};
  CHECK(empty_il.empty());

  const std::vector<int> src{4, 5, 6, 7};
  const nx::blob<int> from_span{std::span<const int>(src)};
  REQUIRE(from_span.size() == 4);
  CHECK(from_span[0] == 4);
  CHECK(from_span[3] == 7);
}

TEST_CASE("blob: element access through operator[], front, back and data") {
  nx::blob<int> b{10, 20, 30};

  CHECK(b[0] == 10);
  CHECK(b[2] == 30);
  b[1] = 99;
  CHECK(b[1] == 99);

  CHECK(b.front() == 10);
  CHECK(b.back() == 30);
  b.front() = 1;
  b.back() = 3;
  CHECK(b[0] == 1);
  CHECK(b[2] == 3);

  CHECK(b.data()[1] == 99);
  CHECK(&b.front() == b.data());
  CHECK(&b.back() == b.data() + 2);

  const nx::blob<int> &cb = b;
  CHECK(cb[0] == 1);
  CHECK(cb.front() == 1);
  CHECK(cb.back() == 3);
  CHECK(cb.data() == b.data());
}

TEST_CASE("blob: at() is bounds-checked and throws out_of_range") {
  nx::blob<int> b{1, 2, 3};

  CHECK(b.at(0) == 1);
  CHECK(b.at(2) == 3);
  b.at(1) = 7;
  CHECK(b[1] == 7);

  CHECK_THROWS_AS(b.at(3), std::out_of_range);
  CHECK_THROWS_AS(b.at(usize(-1)), std::out_of_range);

  const nx::blob<int> &cb = b;
  CHECK(cb.at(0) == 1);
  CHECK_THROWS_AS(cb.at(3), std::out_of_range);

  nx::blob<int> e;
  CHECK_THROWS_AS(e.at(0), std::out_of_range);
}

TEST_CASE("blob: forward and reverse iteration cover the whole range") {
  nx::blob<int> b{1, 2, 3, 4};

  CHECK(std::distance(b.begin(), b.end()) == 4);
  CHECK(std::accumulate(b.begin(), b.end(), 0) == 10);

  std::vector<int> forward(b.begin(), b.end());
  CHECK(forward == std::vector<int>{1, 2, 3, 4});

  std::vector<int> reverse(b.rbegin(), b.rend());
  CHECK(reverse == std::vector<int>{4, 3, 2, 1});

  const nx::blob<int> &cb = b;
  std::vector<int> cforward(cb.begin(), cb.end());
  CHECK(cforward == forward);
  std::vector<int> creverse(cb.rbegin(), cb.rend());
  CHECK(creverse == reverse);
  std::vector<int> crev2(b.crbegin(), b.crend());
  CHECK(crev2 == reverse);

  for (int &v : b)
    v *= 2;
  CHECK(b[0] == 2);
  CHECK(b[3] == 8);

  nx::blob<int> e;
  int hits = 0;
  for (const int v : e) {
    (void)v;
    ++hits;
  }
  CHECK(hits == 0);
  CHECK(e.rbegin() == e.rend());
}

TEST_CASE("blob: copy construction and assignment deep-copy the elements") {
  reset_tracking();
  {
    const nx::blob<Counted> a(4, Counted{7});
    CHECK(g_live == 4);

    const nx::blob<Counted> b = a;
    REQUIRE(b.size() == 4);
    CHECK(b[0].value == 7);
    CHECK(b.data() != a.data());

    nx::blob<Counted> c;
    c = a;
    REQUIRE(c.size() == 4);
    CHECK(c[3].value == 7);
    CHECK(c.data() != a.data());

    c = nx::blob<Counted>(2, Counted{1});
    CHECK(c.size() == 2);
    CHECK(c[0].value == 1);
  }
  CHECK(g_live == 0);
}

TEST_CASE("blob: move construction and assignment steal the buffer") {
  reset_tracking();
  {
    nx::blob<Counted> a(4, Counted{5});
    const Counted *raw = a.data();

    const nx::blob<Counted> b = std::move(a);
    CHECK(b.data() == raw);
    CHECK(b.size() == 4);
    CHECK(a.empty());
    CHECK(a.data() == nullptr);

    nx::blob<Counted> c(2, Counted{9});
    nx::blob<Counted> d(3, Counted{1});
    const Counted *draw = d.data();
    c = std::move(d);
    CHECK(c.data() == draw);
    CHECK(c.size() == 3);
    CHECK(d.empty());
  }
  CHECK(g_live == 0);
}

TEST_CASE("blob: self-assignment is a no-op") {
  nx::blob<int> b{1, 2, 3};
  const int *raw = b.data();

  const nx::blob<int> *alias = &b;
  b = *alias;
  REQUIRE(b.size() == 3);
  CHECK(b.data() == raw);
  CHECK(b[1] == 2);

  nx::blob<int> *malias = &b;
  b = std::move(*malias);
  REQUIRE(b.size() == 3);
  CHECK(b[1] == 2);
}

TEST_CASE("blob: the destructor runs every element destructor") {
  reset_tracking();
  {
    const nx::blob<Counted> b(100);
    CHECK(g_live == 100);
  }
  CHECK(g_live == 0);

  reset_tracking();
  {
    nx::blob<Counted> a(10);
    nx::blob<Counted> b = std::move(a);
    CHECK(g_live == 10);
  }
  CHECK(g_live == 0);
}

TEST_CASE("blob: a throwing element constructor leaves nothing allocated") {
#if NXTEST_ASAN_UNWIND_BROKEN
  SKIP("ASan on Windows faults in the unwinder here; see "
       "NXTEST_ASAN_UNWIND_BROKEN");
#else
  reset_tracking();
  g_ctor_budget = 3;
  CHECK_THROWS_AS(nx::blob<Counted>(10), std::runtime_error);
  CHECK(g_live == 0);

  reset_tracking();
  {
    const Counted prototype{1};
    CHECK(g_live == 1);
    g_ctor_budget = 3;
    CHECK_THROWS_AS(nx::blob<Counted>(10, prototype), std::runtime_error);
    g_ctor_budget = -1;
    CHECK(g_live == 1);
  }
  CHECK(g_live == 0);

  reset_tracking();
  {
    const std::vector<Counted> src(10);
    CHECK(g_live == 10);
    g_ctor_budget = 3;
    CHECK_THROWS_AS(nx::blob<Counted>(src.begin(), src.end()),
                    std::runtime_error);
    g_ctor_budget = -1;
    CHECK(g_live == 10);
  }
  CHECK(g_live == 0);
#endif
}

TEST_CASE("blob: a throwing copy assignment does not corrupt the destination") {
#if NXTEST_ASAN_UNWIND_BROKEN
  SKIP("ASan on Windows faults in the unwinder here; see "
       "NXTEST_ASAN_UNWIND_BROKEN");
#else
  reset_tracking();
  {
    nx::blob<Counted> dst(2, Counted{1});
    const nx::blob<Counted> src(5, Counted{2});
    const int before = g_live;

    g_ctor_budget = 2;
    CHECK_THROWS_AS(dst = src, std::runtime_error);
    g_ctor_budget = -1;

    CHECK(g_live == before - 2);
    CHECK(dst.empty());
  }
  CHECK(g_live == 0);
#endif
}

TEST_CASE("blob: swap exchanges buffers without copying") {
  nx::blob<int> a{1, 2, 3};
  nx::blob<int> b{9, 8};
  const int *araw = a.data();
  const int *braw = b.data();

  a.swap(b);
  CHECK(a.size() == 2);
  CHECK(b.size() == 3);
  CHECK(a.data() == braw);
  CHECK(b.data() == araw);
  CHECK(a[0] == 9);
  CHECK(b[0] == 1);

  swap(a, b);
  CHECK(a.size() == 3);
  CHECK(a[0] == 1);

  nx::blob<int> e;
  a.swap(e);
  CHECK(a.empty());
  CHECK(e.size() == 3);
  CHECK(e[2] == 3);
}

TEST_CASE("blob: release and from_owned transfer ownership") {
  reset_tracking();
  {
    nx::blob<Counted> a(4, Counted{3});
    const Counted *raw = a.data();

    const auto owned = a.release();
    CHECK(owned.data == raw);
    CHECK(owned.size == 4);
    CHECK(a.empty());
    CHECK(a.data() == nullptr);

    const nx::blob<Counted> b =
        nx::blob<Counted>::from_owned(owned.data, owned.size);
    CHECK(b.size() == 4);
    CHECK(b.data() == raw);
    CHECK(b[0].value == 3);
  }
  CHECK(g_live == 0);

  nx::blob<int> e;
  const auto owned = e.release();
  CHECK(owned.data == nullptr);
  CHECK(owned.size == 0);
}

TEST_CASE("blob: to_vector produces an equal, independent vector") {
  const nx::blob<int> b{1, 2, 3, 4};
  const nx::vector<int> v = b.to_vector();
  REQUIRE(v.size() == 4);
  for (usize i = 0; i < 4; ++i)
    CHECK(v[i] == b[i]);

  const nx::blob<int> e;
  CHECK(e.to_vector().empty());
}

TEST_CASE("blob: converts to spans that alias the storage") {
  nx::blob<int> b{1, 2, 3};

  const std::span<const int> cs = b;
  REQUIRE(cs.size() == 3);
  CHECK(cs.data() == b.data());
  CHECK(cs[1] == 2);

  const auto ms = static_cast<std::span<int>>(b);
  REQUIRE(ms.size() == 3);
  ms[0] = 99;
  CHECK(b[0] == 99);

  const nx::blob<int> e;
  const std::span<const int> es = e;
  CHECK(es.empty());
}

TEST_CASE("blob: equality compares size then contents") {
  const nx::blob<int> a{1, 2, 3};
  const nx::blob<int> b{1, 2, 3};
  const nx::blob<int> c{1, 2, 4};
  const nx::blob<int> d{1, 2};
  const nx::blob<int> e;
  const nx::blob<int> f;

  CHECK(a == b);
  CHECK_FALSE(a == c);
  CHECK(a != c);
  CHECK_FALSE(a == d);
  CHECK(a != d);
  CHECK(e == f);
  CHECK(a != e);
}

TEST_CASE("blob: three-way comparison is lexicographic") {
  const nx::blob<int> a{1, 2, 3};
  const nx::blob<int> b{1, 2, 4};
  const nx::blob<int> shorter{1, 2};
  const nx::blob<int> empty;

  CHECK(a < b);
  CHECK(b > a);
  CHECK(a <= a);
  CHECK(a >= a);
  CHECK((a <=> a) == std::strong_ordering::equal);

  CHECK(shorter < a);
  CHECK(empty < shorter);
  CHECK_FALSE(empty < empty);
}

TEST_CASE("blob: holds non-trivial and move-only-ish element types") {
  nx::blob<std::string> s(3);
  CHECK(s[0].empty());
  s[0] = "hello";
  s[1] = "world";
  CHECK(s[0] == "hello");
  CHECK(s.back().empty());

  const nx::blob<std::string> copy = s;
  CHECK(copy[0] == "hello");
  CHECK(copy.data() != s.data());
  s[0] = "changed";
  CHECK(copy[0] == "hello");

  nx::blob<nx::blob<int>> nested(2);
  nested[0] = nx::blob<int>{1, 2};
  nested[1] = nx::blob<int>{3};
  CHECK(nested[0].size() == 2);
  CHECK(nested[1][0] == 3);
}

TEST_CASE("blob: a large allocation is contiguous and fully writable") {
  constexpr usize n = 100000;
  nx::blob<u32> b(n);
  REQUIRE(b.size() == n);

  for (usize i = 0; i < n; ++i)
    b[i] = static_cast<u32>(i);
  for (usize i = 0; i < n; ++i)
    CHECK_FALSE(b[i] != static_cast<u32>(i));

  CHECK(&b[n - 1] - &b[0] == static_cast<isize>(n) - 1);
  CHECK(b.bytes_size() == n * sizeof(u32));
}

TEST_CASE("blob: get_allocator returns an equal allocator") {
  const nx::blob<int> b(4);
  const auto a1 = b.get_allocator();
  const auto a2 = b.get_allocator();
  CHECK(a1 == a2);
}
