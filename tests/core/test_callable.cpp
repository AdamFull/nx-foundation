#include "framework/nxtest.h"

#include "core/foundation/core/callable.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

int free_add(int a, int b) { return a + b; }
void free_void() {}

int g_live = 0;

struct Tracked {
  int value;
  explicit Tracked(int v) : value(v) { ++g_live; }
  Tracked(const Tracked &o) : value(o.value) { ++g_live; }
  Tracked(Tracked &&o) noexcept : value(o.value) {
    o.value = -1;
    ++g_live;
  }
  ~Tracked() { --g_live; }
  int operator()(int x) const { return value + x; }
};

struct BigFunctor {
  char pad[128]{};
  int base = 5;
  int operator()(int x) const { return base + x; }
};

}

TEST_CASE("inplace_function: default-constructed is empty and not callable") {
  const nx::inplace_function<int(int), 32> f;
  CHECK_FALSE(f.valid());
  CHECK_FALSE(static_cast<bool>(f));
}

TEST_CASE("unique_function: owns and moves a small callable inline") {
  static_assert(!std::is_copy_constructible_v<nx::unique_function<int(int)>>);
  static_assert(
      std::is_nothrow_move_constructible_v<nx::unique_function<int(int)>>);
  static_assert(sizeof(nx::unique_function<void()>) == 32);

  int base = 7;
  nx::unique_function<int(int)> source{
      [base](const int x) { return base + x; }};
  REQUIRE(static_cast<bool>(source));
  CHECK(source(2) == 9);

  nx::unique_function<int(int)> moved{std::move(source)};
  CHECK_FALSE(static_cast<bool>(source));
  REQUIRE(static_cast<bool>(moved));
  CHECK(moved(5) == 12);
}

TEST_CASE("unique_function: accepts move-only captures") {
  nx::unique_function<int()> fn{
      [value = std::make_unique<int>(41)] { return *value + 1; }};
  REQUIRE(static_cast<bool>(fn));
  CHECK(fn() == 42);
}

TEST_CASE("unique_function: a small target requires no allocation") {
  int calls = 0;
  {
    const nx::scoped_no_alloc no_alloc;
    nx::unique_function<void()> fn{[&calls] { ++calls; }};
    fn();
  }
  CHECK(calls == 1);
}

TEST_CASE("unique_function: falls back to owned storage for large callables") {
  g_live = 0;
  {
    nx::unique_function<int(int)> fn{BigFunctor{}};
    REQUIRE(static_cast<bool>(fn));
    CHECK(fn(4) == 9);

    nx::unique_function<int(int)> moved;
    moved = std::move(fn);
    CHECK_FALSE(static_cast<bool>(fn));
    REQUIRE(static_cast<bool>(moved));
    CHECK(moved(6) == 11);
  }
}

TEST_CASE("unique_function: destroys its target exactly once") {
  g_live = 0;
  {
    nx::unique_function<int(int)> fn{Tracked{10}};
    CHECK(g_live == 1);
    nx::unique_function<int(int)> moved{std::move(fn)};
    CHECK(g_live == 1);
    CHECK(moved(1) == 11);
    moved.reset();
    CHECK(g_live == 0);
  }
  CHECK(g_live == 0);
}

TEST_CASE("inplace_function: invokes a captureless lambda") {
  const nx::inplace_function<int(int), 32> f{[](int x) { return x * 2; }};
  CHECK(f.valid());
  CHECK(static_cast<bool>(f));
  CHECK(f(21) == 42);
  CHECK(f(0) == 0);
  CHECK(f(-3) == -6);
}

TEST_CASE("inplace_function: invokes a capturing lambda") {
  int base = 100;
  const nx::inplace_function<int(int), 32> f{
      [base](int x) { return base + x; }};
  CHECK(f(5) == 105);

  base = 0;
  CHECK(f(5) == 105);
}

TEST_CASE("inplace_function: captures by reference and observes later writes") {
  int counter = 0;
  const nx::inplace_function<void(), 32> f{[&counter] { ++counter; }};
  f();
  f();
  f();
  CHECK(counter == 3);
}

