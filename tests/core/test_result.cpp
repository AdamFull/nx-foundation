#include "framework/nxtest.h"

#include "core/foundation/core/result.h"

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace {

enum class Error { None, NotFound, Denied };

int g_ctors = 0;
int g_dtors = 0;
int g_copies = 0;
int g_moves = 0;

void reset_counts() { g_ctors = g_dtors = g_copies = g_moves = 0; }

struct Payload {
  int value;
  explicit Payload(int v = 0) noexcept : value(v) { ++g_ctors; }
  Payload(const Payload &o) : value(o.value) {
    ++g_ctors;
    ++g_copies;
  }
  Payload(Payload &&o) noexcept : value(o.value) {
    o.value = -1;
    ++g_ctors;
    ++g_moves;
  }
  Payload &operator=(const Payload &) = default;
  Payload &operator=(Payload &&) = default;
  ~Payload() { ++g_dtors; }
};

struct MoveOnly {
  int value;
  explicit MoveOnly(int v) : value(v) {}
  MoveOnly(const MoveOnly &) = delete;
  MoveOnly &operator=(const MoveOnly &) = delete;
  MoveOnly(MoveOnly &&o) noexcept : value(o.value) { o.value = -1; }
  MoveOnly &operator=(MoveOnly &&o) noexcept {
    value = o.value;
    o.value = -1;
    return *this;
  }
};

struct ThrowOnCopy {
  static inline bool enabled = false;
  int value = 0;

  explicit ThrowOnCopy(const int v = 0) : value(v) {}
  ThrowOnCopy(const ThrowOnCopy &other) : value(other.value) {
    if (enabled)
      throw std::runtime_error("copy refused");
  }
  ThrowOnCopy(ThrowOnCopy &&) noexcept = default;
  ThrowOnCopy &operator=(const ThrowOnCopy &other) {
    if (enabled)
      throw std::runtime_error("copy refused");
    value = other.value;
    return *this;
  }
  ThrowOnCopy &operator=(ThrowOnCopy &&) noexcept = default;
};

struct ThrowingMove {
  ThrowingMove() = default;
  ThrowingMove(const ThrowingMove &) = default;
  ThrowingMove &operator=(const ThrowingMove &) = default;
  ThrowingMove(ThrowingMove &&) noexcept(false) {}
  ThrowingMove &operator=(ThrowingMove &&) noexcept(false) { return *this; }
};

}

TEST_CASE("result: special-member traits match the alternatives") {
  using MoveOnlyResult = nx::result<MoveOnly, Error>;
  static_assert(!std::is_copy_constructible_v<MoveOnlyResult>);
  static_assert(!std::is_copy_assignable_v<MoveOnlyResult>);
  static_assert(std::is_nothrow_move_constructible_v<MoveOnlyResult>);
  static_assert(std::is_nothrow_move_assignable_v<MoveOnlyResult>);

  using ThrowingMoveResult = nx::result<ThrowingMove, Error>;
  static_assert(std::is_move_constructible_v<ThrowingMoveResult>);
  static_assert(!std::is_nothrow_move_constructible_v<ThrowingMoveResult>);
  static_assert(!std::is_move_assignable_v<ThrowingMoveResult>);

  using VoidMoveOnly = nx::result<void, MoveOnly>;
  static_assert(!std::is_copy_constructible_v<VoidMoveOnly>);
  static_assert(std::is_nothrow_move_constructible_v<VoidMoveOnly>);
  CHECK(true);
}

TEST_CASE("result: err wrapper carries the error by value and by move") {
  const nx::err e{Error::NotFound};
  CHECK(e.error() == Error::NotFound);

  nx::err moved{std::string("boom")};
  CHECK(std::move(moved).error() == "boom");

  static_assert(std::is_same_v<decltype(nx::err{1}), nx::err<int>>);
  static_assert(
      std::is_same_v<decltype(nx::err{Error::Denied}), nx::err<Error>>);
}

