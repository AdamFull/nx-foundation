#include "framework/nxtest.h"

#include "core/foundation/containers/map.h"
#include "core/foundation/strings/utf8_string.h"

#include <array>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace {

/// The tree's links, leaf depths, node fill and ends, checked from outside.
template <typename Tree> struct probed : Tree {
  using Tree::Tree;
  probed() = default;
  explicit probed(const Tree &tree) : Tree(tree) {}

  [[nodiscard]] bool sound() const {
    if (this->m_root == nullptr)
      return this->m_size == 0 && this->m_leftmost == nullptr &&
             this->m_rightmost == nullptr;
    if (this->m_root->parent != nullptr)
      return false;
    walk w;
    if (!visit(this->m_root, 0, w))
      return false;
    return w.elements == this->m_size && w.first == this->m_leftmost &&
           w.last == this->m_rightmost;
  }

  [[nodiscard]] u32 slots() const { return Tree::SLOTS; }

private:
  using node = typename Tree::leaf_node;
  struct walk {
    usize elements = 0;
    i32 leaf_depth = -1;
    const node *first = nullptr;
    const node *last = nullptr;
  };

  bool visit(const node *n, const i32 depth, walk &w) const {
    if (n->count == 0 && (n != this->m_root || !n->leaf))
      return false;
    w.elements += n->count;
    if (n->leaf) {
      if (w.leaf_depth < 0)
        w.leaf_depth = depth;
      if (w.first == nullptr)
        w.first = n;
      w.last = n;
      return w.leaf_depth == depth;
    }
    for (u32 c = 0; c <= n->count; ++c) {
      const node *k = Tree::child(const_cast<node *>(n), c);
      if (k->parent != n || k->position != c || !visit(k, depth + 1, w))
        return false;
    }
    return true;
  }
};

/// Big enough that a node holds three: the deepest trees, and every merge
/// and borrow at the smallest fill.
struct wide {
  u32 tag = 0;
  std::array<u8, 140> pad{};
  bool operator==(const wide &) const = default;
};

struct lcg {
  u64 state;
  u32 next(const u32 bound) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<u32>(state >> 33) % bound;
  }
};

template <typename Map, typename V>
void agree_with_std(const u32 keys, const u32 steps, const u64 seed,
                    V (*value)(u32)) {
  probed<Map> map;
  std::map<u32, V> model;
  lcg rng{seed};
  for (u32 step = 0; step < steps; ++step) {
    const u32 key = rng.next(keys);
    const u32 op = rng.next(10);
    if (op < 5) {
      const bool fresh = map.try_emplace(key, value(key)).second;
      REQUIRE(fresh == model.try_emplace(key, value(key)).second);
    } else if (op < 8) {
      // Erase by iterator, which gives the element after.
      const auto at = map.find(key);
      const auto expected = model.find(key);
      REQUIRE((at == map.end()) == (expected == model.end()));
      if (at != map.end()) {
        const auto next = map.erase(at);
        const auto expected_next = model.erase(expected);
        REQUIRE((next == map.end()) == (expected_next == model.end()));
        if (next != map.end())
          REQUIRE(next->first == expected_next->first);
      }
    } else {
      REQUIRE(map.erase(key) == model.erase(key));
    }
    REQUIRE(map.size() == model.size());
    REQUIRE(map.sound());
    const auto lower = map.lower_bound(key);
    const auto expected_lower = model.lower_bound(key);
    REQUIRE((lower == map.end()) == (expected_lower == model.end()));
    if (lower != map.end())
      REQUIRE(lower->first == expected_lower->first);
    const auto upper = map.upper_bound(key);
    const auto expected_upper = model.upper_bound(key);
    REQUIRE((upper == map.end()) == (expected_upper == model.end()));
    if (upper != map.end())
      REQUIRE(upper->first == expected_upper->first);
    if (step % 64 == 0) {
      REQUIRE(std::equal(map.begin(), map.end(), model.begin(), model.end(),
                         [](const auto &a, const auto &b) {
                           return a.first == b.first && a.second == b.second;
                         }));
      REQUIRE(std::equal(
          map.rbegin(), map.rend(), model.rbegin(), model.rend(),
          [](const auto &a, const auto &b) { return a.first == b.first; }));
    }
  }
}

u32 same(const u32 key) { return key * 3u; }
wide tagged(const u32 key) { return wide{.tag = key}; }

} // namespace

TEST_CASE("map: keys of an order but no hash iterate in it, on the "
          "foundation allocator") {
  nx::map<nx::vector<u32>, u32> map;
  map[{2, 1}] = 3;
  map[{1}] = 1;
  map[{1, 5}] = 2;
  nx::vector<u32> values;
  for (const auto &[key, value] : map)
    values.push_back(value);
  CHECK(values == nx::vector<u32>{1, 2, 3});
  CHECK(map.contains({1, 5}));
  CHECK(!map.contains({5}));
  CHECK(std::is_same_v<nx::map<u32, u32>::allocator_type,
                       nx::Allocator<std::pair<const u32, u32>>>);

  nx::set<nx::vector<u32>> set{{3}, {1, 2}, {1}};
  CHECK(*set.begin() == nx::vector<u32>{1});
  CHECK(!set.insert({1, 2}).second);
  CHECK(set.size() == 3u);
  CHECK(std::is_same_v<nx::set<u32>::allocator_type, nx::Allocator<u32>>);
}