TEST_CASE("inplace_function: supports void returns and multiple arguments") {
  int sum = 0;
  const nx::inplace_function<void(int, int), 32> add{
      [&sum](int a, int b) { sum = a + b; }};
  add(3, 4);
  CHECK(sum == 7);

  const nx::inplace_function<int(int, int, int), 32> tri{
      [](int a, int b, int c) { return a * b * c; }};
  CHECK(tri(2, 3, 4) == 24);

  const nx::inplace_function<void(), 32> nothing{[] {}};
  CHECK_NOTHROW(nothing());
}

TEST_CASE("inplace_function: holds a function pointer") {
  const nx::inplace_function<int(int, int), 32> f{&free_add};
  CHECK(f(2, 3) == 5);

  const nx::inplace_function<void(), 32> v{&free_void};
  CHECK_NOTHROW(v());
}

TEST_CASE("inplace_function: forwards reference and move-only arguments") {
  const nx::inplace_function<void(int &), 32> bump{[](int &x) { ++x; }};
  int n = 1;
  bump(n);
  CHECK(n == 2);

  const nx::inplace_function<int(std::unique_ptr<int>), 64> consume{
      [](std::unique_ptr<int> p) { return *p; }};
  CHECK(consume(std::make_unique<int>(9)) == 9);

  const nx::inplace_function<const std::string *(const std::string &), 32> addr{
      [](const std::string &s) { return &s; }};
  const std::string s = "hello";
  CHECK(addr(s) == &s);
}

TEST_CASE(
    "inplace_function: copy construction produces an independent callable") {
  g_live = 0;
  {
    const nx::inplace_function<int(int), 64> a{Tracked{10}};
    CHECK(a(1) == 11);

    const nx::inplace_function<int(int), 64> b{a};
    CHECK(b.valid());
    CHECK(b(1) == 11);
    CHECK(a(1) == 11);
    CHECK(g_live == 2);
  }
  CHECK(g_live == 0);
}

TEST_CASE("inplace_function: move construction empties the source") {
  g_live = 0;
  {
    nx::inplace_function<int(int), 64> a{Tracked{10}};
    CHECK(g_live == 1);

    const nx::inplace_function<int(int), 64> b{std::move(a)};
    CHECK(b.valid());
    CHECK(b(2) == 12);
    CHECK_FALSE(a.valid());
    CHECK(g_live == 1);
  }
  CHECK(g_live == 0);
}

TEST_CASE("inplace_function: copy assignment replaces the previous target") {
  g_live = 0;
  {
    nx::inplace_function<int(int), 64> a{Tracked{1}};
    const nx::inplace_function<int(int), 64> b{Tracked{2}};
    CHECK(g_live == 2);

    a = b;
    CHECK(a(0) == 2);
    CHECK(b(0) == 2);
    CHECK(g_live == 2);
  }
  CHECK(g_live == 0);
}

TEST_CASE("inplace_function: move assignment replaces and empties the source") {
  g_live = 0;
  {
    nx::inplace_function<int(int), 64> a{Tracked{1}};
    nx::inplace_function<int(int), 64> b{Tracked{2}};
    CHECK(g_live == 2);

    a = std::move(b);
    CHECK(a(0) == 2);
    CHECK_FALSE(b.valid());
    CHECK(g_live == 1);
  }
  CHECK(g_live == 0);
}

TEST_CASE("inplace_function: self-assignment leaves the callable intact") {
  nx::inplace_function<int(int), 64> a{[](int x) { return x + 1; }};
  const nx::inplace_function<int(int), 64> *alias = &a;
  a = *alias;
  REQUIRE(a.valid());
  CHECK(a(1) == 2);

  nx::inplace_function<int(int), 64> *malias = &a;
  a = std::move(*malias);
  REQUIRE(a.valid());
  CHECK(a(1) == 2);
}

TEST_CASE(
    "inplace_function: assigning from an empty source clears the target") {
  g_live = 0;
  {
    nx::inplace_function<int(int), 64> a{Tracked{1}};
    const nx::inplace_function<int(int), 64> empty;
    CHECK(g_live == 1);

    a = empty;
    CHECK_FALSE(a.valid());
    CHECK(g_live == 0);
  }
  CHECK(g_live == 0);

  g_live = 0;
  {
    nx::inplace_function<int(int), 64> a{Tracked{1}};
    nx::inplace_function<int(int), 64> empty;
    a = std::move(empty);
    CHECK_FALSE(a.valid());
    CHECK(g_live == 0);
  }
  CHECK(g_live == 0);
}