TEST_CASE("result: value state via default, copy and move construction") {
  const nx::result<int, Error> d;
  CHECK(d.has_value());
  CHECK(static_cast<bool>(d));
  CHECK(*d == 0);

  int lvalue = 5;
  const nx::result<int, Error> from_lvalue = lvalue;
  CHECK(from_lvalue.has_value());
  CHECK(*from_lvalue == 5);

  nx::result<std::string, Error> from_rvalue = std::string("hello");
  REQUIRE(from_rvalue.has_value());
  CHECK(*from_rvalue == "hello");
  CHECK(from_rvalue->size() == 5);
}

TEST_CASE("result: error state via err, and accessors") {
  nx::result<int, Error> r = nx::err{Error::Denied};
  CHECK_FALSE(r.has_value());
  CHECK_FALSE(static_cast<bool>(r));
  CHECK(r.error() == Error::Denied);

  const nx::result<int, Error> &cr = r;
  CHECK(cr.error() == Error::Denied);

  CHECK(nx::result<int, Error>(nx::err{Error::NotFound}).error() ==
        Error::NotFound);

  const nx::err<Error> e{Error::NotFound};
  nx::result<int, Error> r2 = e;
  CHECK_FALSE(r2.has_value());
  CHECK(r2.error() == Error::NotFound);
}

TEST_CASE("result: value_or returns the fallback only in the error state") {
  const nx::result<int, Error> ok = 10;
  const nx::result<int, Error> bad = nx::err{Error::NotFound};

  CHECK(ok.value_or(99) == 10);
  CHECK(bad.value_or(99) == 99);

  CHECK(nx::result<int, Error>(7).value_or(99) == 7);
  CHECK(nx::result<int, Error>(nx::err{Error::None}).value_or(99) == 99);

  CHECK(bad.value_or(short(3)) == 3);

  nx::result<std::string, Error> s = std::string("abcdef");
  const std::string got = std::move(s).value_or("fallback");
  CHECK(got == "abcdef");
}

TEST_CASE("result: value() returns references that alias the stored object") {
  nx::result<int, Error> r = 1;
  r.value() = 42;
  CHECK(*r == 42);
  CHECK(&r.value() == &*r);

  const nx::result<int, Error> &cr = r;
  CHECK(cr.value() == 42);

  CHECK(nx::result<int, Error>(3).value() == 3);
}

TEST_CASE("result: operator-> reaches the stored value") {
  nx::result<std::string, Error> r = std::string("abc");
  CHECK(r->size() == 3);
  r->push_back('d');
  CHECK(*r == "abcd");

  const nx::result<std::string, Error> &cr = r;
  CHECK(cr->size() == 4);
}

TEST_CASE("result: copy and move construction preserve both states") {
  reset_counts();
  {
    nx::result<Payload, Error> src = Payload{1};
    const nx::result<Payload, Error> copy = src;
    REQUIRE(copy.has_value());
    CHECK(copy->value == 1);
    CHECK(src->value == 1);

    const nx::result<Payload, Error> moved = std::move(src);
    REQUIRE(moved.has_value());
    CHECK(moved->value == 1);
  }
  CHECK(g_ctors == g_dtors);

  const nx::result<Payload, std::string> bad = nx::err{std::string("nope")};
  const nx::result<Payload, std::string> bad_copy = bad;
  CHECK_FALSE(bad_copy.has_value());
  CHECK(bad_copy.error() == "nope");

  nx::result<Payload, std::string> bad2 = nx::err{std::string("nope2")};
  const nx::result<Payload, std::string> bad_moved = std::move(bad2);
  CHECK_FALSE(bad_moved.has_value());
  CHECK(bad_moved.error() == "nope2");
}

