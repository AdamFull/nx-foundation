#include "framework/nxtest.h"

#include "core/foundation/strings/id_string.h"

#include <algorithm>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace nx::literals;

TEST_CASE("id_string: the default and empty forms share id 0") {
  constexpr nx::id_string d;
  static_assert(d.id() == 0);
  static_assert(d.empty());

  CHECK(d.empty());
  CHECK_FALSE(static_cast<bool>(d));
  CHECK(d.id() == 0);

  const nx::id_string from_empty_view{nx::string_view("")};
  CHECK(from_empty_view.id() == 0);
  CHECK(from_empty_view == d);

  const nx::id_string from_empty_literal("");
  CHECK(from_empty_literal.id() == 0);

  CHECK(d.is_resolvable());
  CHECK(d.view().empty());
  CHECK(std::string(d.c_str()).empty());
  CHECK(d.to_string().empty());
}

TEST_CASE("id_string: the id is the FNV-1a hash of the bytes") {
  const nx::id_string a("hello");
  CHECK(a.id() == nx::hash_fnv1a64("hello", 5));
  CHECK_FALSE(a.empty());
  CHECK(static_cast<bool>(a));

  CHECK(nx::id_string("hello").id() == a.id());
  CHECK(nx::id_string(nx::string_view("hello")).id() == a.id());
  CHECK(nx::id_string(nx::string("hello")).id() == a.id());

  CHECK(nx::id_string("hell").id() != a.id());
  CHECK(nx::id_string("hello ").id() != a.id());
  CHECK(nx::id_string("Hello").id() != a.id());
  CHECK(nx::id_string("olleh").id() != a.id());
}

TEST_CASE("id_string: a hash that lands on zero is remapped") {
  CHECK(nx::detail::id_string_hash("") == 0);
  for (int i = 0; i < 2000; ++i) {
    const std::string s = "probe_" + std::to_string(i);
    CHECK(nx::detail::id_string_hash(nx::string_view(s)) != 0);
  }
}

TEST_CASE("id_string: from_literal computes the id at compile time") {
  constexpr nx::id_string jump = nx::id_string::from_literal("jump");
  static_assert(jump.id() == nx::hash_fnv1a64("jump", 4));
  static_assert(!jump.empty());
  static_assert(jump == nx::id_string::from_literal("jump"));
  static_assert(jump != nx::id_string::from_literal("fire"));

  CHECK(nx::id_string("jump").id() == jump.id());

  int hit = 0;
  switch (nx::id_string("fire").id()) {
  case nx::id_string::from_literal("jump").id():
    hit = 1;
    break;
  case nx::id_string::from_literal("fire").id():
    hit = 2;
    break;
  default:
    hit = 3;
    break;
  }
  CHECK(hit == 2);
}

TEST_CASE("id_string: from_id reconstructs without interning") {
  const nx::id_string original("round_trip_subject");
  const nx::id_string rebuilt = nx::id_string::from_id(original.id());

  CHECK(rebuilt == original);
  CHECK(rebuilt.id() == original.id());
  CHECK(rebuilt.view() == "round_trip_subject");

  static_assert(nx::id_string::from_id(0).empty());
  CHECK(nx::id_string::from_id(0).empty());

  const nx::id_string unknown = nx::id_string::from_id(0xDEADBEEFDEADBEEFull);
  CHECK(unknown.id() == 0xDEADBEEFDEADBEEFull);
  CHECK_FALSE(unknown.empty());
  CHECK_FALSE(unknown.is_resolvable());
  CHECK(unknown.view().empty());
  CHECK(std::string(unknown.c_str()).empty());
  CHECK(unknown.to_string().empty());
}

TEST_CASE("id_string: run-time construction interns the original bytes") {
  const nx::id_string s("the quick brown fox");
  CHECK(s.is_resolvable());
  CHECK(s.view() == "the quick brown fox");
  CHECK(std::string(s.c_str()) == "the quick brown fox");
  CHECK(s.to_string() == "the quick brown fox");

  CHECK(s.c_str()[s.view().size()] == '\0');

  CHECK(nx::id_string(nx::string_view("view_form")).view() == "view_form");
  CHECK(nx::id_string(nx::string("string_form")).view() == "string_form");
  CHECK(nx::id_string("pointer_form").view() == "pointer_form");

  const nx::id_string again("the quick brown fox");
  CHECK(again.view().data() == s.view().data());
}