TEST_CASE(
    "inplace_function: copying and moving an empty function stays empty") {
  const nx::inplace_function<int(int), 32> empty;
  const nx::inplace_function<int(int), 32> copy{empty};
  CHECK_FALSE(copy.valid());

  nx::inplace_function<int(int), 32> src;
  const nx::inplace_function<int(int), 32> moved{std::move(src)};
  CHECK_FALSE(moved.valid());
  CHECK_FALSE(src.valid());
}

TEST_CASE("inplace_function: destroys the stored target exactly once") {
  g_live = 0;
  {
    const nx::inplace_function<int(int), 64> f{Tracked{3}};
    CHECK(g_live == 1);
  }
  CHECK(g_live == 0);

  g_live = 0;
  {
    nx::inplace_function<int(int), 64> f{Tracked{1}};
    for (int i = 0; i < 20; ++i) {
      f = nx::inplace_function<int(int), 64>{Tracked{i}};
      CHECK(g_live == 1);
    }
  }
  CHECK(g_live == 0);
}

TEST_CASE("inplace_function: buffer_size reflects the template argument") {
  CHECK((nx::inplace_function<void(), 32>::buffer_size) == 32);
  CHECK((nx::inplace_function<void(), 128>::buffer_size) == 128);
  CHECK(sizeof(nx::inplace_function<void(), 128>) >= 128);
}

TEST_CASE(
    "inplace_function: make_inplace_function deduces signature and size") {
  auto f = nx::make_inplace_function([](int x) { return x * 3; });
  CHECK(f.valid());
  CHECK(f(4) == 12);

  int captured = 7;
  auto g =
      nx::make_inplace_function([captured](int x) { return captured + x; });
  CHECK(g(1) == 8);

  auto big = nx::make_inplace_function(BigFunctor{});
  CHECK(big(1) == 6);
  CHECK(decltype(big)::buffer_size >= sizeof(BigFunctor));

  auto sized = nx::make_inplace_function<256>([](int x) { return x; });
  CHECK(decltype(sized)::buffer_size == 256);
  CHECK(sized(5) == 5);

  auto fp = nx::make_inplace_function(&free_add);
  CHECK(fp(1, 2) == 3);
  auto fp_sized = nx::make_inplace_function<64>(&free_add);
  CHECK(fp_sized(4, 5) == 9);
}

TEST_CASE("inplace_function: class template argument deduction") {
  const nx::inplace_function f{[](int x) { return x + 1; }};
  CHECK(f(1) == 2);

  const nx::inplace_function g{&free_add};
  CHECK(g(3, 4) == 7);
}

TEST_CASE("inplace_function: stores a trivially copyable functor via memcpy") {
  struct Trivial {
    int a, b;
    int operator()(int x) const { return a * x + b; }
  };
  nx::inplace_function<int(int), 32> f{Trivial{2, 3}};
  CHECK(f(4) == 11);

  nx::inplace_function<int(int), 32> copy{f};
  CHECK(copy(4) == 11);

  nx::inplace_function<int(int), 32> moved{std::move(f)};
  CHECK(moved(4) == 11);
  CHECK_FALSE(f.valid());

  nx::inplace_function<int(int), 32> assigned;
  assigned = copy;
  CHECK(assigned(4) == 11);
}