TEST_CASE("result: assignment across every state transition") {
  nx::result<std::string, Error> a = std::string("one");
  const nx::result<std::string, Error> b = std::string("two");
  a = b;
  REQUIRE(a.has_value());
  CHECK(*a == "two");

  const nx::result<std::string, Error> e = nx::err{Error::Denied};
  a = e;
  CHECK_FALSE(a.has_value());
  CHECK(a.error() == Error::Denied);

  const nx::result<std::string, Error> e2 = nx::err{Error::NotFound};
  a = e2;
  CHECK_FALSE(a.has_value());
  CHECK(a.error() == Error::NotFound);

  a = b;
  REQUIRE(a.has_value());
  CHECK(*a == "two");

  nx::result<std::string, Error> m = std::string("x");
  m = nx::result<std::string, Error>(std::string("y"));
  CHECK(*m == "y");
  m = nx::result<std::string, Error>(nx::err{Error::Denied});
  CHECK(m.error() == Error::Denied);
  m = nx::result<std::string, Error>(nx::err{Error::NotFound});
  CHECK(m.error() == Error::NotFound);
  m = nx::result<std::string, Error>(std::string("z"));
  CHECK(*m == "z");
}

TEST_CASE("result: self-assignment leaves the object intact") {
  nx::result<std::string, Error> r = std::string("keepme");
  const nx::result<std::string, Error> *alias = &r;
  r = *alias;
  REQUIRE(r.has_value());
  CHECK(*r == "keepme");

  nx::result<std::string, Error> *malias = &r;
  r = std::move(*malias);
  REQUIRE(r.has_value());
  CHECK(*r == "keepme");

  nx::result<std::string, Error> e = nx::err{Error::Denied};
  const nx::result<std::string, Error> *ealias = &e;
  e = *ealias;
  CHECK_FALSE(e.has_value());
  CHECK(e.error() == Error::Denied);
}

TEST_CASE("result: a failed cross-state copy leaves the old state intact") {
  const nx::result<ThrowOnCopy, Error> value = ThrowOnCopy{7};
  nx::result<ThrowOnCopy, Error> error = nx::err{Error::Denied};

  ThrowOnCopy::enabled = true;
  CHECK_THROWS_AS(error = value, std::runtime_error);
  ThrowOnCopy::enabled = false;
  CHECK_FALSE(error.has_value());
  CHECK(error.error() == Error::Denied);

  const nx::result<int, ThrowOnCopy> failed = nx::err{ThrowOnCopy{9}};
  nx::result<int, ThrowOnCopy> number = 3;
  ThrowOnCopy::enabled = true;
  CHECK_THROWS_AS(number = failed, std::runtime_error);
  ThrowOnCopy::enabled = false;
  REQUIRE(number.has_value());
  CHECK(*number == 3);
}

TEST_CASE("result: destructor destroys exactly the active alternative") {
  reset_counts();
  {
    const nx::result<Payload, Error> ok = Payload{1};
    CHECK(ok.has_value());
  }
  CHECK(g_dtors == g_ctors);

  reset_counts();
  {
    const nx::result<Payload, Error> bad = nx::err{Error::Denied};
    CHECK_FALSE(bad.has_value());
  }
  CHECK(g_ctors == 0);
  CHECK(g_dtors == 0);
}

TEST_CASE("result: move-only value types are supported end to end") {
  nx::result<MoveOnly, Error> r = MoveOnly{5};
  REQUIRE(r.has_value());
  CHECK(r->value == 5);

  nx::result<MoveOnly, Error> moved = std::move(r);
  CHECK(moved->value == 5);

  const MoveOnly out = std::move(moved).value();
  CHECK(out.value == 5);
}

TEST_CASE(
    "result: and_then chains on the value and short-circuits on the error") {
  const auto twice = [](int v) { return nx::result<int, Error>(v * 2); };
  const auto fail = [](int) {
    return nx::result<int, Error>(nx::err{Error::Denied});
  };

  nx::result<int, Error> ok = 21;
  CHECK(*ok.and_then(twice) == 42);
  CHECK(ok.and_then(fail).error() == Error::Denied);

  nx::result<int, Error> bad = nx::err{Error::NotFound};
  const auto chained = bad.and_then(twice);
  CHECK_FALSE(chained.has_value());
  CHECK(chained.error() == Error::NotFound);

  const nx::result<int, Error> cok = 3;
  CHECK(*cok.and_then(
            [](const int &v) { return nx::result<int, Error>(v + 1); }) == 4);
  CHECK(*nx::result<int, Error>(4).and_then(
            [](int &&v) { return nx::result<int, Error>(v + 1); }) == 5);

  CHECK(*ok.and_then([](int v) {
    return nx::result<std::string, Error>(std::to_string(v));
  }) == "21");
}