TEST_CASE("id_string: interned bytes stay valid as the pool grows") {
  std::vector<nx::id_string> ids;
  std::vector<const char *> pointers;
  for (int i = 0; i < 3000; ++i) {
    const std::string text = "stable_pointer_subject_" + std::to_string(i);
    const nx::id_string id{nx::string_view(text)};
    ids.push_back(id);
    pointers.push_back(id.view().data());
  }

  for (int i = 0; i < 3000; ++i) {
    const std::string expected = "stable_pointer_subject_" + std::to_string(i);
    REQUIRE(ids[static_cast<usize>(i)].is_resolvable());
    CHECK(ids[static_cast<usize>(i)].view() == expected);
    CHECK(ids[static_cast<usize>(i)].view().data() ==
          pointers[static_cast<usize>(i)]);
  }
}

TEST_CASE("id_string: a string larger than one pool page is interned whole") {
  const std::string big(200 * 1024, 'B');
  const nx::id_string id{nx::string_view(big)};
  REQUIRE(id.is_resolvable());
  CHECK(id.view().size() == big.size());
  CHECK(id.view() == big);
  CHECK(id.c_str()[big.size()] == '\0');

  const nx::id_string after("after_the_big_one");
  CHECK(after.view() == "after_the_big_one");
}

TEST_CASE(
    "id_string: comparison and ordering are integer operations on the id") {
  const nx::id_string a("alpha");
  const nx::id_string b("alpha");
  const nx::id_string c("beta");

  CHECK(a == b);
  CHECK_FALSE(a != b);
  CHECK(a != c);

  const bool by_id = a.id() < c.id();
  CHECK((a < c) == by_id);
  CHECK((c < a) == !by_id);
  CHECK(a <= b);
  CHECK(a >= b);
  CHECK((a <=> b) == std::strong_ordering::equal);

  CHECK(a == nx::string_view("alpha"));
  CHECK_FALSE(a == nx::string_view("beta"));
  CHECK(nx::id_string() == nx::string_view(""));

  static_assert(nx::id_string::from_literal("x") ==
                nx::id_string::from_literal("x"));
}

TEST_CASE("id_string: lexical_less orders by the resolved text") {
  const nx::id_string a("aaa_lexical");
  const nx::id_string b("bbb_lexical");
  const nx::id_string c("ccc_lexical");

  const nx::id_string::lexical_less less;
  CHECK(less(a, b));
  CHECK(less(b, c));
  CHECK_FALSE(less(b, a));
  CHECK_FALSE(less(a, a));

  std::vector<nx::id_string> v{c, a, b};
  std::sort(v.begin(), v.end(), less);
  CHECK(v[0].view() == "aaa_lexical");
  CHECK(v[1].view() == "bbb_lexical");
  CHECK(v[2].view() == "ccc_lexical");
}

TEST_CASE("id_string: swap exchanges two ids") {
  nx::id_string a("swap_left");
  nx::id_string b("swap_right");
  const auto ia = a.id(), ib = b.id();

  a.swap(b);
  CHECK(a.id() == ib);
  CHECK(b.id() == ia);
  CHECK(a.view() == "swap_right");

  swap(a, b);
  CHECK(a.id() == ia);
  CHECK(b.id() == ib);

  a.swap(a);
  CHECK(a.id() == ia);
}

TEST_CASE("id_string: is a trivially copyable 8-byte value") {
  static_assert(sizeof(nx::id_string) == sizeof(u64));
  static_assert(std::is_trivially_copyable_v<nx::id_string>);
  static_assert(std::is_trivially_destructible_v<nx::id_string>);
  CHECK(sizeof(nx::id_string) == 8);
}