TEST_CASE("map: random inserts and erases agree with std::map at every "
          "step, through splits, merges and borrows") {
  CHECK(probed<nx::map<u32, wide>>().slots() == 3u);
  agree_with_std<nx::map<u32, u32>>(4000, 40000, 1, same);
  agree_with_std<nx::map<u32, wide>>(600, 20000, 2, tagged);
  // Few keys: the tree grows and empties again many times.
  agree_with_std<nx::map<u32, wide>>(12, 4000, 3, tagged);
}

TEST_CASE("map: runs inserted in order and in reverse, erased from the "
          "front, the back and the middle") {
  for (const u32 order : {0u, 1u, 2u}) {
    probed<nx::map<u32, wide>> map;
    constexpr u32 COUNT = 500;
    for (u32 i = 0; i < COUNT; ++i) {
      const u32 key = order == 0 ? i : COUNT - 1u - i;
      if (order == 2)
        map.emplace_hint(map.end(), i, tagged(i));
      else
        map.try_emplace(key, tagged(key));
    }
    REQUIRE(map.sound());
    REQUIRE(map.size() == COUNT);
    u32 expected = 0;
    for (const auto &[key, value] : map) {
      CHECK(key == expected);
      CHECK(value.tag == expected);
      ++expected;
    }
    if (order == 0) {
      for (u32 i = 0; i < COUNT; ++i) {
        const auto next = map.erase(map.begin());
        if (i + 1u < COUNT)
          REQUIRE(next->first == i + 1u);
        REQUIRE(map.sound());
      }
    } else if (order == 1) {
      for (u32 i = COUNT; i-- > 0;) {
        REQUIRE(map.erase(std::prev(map.end())) == map.end());
        REQUIRE(map.sound());
      }
    } else {
      for (u32 i = 0; i < COUNT; ++i) {
        const auto middle = std::next(map.begin(), map.size() / 2);
        const u32 after =
            std::next(middle) == map.end() ? ~0u : std::next(middle)->first;
        const auto next = map.erase(middle);
        REQUIRE((next == map.end() ? ~0u : next->first) == after);
        REQUIRE(map.sound());
      }
    }
    CHECK(map.empty());
    CHECK(map.begin() == map.end());
  }
}

TEST_CASE("map: bounds and ranges at present and absent keys and past "
          "both ends") {
  nx::map<u32, u32> map;
  CHECK(map.lower_bound(5) == map.end());
  CHECK(map.upper_bound(5) == map.end());
  CHECK(map.find(5) == map.end());
  for (u32 k = 10; k <= 1000; k += 10)
    map[k] = k;
  CHECK(map.lower_bound(0)->first == 10u);
  CHECK(map.lower_bound(10)->first == 10u);
  CHECK(map.lower_bound(11)->first == 20u);
  CHECK(map.lower_bound(1000)->first == 1000u);
  CHECK(map.lower_bound(1001) == map.end());
  CHECK(map.upper_bound(10)->first == 20u);
  CHECK(map.upper_bound(995)->first == 1000u);
  CHECK(map.upper_bound(1000) == map.end());
  const auto [first, last] = map.equal_range(500);
  CHECK(first->first == 500u);
  CHECK(last->first == 510u);
  const auto [none, also_none] = map.equal_range(505);
  CHECK(none == also_none);
  CHECK(none->first == 510u);
  CHECK(map.count(500) == 1u);
  CHECK(map.count(505) == 0u);

  const nx::map<u32, u32> &read = map;
  CHECK(read.lower_bound(11)->first == 20u);
  CHECK(read.find(30)->second == 30u);
  CHECK(std::prev(read.end())->first == 1000u);
}

TEST_CASE("map: erasing a range, by key and by predicate") {
  nx::map<u32, u32> map;
  for (u32 k = 0; k < 300; ++k)
    map[k] = k;
  const auto after = map.erase(map.find(100), map.find(200));
  CHECK(after->first == 200u);
  CHECK(map.size() == 200u);
  CHECK(!map.contains(150));
  CHECK(map.erase(150) == 0u);
  CHECK(map.erase(250) == 1u);
  CHECK(erase_if(map, [](const auto &e) { return e.first % 2 == 1; }) == 100u);
  CHECK(map.size() == 99u);
  for (const auto &[key, value] : map)
    CHECK(key % 2 == 0u);
  CHECK(map.erase(map.begin(), map.end()) == map.end());
  CHECK(map.empty());
}