TEST_CASE("result: transform maps the value and forwards the error") {
  nx::result<int, Error> ok = 5;
  const auto mapped = ok.transform([](int v) { return v * 3; });
  static_assert(std::is_same_v<decltype(mapped), const nx::result<int, Error>>);
  CHECK(*mapped == 15);

  const auto as_string =
      ok.transform([](int v) { return std::to_string(v) + "!"; });
  CHECK(*as_string == "5!");

  nx::result<int, Error> bad = nx::err{Error::Denied};
  const auto bad_mapped = bad.transform([](int v) { return v * 3; });
  CHECK_FALSE(bad_mapped.has_value());
  CHECK(bad_mapped.error() == Error::Denied);

  const nx::result<int, Error> cok = 2;
  CHECK(*cok.transform([](const int &v) { return v + 1; }) == 3);
  CHECK(*nx::result<int, Error>(2).transform([](int &&v) { return v + 8; }) ==
        10);
}

TEST_CASE(
    "result: or_else recovers from the error and passes the value through") {
  const auto recover = [](Error) { return nx::result<int, Error>(0); };

  nx::result<int, Error> bad = nx::err{Error::NotFound};
  const auto recovered = bad.or_else(recover);
  REQUIRE(recovered.has_value());
  CHECK(*recovered == 0);

  nx::result<int, Error> ok = 9;
  const auto untouched = ok.or_else(recover);
  REQUIRE(untouched.has_value());
  CHECK(*untouched == 9);

  const auto rethrow = [](Error) {
    return nx::result<int, std::string>(nx::err{std::string("still bad")});
  };
  nx::result<int, Error> bad2 = nx::err{Error::Denied};
  CHECK(bad2.or_else(rethrow).error() == "still bad");

  const nx::result<int, Error> cbad = nx::err{Error::Denied};
  CHECK(*cbad.or_else(
            [](const Error &) { return nx::result<int, Error>(1); }) == 1);
  CHECK(*nx::result<int, Error>(nx::err{Error::Denied}).or_else([](Error &&) {
    return nx::result<int, Error>(2);
  }) == 2);
}

TEST_CASE("result: transform_error maps the error and forwards the value") {
  const auto describe = [](Error e) {
    return e == Error::NotFound ? std::string("not found")
                                : std::string("other");
  };

  nx::result<int, Error> bad = nx::err{Error::NotFound};
  const auto mapped = bad.transform_error(describe);
  static_assert(
      std::is_same_v<decltype(mapped), const nx::result<int, std::string>>);
  CHECK_FALSE(mapped.has_value());
  CHECK(mapped.error() == "not found");

  nx::result<int, Error> ok = 8;
  const auto untouched = ok.transform_error(describe);
  REQUIRE(untouched.has_value());
  CHECK(*untouched == 8);

  const nx::result<int, Error> cbad = nx::err{Error::Denied};
  CHECK(cbad.transform_error([](const Error &) { return 7; }).error() == 7);
  CHECK(nx::result<int, Error>(nx::err{Error::Denied})
            .transform_error([](Error &&) { return 8; })
            .error() == 8);
}

TEST_CASE("result: monadic combinators compose into a pipeline") {
  const auto parse = [](const std::string &s) -> nx::result<int, Error> {
    if (s.empty())
      return nx::err{Error::NotFound};
    int out = 0;
    for (const char c : s) {
      if (c < '0' || c > '9')
        return nx::err{Error::Denied};
      out = out * 10 + (c - '0');
    }
    return out;
  };

  const auto pipeline = [&](const std::string &s) {
    return parse(s)
        .and_then([](int v) { return nx::result<int, Error>(v + 1); })
        .transform([](int v) { return v * 2; })
        .transform_error([](Error e) { return static_cast<int>(e); });
  };

  CHECK(*pipeline("20") == 42);
  CHECK(pipeline("").error() == static_cast<int>(Error::NotFound));
  CHECK(pipeline("2a").error() == static_cast<int>(Error::Denied));
}

