
#include "framework/nxtest.h"

#include "core/foundation/containers/string_map.h"
#include "core/foundation/strings/format.h"

namespace {

template <class Map>
concept finds_by_view = requires(Map &m, nx::string_view v) { m.find(v); };

static_assert(finds_by_view<nx::string_map<int>>,
              "string_map must look up by view without building a key");
static_assert(
    !finds_by_view<nx::hash_map<nx::string, int>>,
    "the plain map is the thing string_map exists to replace; if this "
    "starts passing, hash_map's default hasher became transparent "
    "and string_map is redundant");

}

TEST_CASE("string_map: a view, a string and a literal find the same entry") {
  nx::string_map<int> map;
  map.insert_or_assign(nx::string("/fonts/NotoSans-Regular.ttf"), 7);

  const nx::string owned("/fonts/NotoSans-Regular.ttf");
  const nx::string_view view = owned.view();

  REQUIRE(map.find(view) != map.end());
  CHECK(map.find(view)->second == 7);
  CHECK(map.find(owned)->second == 7);
  CHECK(map.find("/fonts/NotoSans-Regular.ttf")->second == 7);
}

TEST_CASE("string_map: a miss by view is a miss") {
  nx::string_map<int> map;
  map.insert_or_assign(nx::string("a"), 1);

  CHECK(map.find(nx::string_view("b")) == map.end());
  CHECK(!map.contains(nx::string_view("b")));
  CHECK(map.contains(nx::string_view("a")));
}

TEST_CASE("string_map: prefixes are distinct keys") {
  nx::string_map<int> map;
  map.insert_or_assign(nx::string("tex"), 1);
  map.insert_or_assign(nx::string("texture"), 2);
  map.insert_or_assign(nx::string("texture/a.png"), 3);

  CHECK(map.find(nx::string_view("tex"))->second == 1);
  CHECK(map.find(nx::string_view("texture"))->second == 2);
  CHECK(map.find(nx::string_view("texture/a.png"))->second == 3);
  CHECK(map.size() == 3u);
}

TEST_CASE("string_map: an empty key is a key") {
  nx::string_map<int> map;
  map.insert_or_assign(nx::string(""), 5);
  REQUIRE(map.find(nx::string_view("")) != map.end());
  CHECK(map.find(nx::string_view(""))->second == 5);
}

TEST_CASE("string_map: survives enough keys to rehash") {
  nx::string_map<u32> map;
  for (u32 i = 0; i < 500; ++i)
    map.insert_or_assign(nx::format("/asset/{}.png", i), i);

  CHECK(map.size() == 500u);
  for (u32 i = 0; i < 500; ++i) {
    const nx::string key = nx::format("/asset/{}.png", i);
    const auto it = map.find(key.view());
    REQUIRE(it != map.end());
    CHECK(it->second == i);
  }
}

TEST_CASE("string_hash: FNV-1a over the bytes, whatever the argument type") {
  const nx::string_hash hash;
  const nx::string owned("hello");

  CHECK(hash(nx::string_view("hello")) ==
        nx::cast<usize>(nx::hash_fnv1a64("hello", 5)));
  CHECK(hash(owned) == hash(nx::string_view("hello")));
  CHECK(hash("hello") == hash(nx::string_view("hello")));
  CHECK(hash(nx::string_view("hellp")) != hash(nx::string_view("hello")));
}

TEST_CASE("string_equal: compares bytes across the string types") {
  const nx::string_equal equal;
  const nx::string owned("abc");

  CHECK(equal(owned, nx::string_view("abc")));
  CHECK(equal(nx::string_view("abc"), "abc"));
  CHECK(!equal(nx::string_view("abc"), nx::string_view("abcd")));
  CHECK(equal(nx::string_view(""), nx::string_view("")));
}