TEST_CASE("inplace_function: destroys a moved-from target that needs it") {
  struct Recorder {
    static int &dtors() {
      static int n = 0;
      return n;
    }
    int value;
    explicit Recorder(int v) : value(v) {}
    Recorder(const Recorder &) = default;
    Recorder(Recorder &&) = default;
    ~Recorder() { ++dtors(); }
    int operator()(int x) const { return value + x; }
  };

  CHECK_FALSE(std::is_trivially_copyable_v<Recorder>);

  Recorder::dtors() = 0;
  {
    nx::inplace_function<int(int), 64> a{Recorder{5}};
    const int after_construct = Recorder::dtors();

    nx::inplace_function<int(int), 64> b{std::move(a)};
    CHECK(b(1) == 6);
    CHECK_FALSE(a.valid());
    CHECK(Recorder::dtors() == after_construct + 1);

    const int before_copy = Recorder::dtors();
    nx::inplace_function<int(int), 64> c{b};
    CHECK(c(1) == 6);
    CHECK(Recorder::dtors() == before_copy);
  }

  Recorder::dtors() = 0;
  {
    nx::inplace_function<int(int), 64> f{Recorder{1}};
    const int base = Recorder::dtors();
    for (int i = 0; i < 50; ++i) {
      nx::inplace_function<int(int), 64> tmp{std::move(f)};
      f = std::move(tmp);
      REQUIRE(Recorder::dtors() == base + 2 * (i + 1));
    }
    CHECK(f(0) == 1);
  }
}

TEST_CASE("inplace_function: propagates an exception out of the target") {
  const nx::inplace_function<int(), 32> thrower{
      []() -> int { throw std::string("boom"); }};
  CHECK_THROWS_AS(thrower(), std::string);
}

TEST_CASE("inplace_function: survives being stored in a vector") {
  std::vector<nx::inplace_function<int(int), 64>> fns;
  for (int i = 0; i < 50; ++i)
    fns.emplace_back([i](int x) { return i * x; });

  REQUIRE(fns.size() == 50);
  for (int i = 0; i < 50; ++i)
    CHECK(fns[static_cast<usize>(i)](2) == i * 2);

  fns.reserve(500);
  for (int i = 0; i < 50; ++i)
    CHECK(fns[static_cast<usize>(i)](3) == i * 3);
}

TEST_CASE("function: default-constructed is empty") {
  const nx::function<int(int)> f;
  CHECK_FALSE(f.valid());
  CHECK_FALSE(static_cast<bool>(f));
}

TEST_CASE("function: invokes lambdas, captures and function pointers") {
  const nx::function<int(int)> a{[](int x) { return x * 2; }};
  CHECK(a.valid());
  CHECK(a(21) == 42);

  int base = 10;
  const nx::function<int(int)> b{[base](int x) { return base + x; }};
  CHECK(b(5) == 15);

  const nx::function<int(int, int)> c{&free_add};
  CHECK(c(2, 3) == 5);

  int side = 0;
  const nx::function<void()> d{[&side] { side = 1; }};
  d();
  CHECK(side == 1);
}

TEST_CASE("function: holds a functor far larger than any inline buffer") {
  struct Huge {
    char pad[4096]{};
    int tag = 77;
    int operator()() const { return tag; }
  };
  const nx::function<int()> f{Huge{}};
  CHECK(f() == 77);
  CHECK(sizeof(nx::function<int()>) < 128);
}

TEST_CASE("function: copy construction deep-copies the target") {
  g_live = 0;
  {
    const nx::function<int(int)> a{Tracked{10}};
    CHECK(g_live == 1);

    const nx::function<int(int)> b{a};
    CHECK(b.valid());
    CHECK(b(1) == 11);
    CHECK(a(1) == 11);
    CHECK(g_live == 2);
  }
  CHECK(g_live == 0);
}

TEST_CASE("function: move construction empties the source") {
  g_live = 0;
  {
    nx::function<int(int)> a{Tracked{10}};
    const nx::function<int(int)> b{std::move(a)};
    CHECK(b.valid());
    CHECK(b(2) == 12);
    CHECK_FALSE(a.valid());
    CHECK(g_live == 1);
  }
  CHECK(g_live == 0);
}

TEST_CASE("function: copy assignment releases the old target") {
  g_live = 0;
  {
    nx::function<int(int)> a{Tracked{1}};
    const nx::function<int(int)> b{Tracked{2}};
    CHECK(g_live == 2);

    a = b;
    CHECK(a(0) == 2);
    CHECK(b(0) == 2);
    CHECK(g_live == 2);
  }
  CHECK(g_live == 0);
}