TEST_CASE("result: constexpr construction and inspection") {
  constexpr nx::result<int, Error> ok(7);
  static_assert(ok.has_value());
  static_assert(*ok == 7);

  constexpr nx::result<int, Error> bad(nx::err{Error::Denied});
  static_assert(!bad.has_value());
  static_assert(bad.error() == Error::Denied);

  static_assert(ok.value_or(99) == 7);
  static_assert(bad.value_or(99) == 99);

  CHECK(ok.has_value());
  CHECK(bad.error() == Error::Denied);
}

TEST_CASE("result<void>: default construction is the success state") {
  const nx::result<void, Error> ok;
  CHECK(ok.has_value());
  CHECK(static_cast<bool>(ok));
  CHECK_NOTHROW(ok.value());
  CHECK_NOTHROW(*ok);
}

TEST_CASE("result<void>: error state carries the error") {
  nx::result<void, Error> bad = nx::err{Error::Denied};
  CHECK_FALSE(bad.has_value());
  CHECK_FALSE(static_cast<bool>(bad));
  CHECK(bad.error() == Error::Denied);

  const nx::result<void, Error> &cbad = bad;
  CHECK(cbad.error() == Error::Denied);
  CHECK(nx::result<void, Error>(nx::err{Error::NotFound}).error() ==
        Error::NotFound);

  const nx::err<Error> e{Error::NotFound};
  const nx::result<void, Error> from_lvalue = e;
  CHECK(from_lvalue.error() == Error::NotFound);
}

TEST_CASE("result<void>: copy, move and assignment across both states") {
  const nx::result<void, std::string> ok;
  const nx::result<void, std::string> ok_copy = ok;
  CHECK(ok_copy.has_value());

  const nx::result<void, std::string> bad = nx::err{std::string("bad")};
  const nx::result<void, std::string> bad_copy = bad;
  CHECK_FALSE(bad_copy.has_value());
  CHECK(bad_copy.error() == "bad");

  nx::result<void, std::string> src = nx::err{std::string("moved")};
  const nx::result<void, std::string> moved = std::move(src);
  CHECK(moved.error() == "moved");

  nx::result<void, std::string> a;
  a = bad;
  CHECK(a.error() == "bad");
  a = nx::err{std::string("other")};
  CHECK(a.error() == "other");
  a = ok;
  CHECK(a.has_value());
  a = ok;
  CHECK(a.has_value());

  a = nx::result<void, std::string>(nx::err{std::string("mv")});
  CHECK(a.error() == "mv");
  const nx::result<void, std::string> *alias = &a;
  a = *alias;
  CHECK(a.error() == "mv");
  nx::result<void, std::string> *malias = &a;
  a = std::move(*malias);
  CHECK(a.error() == "mv");
}

TEST_CASE("result<void>: a failed error copy remains successful") {
  const nx::result<void, ThrowOnCopy> failed = nx::err{ThrowOnCopy{4}};
  nx::result<void, ThrowOnCopy> ok;

  ThrowOnCopy::enabled = true;
  CHECK_THROWS_AS(ok = failed, std::runtime_error);
  ThrowOnCopy::enabled = false;
  CHECK(ok.has_value());
}

TEST_CASE("result<void>: and_then runs the continuation only on success") {
  int calls = 0;
  const auto step = [&calls] {
    ++calls;
    return nx::result<int, Error>(1);
  };

  nx::result<void, Error> ok;
  CHECK(*ok.and_then(step) == 1);
  CHECK(calls == 1);

  nx::result<void, Error> bad = nx::err{Error::Denied};
  const auto skipped = bad.and_then(step);
  CHECK(calls == 1);
  CHECK(skipped.error() == Error::Denied);

  const nx::result<void, Error> cok;
  CHECK(*cok.and_then(step) == 1);
  CHECK(*nx::result<void, Error>().and_then(step) == 1);
  CHECK(calls == 3);
}

