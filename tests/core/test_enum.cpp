#include "framework/nxtest.h"

#include "core/foundation/core/enum.h"

#include <iterator>
#include <vector>

namespace {

enum class Perm : u32 {
  None = 0,
  Read = 1 << 0,
  Write = 1 << 1,
  Exec = 1 << 2,
  All = Read | Write | Exec,
};

enum class Color { Red, Green, Blue, Count };

enum class Small : u8 { A, B, C, Count };

enum class Signed : i8 { Neg = -2, Zero = 0, Pos = 2 };

NX_ENUM_FLAGS_DEFINE(Perm, PermFlags)

}

NX_ENUM_REG_META(Color, NX_ENUM_META(Color::Red, "red"),
                 NX_ENUM_META(Color::Green, "green"),
                 NX_ENUM_META(Color::Blue, "blue"))

TEST_CASE("enums::flags: construction and value()") {
  constexpr nx::enums::flags<Perm> empty;
  static_assert(empty.value() == 0);
  CHECK(empty.none());
  CHECK_FALSE(empty.any());
  CHECK_FALSE(static_cast<bool>(empty));

  const nx::enums::flags<Perm> one = Perm::Read;
  CHECK(one.value() == 1u);
  CHECK(one.any());
  CHECK(static_cast<bool>(one));

  const nx::enums::flags<Perm> raw{u32(0b101)};
  CHECK(raw.value() == 5u);
  CHECK(raw.has(Perm::Read));
  CHECK(raw.has(Perm::Exec));
  CHECK_FALSE(raw.has(Perm::Write));

  static_assert(std::is_same_v<nx::enums::flags<Perm>::underlying_type, u32>);
}

TEST_CASE("enums::flags: has, has_any and has_all") {
  const PermFlags rw = Perm::Read | Perm::Write;

  CHECK(rw.has(Perm::Read));
  CHECK(rw.has(Perm::Write));
  CHECK_FALSE(rw.has(Perm::Exec));

  CHECK_FALSE(rw.has(Perm::None));

  CHECK(rw.has_any(Perm::Read));
  CHECK(rw.has_any(Perm::Read | Perm::Exec));
  CHECK_FALSE(rw.has_any(PermFlags{Perm::Exec}));
  CHECK_FALSE(rw.has_any(PermFlags{}));

  CHECK(rw.has_all(Perm::Read | Perm::Write));
  CHECK(rw.has_all(PermFlags{Perm::Read}));
  CHECK_FALSE(rw.has_all(Perm::Read | Perm::Exec));

  CHECK(rw.has_all(PermFlags{}));
  CHECK(PermFlags{}.has_all(PermFlags{}));
  CHECK_FALSE(PermFlags{}.has_all(PermFlags{Perm::Read}));
}

TEST_CASE("enums::flags: set, clear, toggle and reset are chainable") {
  nx::enums::flags<Perm> f;

  CHECK(f.set(Perm::Read).set(Perm::Write).value() == 3u);
  CHECK(f.has(Perm::Read));
  CHECK(f.has(Perm::Write));

  CHECK(f.set(Perm::Read).value() == 3u);

  CHECK(f.clear(Perm::Read).value() == 2u);
  CHECK_FALSE(f.has(Perm::Read));
  CHECK(f.clear(Perm::Read).value() == 2u);

  CHECK(f.toggle(Perm::Read).value() == 3u);
  CHECK(f.toggle(Perm::Read).value() == 2u);

  CHECK(f.reset().value() == 0u);
  CHECK(f.none());
}

TEST_CASE("enums::flags: bitwise operators produce new values") {
  const nx::enums::flags<Perm> r = Perm::Read;
  const nx::enums::flags<Perm> w = Perm::Write;

  CHECK((r | w).value() == 3u);
  CHECK((r | Perm::Exec).value() == 5u);
  CHECK((r & w).value() == 0u);
  CHECK(((r | w) & Perm::Read).value() == 1u);
  CHECK(((r | w) & w).value() == 2u);
  CHECK((r ^ w).value() == 3u);
  CHECK((r ^ Perm::Read).value() == 0u);
  CHECK(((r | w) ^ (r | w)).value() == 0u);

  CHECK((~r).value() == ~u32(1));
  CHECK((~nx::enums::flags<Perm>{}).value() == ~u32(0));

  CHECK(r.value() == 1u);
  CHECK(w.value() == 2u);
}