TEST_CASE("id_string: hashes and works as an unordered_map key") {
  const std::hash<nx::id_string> hasher;
  CHECK(hasher(nx::id_string("k1")) == nx::id_string("k1").id());
  CHECK(hasher(nx::id_string("k1")) != hasher(nx::id_string("k2")));

  std::unordered_map<nx::id_string, int> m;
  for (int i = 0; i < 500; ++i)
    m[nx::id_string{nx::string_view("map_key_" + std::to_string(i))}] = i;
  CHECK(m.size() == 500);
  CHECK(m[nx::id_string("map_key_37")] == 37);
  CHECK(m.find(nx::id_string("absent_key")) == m.end());
}

TEST_CASE("id_string: the _id literal interns at run time") {
  const nx::id_string s = "literal_suffix_subject"_id;
  CHECK(s.id() == nx::id_string("literal_suffix_subject").id());
  CHECK(s.is_resolvable());
  CHECK(s.view() == "literal_suffix_subject");

  CHECK(""_id == nx::id_string());
}

TEST_CASE("id_string: distinct strings do not collide across a large corpus") {
  std::set<u64> ids;
  std::vector<std::string> texts;
  for (int i = 0; i < 20000; ++i)
    texts.push_back("collision_corpus/entry_" + std::to_string(i) + ".asset");
  for (const std::string &t : texts)
    ids.insert(nx::detail::id_string_hash(nx::string_view(t)));
  CHECK(ids.size() == texts.size());
}

TEST_CASE("id_string: stats grow as strings are interned") {
  const auto before = nx::id_string_stats();

  for (int i = 0; i < 100; ++i)
    (void)nx::id_string{nx::string_view("stats_subject_" + std::to_string(i))};

  const auto after = nx::id_string_stats();
  CHECK(after.count >= before.count + 100);
  CHECK(after.bytes_used > before.bytes_used);
  CHECK(after.page_count >= 1);

  const auto mid = nx::id_string_stats();
  for (int i = 0; i < 100; ++i)
    (void)nx::id_string{nx::string_view("stats_subject_" + std::to_string(i))};
  const auto again = nx::id_string_stats();
  CHECK(again.count == mid.count);
  CHECK(again.bytes_used == mid.bytes_used);
}

TEST_CASE("id_string: concurrent interning is safe and consistent") {
  constexpr int threads = 4;
  constexpr int per_thread = 2000;

  std::vector<std::thread> workers;
  std::vector<std::vector<nx::id_string>> results(threads);

  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([t, &results] {
      auto &out = results[static_cast<usize>(t)];
      out.reserve(per_thread);
      for (int i = 0; i < per_thread; ++i) {
        const std::string text =
            "concurrent_subject_" + std::to_string((i + t * 500) % per_thread);
        out.emplace_back(nx::string_view(text));
      }
    });
  }
  for (std::thread &w : workers)
    w.join();

  for (int t = 0; t < threads; ++t) {
    for (int i = 0; i < per_thread; ++i) {
      const std::string expected =
          "concurrent_subject_" + std::to_string((i + t * 500) % per_thread);
      const nx::id_string id =
          results[static_cast<usize>(t)][static_cast<usize>(i)];
      REQUIRE(id.is_resolvable());
      REQUIRE(id.view() == expected);
    }
  }

  for (int i = 0; i < 50; ++i) {
    const std::string text = "concurrent_subject_" + std::to_string(i);
    const nx::id_string direct{nx::string_view(text)};
    CHECK(direct.view() == text);
  }
}

TEST_CASE("id_string: formats as its text, or as a hex id when unresolvable") {
  const nx::id_string named("formatted_subject");
  CHECK(std::format("{}", named) == "formatted_subject");
  CHECK(std::format("[{:>20}]", named) == "[   formatted_subject]");

  CHECK(std::format("[{}]", nx::id_string()) == "[]");

  const nx::id_string opaque = nx::id_string::from_id(0x0123456789ABCDEFull);
  CHECK(std::format("{}", opaque) == "0x0123456789abcdef");
}