TEST_CASE("result<void>: transform lifts a value into the result") {
  nx::result<void, Error> ok;
  const auto lifted = ok.transform([] { return 5; });
  static_assert(std::is_same_v<decltype(lifted), const nx::result<int, Error>>);
  CHECK(*lifted == 5);

  nx::result<void, Error> bad = nx::err{Error::NotFound};
  const auto forwarded = bad.transform([] { return 5; });
  CHECK(forwarded.error() == Error::NotFound);

  const nx::result<void, Error> cok;
  CHECK(*cok.transform([] { return 6; }) == 6);
  CHECK(*nx::result<void, Error>().transform([] { return 7; }) == 7);
}

TEST_CASE("result<void>: or_else and transform_error") {
  const auto recover = [](Error) { return nx::result<void, Error>(); };

  nx::result<void, Error> bad = nx::err{Error::Denied};
  CHECK(bad.or_else(recover).has_value());

  nx::result<void, Error> ok;
  CHECK(ok.or_else(recover).has_value());

  const nx::result<void, Error> cbad = nx::err{Error::Denied};
  CHECK(cbad.or_else([](const Error &) { return nx::result<void, Error>(); })
            .has_value());
  CHECK(nx::result<void, Error>(nx::err{Error::Denied})
            .or_else([](Error &&) { return nx::result<void, Error>(); })
            .has_value());

  const auto to_int = [](Error e) { return static_cast<int>(e); };
  CHECK(bad.transform_error(to_int).error() == static_cast<int>(Error::Denied));
  CHECK(ok.transform_error(to_int).has_value());
  CHECK(cbad.transform_error([](const Error &) { return 1; }).error() == 1);
  CHECK(nx::result<void, Error>(nx::err{Error::Denied})
            .transform_error([](Error &&) { return 2; })
            .error() == 2);
  CHECK(nx::result<void, Error>()
            .transform_error([](Error &&) { return 2; })
            .has_value());
}

TEST_CASE("result<void>: the error alternative is destroyed exactly once") {
  reset_counts();
  {
    const nx::result<void, Payload> bad = nx::err{Payload{3}};
    CHECK_FALSE(bad.has_value());
    CHECK(bad.error().value == 3);
  }
  CHECK(g_ctors == g_dtors);

  reset_counts();
  {
    const nx::result<void, Payload> ok;
    CHECK(ok.has_value());
  }
  CHECK(g_ctors == 0);
  CHECK(g_dtors == 0);
}

TEST_CASE("result<void>: constexpr usage") {
  constexpr nx::result<void, Error> ok;
  static_assert(ok.has_value());

  constexpr nx::result<void, Error> bad(nx::err{Error::NotFound});
  static_assert(!bad.has_value());
  static_assert(bad.error() == Error::NotFound);

  CHECK(ok.has_value());
}

TEST_CASE("result: converts from anything the value is made from") {
  const nx::result<std::string, Error> text = "hello";
  CHECK(*text == "hello");

  nx::result<long long, Error> wide = 7;
  CHECK(*wide == 7);

  static_assert(std::is_constructible_v<nx::result<MoveOnly, Error>, int>);
  static_assert(!std::is_convertible_v<int, nx::result<MoveOnly, Error>>);
  const nx::result<MoveOnly, Error> made(3);
  CHECK(made->value == 3);

  const nx::result<long long, Error> bad = nx::err{Error::Denied};
  CHECK(bad.error() == Error::Denied);
}

TEST_CASE("result: transform takes a function that returns void") {
  int seen = 0;
  nx::result<int, Error> ok = 4;
  const auto done = ok.transform([&](int v) { seen = v; });
  static_assert(std::is_same_v<decltype(done), const nx::result<void, Error>>);
  CHECK(done.has_value());
  CHECK(seen == 4);

  nx::result<int, Error> bad = nx::err{Error::NotFound};
  const auto skipped = bad.transform([&](int) { seen = -1; });
  CHECK(skipped.error() == Error::NotFound);
  CHECK(seen == 4);

  nx::result<void, Error> unit;
  CHECK(unit.transform([&] { seen = 9; }).has_value());
  CHECK(seen == 9);
  nx::result<void, Error> unit_bad = nx::err{Error::Denied};
  CHECK(unit_bad.transform([&] { seen = 0; }).error() == Error::Denied);
  CHECK(seen == 9);
}