TEST_CASE("function: move assignment releases the old target and empties the "
          "source") {
  g_live = 0;
  {
    nx::function<int(int)> a{Tracked{1}};
    nx::function<int(int)> b{Tracked{2}};
    CHECK(g_live == 2);

    a = std::move(b);
    CHECK(a(0) == 2);
    CHECK_FALSE(b.valid());
    CHECK(g_live == 1);
  }
  CHECK(g_live == 0);
}

TEST_CASE("function: assigning an empty function releases the old target") {
  g_live = 0;
  {
    nx::function<int(int)> a{Tracked{1}};
    const nx::function<int(int)> empty;
    a = empty;
    CHECK_FALSE(a.valid());
    CHECK(g_live == 0);
  }
  CHECK(g_live == 0);

  g_live = 0;
  {
    nx::function<int(int)> a{Tracked{1}};
    nx::function<int(int)> empty;
    a = std::move(empty);
    CHECK_FALSE(a.valid());
    CHECK(g_live == 0);
  }
  CHECK(g_live == 0);
}

TEST_CASE("function: self-assignment leaves the callable intact") {
  nx::function<int(int)> a{[](int x) { return x + 1; }};
  const nx::function<int(int)> *alias = &a;
  a = *alias;
  REQUIRE(a.valid());
  CHECK(a(1) == 2);

  nx::function<int(int)> *malias = &a;
  a = std::move(*malias);
  REQUIRE(a.valid());
  CHECK(a(1) == 2);
}

TEST_CASE("function: copying and moving an empty function stays empty") {
  const nx::function<int(int)> empty;
  const nx::function<int(int)> copy{empty};
  CHECK_FALSE(copy.valid());

  nx::function<int(int)> src;
  const nx::function<int(int)> moved{std::move(src)};
  CHECK_FALSE(moved.valid());
  CHECK_FALSE(src.valid());
}

TEST_CASE("function: repeated reassignment neither leaks nor double-frees") {
  g_live = 0;
  {
    nx::function<int(int)> f{Tracked{0}};
    for (int i = 1; i <= 100; ++i) {
      f = nx::function<int(int)>{Tracked{i}};
      CHECK(g_live == 1);
      CHECK(f(0) == i);
    }
  }
  CHECK(g_live == 0);
}

TEST_CASE("function: make_function deduces the signature") {
  auto f = nx::make_function([](int x) { return x * 3; });
  static_assert(std::is_same_v<decltype(f), nx::function<int(int)>>);
  CHECK(f(4) == 12);

  auto g = nx::make_function([](int a, double b) { return a + b; });
  static_assert(std::is_same_v<decltype(g), nx::function<double(int, double)>>);
  CHECK(g(1, 0.5) == 1.5);

  auto fp = nx::make_function(&free_add);
  static_assert(std::is_same_v<decltype(fp), nx::function<int(int, int)>>);
  CHECK(fp(1, 2) == 3);

  auto v = nx::make_function([] {});
  static_assert(std::is_same_v<decltype(v), nx::function<void()>>);
  CHECK_NOTHROW(v());
}

TEST_CASE("function: forwards move-only and reference arguments") {
  const nx::function<void(int &)> bump{[](int &x) { ++x; }};
  int n = 1;
  bump(n);
  CHECK(n == 2);

  const nx::function<int(std::unique_ptr<int>)> consume{
      [](std::unique_ptr<int> p) { return *p; }};
  CHECK(consume(std::make_unique<int>(9)) == 9);
}

TEST_CASE("function: propagates an exception out of the target") {
  const nx::function<int()> thrower{[]() -> int { throw std::string("boom"); }};
  CHECK_THROWS_AS(thrower(), std::string);
}

TEST_CASE("function: survives being stored and reallocated in a vector") {
  std::vector<nx::function<int(int)>> fns;
  for (int i = 0; i < 100; ++i)
    fns.emplace_back([i](int x) { return i + x; });

  REQUIRE(fns.size() == 100);
  fns.reserve(1000);
  for (int i = 0; i < 100; ++i)
    CHECK(fns[static_cast<usize>(i)](1) == i + 1);

  const std::vector<nx::function<int(int)>> copy = fns;
  for (int i = 0; i < 100; ++i)
    CHECK(copy[static_cast<usize>(i)](2) == i + 2);
}