TEST_CASE("enums::flags: compound assignment mutates in place") {
  nx::enums::flags<Perm> f;

  f |= Perm::Read;
  CHECK(f.value() == 1u);
  f |= nx::enums::flags<Perm>{Perm::Write};
  CHECK(f.value() == 3u);

  f &= Perm::Read;
  CHECK(f.value() == 1u);
  f |= Perm::Exec;
  f &= nx::enums::flags<Perm>{Perm::Exec};
  CHECK(f.value() == 4u);

  f ^= Perm::Exec;
  CHECK(f.value() == 0u);
  f ^= nx::enums::flags<Perm>{Perm::Write};
  CHECK(f.value() == 2u);
}

TEST_CASE(
    "enums::flags: equality against flags and against a bare enumerator") {
  const nx::enums::flags<Perm> rw = Perm::Read | Perm::Write;

  CHECK(rw == (Perm::Read | Perm::Write));
  CHECK(rw != nx::enums::flags<Perm>{Perm::Read});
  CHECK_FALSE(rw == nx::enums::flags<Perm>{Perm::Read});

  CHECK(nx::enums::flags<Perm>{Perm::Read} == Perm::Read);
  CHECK_FALSE(rw == Perm::Read);
  CHECK_FALSE(rw == Perm::All);
  CHECK(nx::enums::flags<Perm>{Perm::All} == Perm::All);
}

TEST_CASE(
    "enums::flags: NX_ENUM_FLAGS_DEFINE lifts the operators onto the enum") {
  static_assert(std::is_same_v<decltype(Perm::Read | Perm::Write),
                               nx::enums::flags<Perm>>);
  static_assert(std::is_same_v<PermFlags, nx::enums::flags<Perm>>);

  CHECK((Perm::Read | Perm::Write).value() == 3u);
  CHECK((Perm::All & Perm::Read).value() == 1u);
  CHECK((Perm::Read ^ Perm::Read).value() == 0u);
  CHECK((~Perm::Read).value() == ~u32(1));

  static_assert(nx::enums::has_flags<Perm>);
  static_assert(!nx::enums::has_flags<Color>);
}

TEST_CASE("enums::flags: is constexpr-usable end to end") {
  constexpr auto f = [] {
    nx::enums::flags<Perm> x;
    x.set(Perm::Read).set(Perm::Exec);
    x.toggle(Perm::Write);
    x.clear(Perm::Read);
    return x;
  }();
  static_assert(f.value() == 6u);
  static_assert(f.has(Perm::Write));
  static_assert(f.has(Perm::Exec));
  static_assert(!f.has(Perm::Read));
  CHECK(f.value() == 6u);
}

TEST_CASE("enums::map: is sized from Count and indexed by the enumerator") {
  nx::enums::map<Color, int> m;
  CHECK(m.size() == 3);

  CHECK(m[Color::Red] == 0);
  CHECK(m[Color::Green] == 0);
  CHECK(m[Color::Blue] == 0);

  m[Color::Red] = 10;
  m[Color::Green] = 20;
  m[Color::Blue] = 30;
  CHECK(m[Color::Red] == 10);
  CHECK(m[Color::Green] == 20);
  CHECK(m[Color::Blue] == 30);

  const nx::enums::map<Color, int> &cm = m;
  CHECK(cm[Color::Blue] == 30);

  std::vector<int> seen;
  for (const int v : m)
    seen.push_back(v);
  REQUIRE(seen.size() == 3);
  CHECK(seen[0] == 10);
  CHECK(seen[1] == 20);
  CHECK(seen[2] == 30);

  std::vector<int> cseen;
  for (const int v : cm)
    cseen.push_back(v);
  CHECK(cseen.size() == 3);
}