TEST_CASE("result: error_or returns the error or the fallback") {
  const nx::result<int, Error> ok = 1;
  const nx::result<int, Error> bad = nx::err{Error::Denied};
  CHECK(ok.error_or(Error::None) == Error::None);
  CHECK(bad.error_or(Error::None) == Error::Denied);
  CHECK(nx::result<int, std::string>(nx::err{std::string("gone")})
            .error_or("none") == "gone");

  const nx::result<void, Error> vok;
  const nx::result<void, Error> vbad = nx::err{Error::NotFound};
  CHECK(vok.error_or(Error::None) == Error::None);
  CHECK(vbad.error_or(Error::None) == Error::NotFound);
}

TEST_CASE("result: emplace replaces either alternative") {
  reset_counts();
  {
    nx::result<Payload, Payload> r = nx::err{Payload{1}};
    Payload &p = r.emplace(5);
    CHECK(r.has_value());
    CHECK(p.value == 5);
    CHECK(r->value == 5);
    r.emplace(6);
    CHECK(r->value == 6);
  }
  CHECK(g_ctors == g_dtors);

  nx::result<void, Error> v = nx::err{Error::Denied};
  v.emplace();
  CHECK(v.has_value());
}

TEST_CASE("result: swap exchanges every combination of alternatives") {
  reset_counts();
  {
    nx::result<Payload, Payload> a(Payload{1});
    nx::result<Payload, Payload> b(Payload{2});
    swap(a, b);
    CHECK(a->value == 2);
    CHECK(b->value == 1);

    nx::result<Payload, Payload> c = nx::err{Payload{3}};
    nx::result<Payload, Payload> d = nx::err{Payload{4}};
    c.swap(d);
    CHECK(c.error().value == 4);
    CHECK(d.error().value == 3);

    a.swap(c);
    CHECK_FALSE(a.has_value());
    CHECK(a.error().value == 4);
    REQUIRE(c.has_value());
    CHECK(c->value == 2);

    a.swap(c);
    REQUIRE(a.has_value());
    CHECK(a->value == 2);
    CHECK_FALSE(c.has_value());
    CHECK(c.error().value == 4);
  }
  CHECK(g_ctors == g_dtors);

  nx::result<void, Error> ok;
  nx::result<void, Error> bad = nx::err{Error::Denied};
  swap(ok, bad);
  CHECK_FALSE(ok.has_value());
  CHECK(ok.error() == Error::Denied);
  CHECK(bad.has_value());
  nx::result<void, Error> other = nx::err{Error::NotFound};
  ok.swap(other);
  CHECK(ok.error() == Error::NotFound);
  CHECK(other.error() == Error::Denied);
}

TEST_CASE("result: equality compares the held alternative") {
  const nx::result<int, Error> one = 1;
  const nx::result<long long, Error> wide_one = 1LL;
  const nx::result<int, Error> two = 2;
  const nx::result<int, Error> denied = nx::err{Error::Denied};
  const nx::result<int, Error> missing = nx::err{Error::NotFound};

  CHECK(one == wide_one);
  CHECK(one != two);
  CHECK(one != denied);
  CHECK(denied == nx::result<int, Error>(nx::err{Error::Denied}));
  CHECK(denied != missing);

  CHECK(one == 1);
  CHECK(one != 2);
  CHECK(denied != 1);
  CHECK(nx::result<int, int>(nx::err{1}) != 1);
  CHECK(nx::result<int, int>(nx::err{1}) != nx::result<int, int>(1));

  CHECK(denied == nx::err{Error::Denied});
  CHECK(denied != nx::err{Error::NotFound});
  CHECK(one != nx::err{Error::Denied});

  const nx::result<void, Error> vok;
  const nx::result<void, Error> vbad = nx::err{Error::Denied};
  CHECK(vok == nx::result<void, Error>());
  CHECK(vok != vbad);
  CHECK(vbad == nx::err{Error::Denied});
  CHECK(vbad != nx::err{Error::NotFound});
  CHECK(vok != nx::err{Error::Denied});
}