TEST_CASE("map: a key of another type is looked up where the comparison "
          "takes it") {
  nx::map<nx::string, u32, std::less<>> map;
  map.try_emplace(nx::string("beta"), 2u);
  map.try_emplace(nx::string("alpha"), 1u);
  const nx::string_view key = "beta";
  CHECK(map.find(key)->second == 2u);
  CHECK(map.contains(nx::string_view("alpha")));
  CHECK(map.lower_bound(nx::string_view("b"))->first == "beta");
  CHECK(map.erase(nx::string_view("alpha")) == 1u);
  CHECK(map.size() == 1u);

  nx::set<nx::string, std::less<>> set{nx::string("x")};
  CHECK(set.contains(nx::string_view("x")));
}

TEST_CASE("map: values that only move; copies and moves of the whole map "
          "and their equality") {
  nx::map<u32, std::unique_ptr<u32>> owned;
  for (u32 k = 0; k < 200; ++k)
    owned.try_emplace(k, std::make_unique<u32>(k));
  for (u32 k = 0; k < 200; k += 2)
    owned.erase(k);
  for (const auto &[key, value] : owned)
    CHECK(*value == key);
  nx::map<u32, std::unique_ptr<u32>> moved = std::move(owned);
  CHECK(owned.empty());
  CHECK(moved.size() == 100u);

  nx::map<u32, wide> map;
  for (u32 k = 0; k < 100; ++k)
    map[k * 7 % 100] = tagged(k);
  const probed<nx::map<u32, wide>> copy(map);
  CHECK(copy.sound());
  CHECK(copy == map);
  nx::map<u32, wide> assigned;
  assigned[1000] = {};
  assigned = map;
  CHECK(assigned == map);
  assigned[3].tag = 999;
  CHECK(!(assigned == map));
  assigned = std::move(map);
  CHECK(assigned.size() == 100u);
}

TEST_CASE("map: an element constructor that throws leaves the map as it "
          "was") {
  struct fragile {
    u32 v;
    explicit fragile(const u32 x) : v(x) {
      if (x == 13)
        throw std::runtime_error("fragile");
    }
    fragile(fragile &&) noexcept = default;
  };
  probed<nx::map<u32, fragile>> map;
  for (u32 k = 0; k < 100; ++k)
    if (k != 13)
      map.try_emplace(k, k);
  CHECK_THROWS(map.try_emplace(13, 13u));
  CHECK(map.size() == 99u);
  CHECK(map.sound());
  CHECK(!map.contains(13));
}

TEST_CASE("map: try_emplace leaves its arguments alone when the key is "
          "there; insert_or_assign, operator[] and at") {
  nx::map<u32, std::unique_ptr<u32>> map;
  auto first = std::make_unique<u32>(1);
  CHECK(map.try_emplace(1, std::move(first)).second);
  auto second = std::make_unique<u32>(2);
  CHECK(!map.try_emplace(1, std::move(second)).second);
  CHECK(second != nullptr);
  CHECK(*map.at(1) == 1u);
  CHECK(!map.insert_or_assign(1, std::move(second)).second);
  CHECK(*map.at(1) == 2u);
  CHECK(map.insert_or_assign(2, std::make_unique<u32>(3)).second);
  CHECK(map[3] == nullptr);
  CHECK(map.size() == 3u);
  CHECK_THROWS_AS(map.at(4), std::out_of_range);
}

TEST_CASE("map: a hint where the key belongs inserts there; a wrong one "
          "still inserts in order") {
  probed<nx::map<u32, wide>> map;
  for (u32 k = 0; k < 100; k += 2)
    map.emplace_hint(map.end(), k, tagged(k));
  // Right: just before the next key.
  map.emplace_hint(map.find(50), 49u, tagged(49));
  // Wrong: far from where it belongs.
  map.emplace_hint(map.begin(), 77u, tagged(77));
  map.emplace_hint(map.end(), 1u, tagged(1));
  CHECK(map.insert(map.find(10), {10u, tagged(0)})->second.tag == 10u);
  CHECK(map.sound());
  CHECK(map.size() == 53u);
  CHECK(
      std::is_sorted(map.begin(), map.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
      }));
}

TEST_CASE("map: a set's elements are keys and never mutable") {
  using set = nx::set<u32>;
  static_assert(
      std::is_same_v<decltype(*std::declval<set::iterator>()), const u32 &>);
  probed<nx::set<std::array<u32, 36>>> wide_set;
  CHECK(wide_set.slots() == 3u);
  set s{5, 1, 3};
  CHECK(!s.insert(3).second);
  CHECK(s.emplace(4).second);
  s.insert({9, 7});
  CHECK(nx::vector<u32>(s.begin(), s.end()) ==
        nx::vector<u32>{1, 3, 4, 5, 7, 9});
  CHECK(nx::vector<u32>(s.rbegin(), s.rend()) ==
        nx::vector<u32>{9, 7, 5, 4, 3, 1});
  CHECK(*s.lower_bound(6) == 7u);
  CHECK(s.erase(5) == 1u);
  CHECK(s.count(5) == 0u);
}