TEST_CASE("enums::map: honours an explicit size and non-trivial value types") {
  nx::enums::map<Color, int, 2> pair;
  CHECK(pair.size() == 2);
  pair[Color::Red] = 1;
  pair[Color::Green] = 2;
  CHECK(pair[Color::Red] == 1);
  CHECK(pair[Color::Green] == 2);

  nx::enums::map<Color, std::vector<int>> vectors;
  CHECK(vectors[Color::Red].empty());
  vectors[Color::Red].push_back(7);
  CHECK(vectors[Color::Red].size() == 1);
  CHECK(vectors[Color::Green].empty());

  nx::enums::map<Small, int> small;
  CHECK(small.size() == 3);
  small[Small::C] = 5;
  CHECK(small[Small::C] == 5);
}

TEST_CASE("enums::map: is usable at compile time") {
  constexpr auto built = [] {
    nx::enums::map<Color, int> m;
    m[Color::Red] = 1;
    m[Color::Blue] = 3;
    return m;
  }();
  static_assert(built[Color::Red] == 1);
  static_assert(built[Color::Green] == 0);
  static_assert(built[Color::Blue] == 3);
  static_assert(built.size() == 3);
  CHECK(built[Color::Blue] == 3);
}

TEST_CASE("enums::range: default range covers [0, Count)") {
  std::vector<Color> seen;
  for (const Color c : nx::enums::range<Color>())
    seen.push_back(c);

  REQUIRE(seen.size() == 3);
  CHECK(seen[0] == Color::Red);
  CHECK(seen[1] == Color::Green);
  CHECK(seen[2] == Color::Blue);
}

TEST_CASE("enums::range: explicit first and last bounds are inclusive") {
  std::vector<Color> seen;
  for (const Color c : nx::enums::range<Color>(Color::Green, Color::Blue))
    seen.push_back(c);
  REQUIRE(seen.size() == 2);
  CHECK(seen[0] == Color::Green);
  CHECK(seen[1] == Color::Blue);

  int count = 0;
  for (const Color c : nx::enums::range<Color>(Color::Red, Color::Red)) {
    CHECK(c == Color::Red);
    ++count;
  }
  CHECK(count == 1);

  std::vector<Color> tail;
  for (const Color c : nx::enums::range<Color>(Color::Green))
    tail.push_back(c);
  REQUIRE(tail.size() == 2);
  CHECK(tail[0] == Color::Green);
  CHECK(tail[1] == Color::Blue);
}

TEST_CASE("enums::range: works with a byte-backed enum") {
  std::vector<Small> seen;
  for (const Small s : nx::enums::range<Small>())
    seen.push_back(s);
  REQUIRE(seen.size() == 3);
  CHECK(seen[0] == Small::A);
  CHECK(seen[2] == Small::C);

  std::vector<Small> partial;
  for (const Small s : nx::enums::range<Small>(Small::B, Small::C))
    partial.push_back(s);
  CHECK(partial.size() == 2);
}

TEST_CASE("enums::range: works with a signed enum spanning negative values") {
  std::vector<Signed> seen;
  for (const Signed s : nx::enums::range<Signed>(Signed::Neg, Signed::Pos))
    seen.push_back(s);
  REQUIRE(seen.size() == 5);
  CHECK(seen.front() == Signed::Neg);
  CHECK(seen.back() == Signed::Pos);
}

TEST_CASE("enums::range: reverse iteration visits the same values backwards") {
  std::vector<Color> seen;
  const nx::enums::range<Color> r;
  for (auto it = r.rbegin(); it != r.rend(); ++it)
    seen.push_back(*it);

  REQUIRE(seen.size() == 3);
  CHECK(seen[0] == Color::Blue);
  CHECK(seen[1] == Color::Green);
  CHECK(seen[2] == Color::Red);
}

TEST_CASE("enums::range: iterator supports pre/post increment and decrement") {
  const nx::enums::range<Color> r;
  auto it = r.begin();
  CHECK(*it == Color::Red);

  auto post = it++;
  CHECK(*post == Color::Red);
  CHECK(*it == Color::Green);

  ++it;
  CHECK(*it == Color::Blue);

  auto postdec = it--;
  CHECK(*postdec == Color::Blue);
  CHECK(*it == Color::Green);

  --it;
  CHECK(*it == Color::Red);
  CHECK(it == r.begin());
  CHECK_FALSE(it == r.end());
}

TEST_CASE(
    "enums: to_string and from_string round-trip through the meta table") {
  static_assert(nx::enums::has_meta<Color>);
  static_assert(!nx::enums::has_meta<Perm>);

  CHECK(nx::to_string(Color::Red) == "red");
  CHECK(nx::to_string(Color::Green) == "green");
  CHECK(nx::to_string(Color::Blue) == "blue");

  CHECK(nx::to_string(Color::Count).empty());
  CHECK(nx::to_string(Color::Count, "?") == "?");

  const auto red = nx::from_string<Color>("red");
  REQUIRE(red.has_value());
  CHECK(*red == Color::Red);
  CHECK(*nx::from_string<Color>("blue") == Color::Blue);

  CHECK_FALSE(nx::from_string<Color>("Red").has_value());
  CHECK_FALSE(nx::from_string<Color>("re").has_value());
  CHECK_FALSE(nx::from_string<Color>("redd").has_value());
  CHECK_FALSE(nx::from_string<Color>("").has_value());

  for (const Color c : nx::enums::range<Color>()) {
    const auto back = nx::from_string<Color>(nx::to_string(c));
    REQUIRE(back.has_value());
    CHECK(*back == c);
  }

  static_assert(nx::to_string(Color::Green) == "green");
}

TEST_CASE("enums: has_count concept detects the Count enumerator") {
  static_assert(nx::enums::has_count<Color>);
  static_assert(nx::enums::has_count<Small>);
  static_assert(!nx::enums::has_count<Signed>);
  CHECK(true);
}

namespace {

enum class NoneCounted : u8 { Count = 0 };
enum class Full8 : u8 {};
enum class Full32 : u32 {};
enum class Signed8 : i8 {};

template <typename E> std::vector<E> collect(const nx::enums::range<E> &r) {
  std::vector<E> out;
  for (const E e : r) {
    out.push_back(e);
    if (out.size() > 300)
      break;
  }
  return out;
}

}

TEST_CASE("enums::range: a Count of zero is empty") {
  const nx::enums::range<NoneCounted> r;
  CHECK(r.begin() == r.end());
  CHECK(collect(r).empty());
  CHECK(r.rbegin() == r.rend());
}

TEST_CASE("enums::range: the underlying type's maximum is a valid last") {
  const auto bytes = collect(
      nx::enums::range<Full8>(nx::cast<Full8>(250), nx::cast<Full8>(255)));
  REQUIRE(bytes.size() == 6);
  CHECK(bytes.front() == nx::cast<Full8>(250));
  CHECK(bytes.back() == nx::cast<Full8>(255));

  const auto words = collect(nx::enums::range<Full32>(
      nx::cast<Full32>(0xffff'ffffu), nx::cast<Full32>(0xffff'ffffu)));
  CHECK(words.size() == 1);
  const nx::enums::range<Full32> every(nx::cast<Full32>(0u),
                                       nx::cast<Full32>(0xffff'ffffu));
  CHECK(every.begin() != every.end());
  CHECK(*std::prev(every.end()) == nx::cast<Full32>(0xffff'ffffu));

  const auto all = collect(
      nx::enums::range<Full8>(nx::cast<Full8>(0), nx::cast<Full8>(255)));
  CHECK(all.size() == 256);

  const nx::enums::range<Signed8> top(nx::cast<Signed8>(126),
                                      nx::cast<Signed8>(127));
  std::vector<Signed8> backwards;
  for (auto it = top.rbegin(); it != top.rend(); ++it)
    backwards.push_back(*it);
  REQUIRE(backwards.size() == 2);
  CHECK(backwards[0] == nx::cast<Signed8>(127));
  CHECK(backwards[1] == nx::cast<Signed8>(126));
}

TEST_CASE("enums::range: first past last is empty") {
  const nx::enums::range<Full8> r(nx::cast<Full8>(5), nx::cast<Full8>(3));
  CHECK(r.begin() == r.end());
  CHECK(collect(r).empty());
}
