#include "framework/nxtest.h"

#include "core/foundation/ecs/registry.h"

#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ecs = nx::ecs;

namespace {

struct Position {
  float x = 0, y = 0;
  bool operator==(const Position &o) const { return x == o.x && y == o.y; }
};
struct Velocity {
  float dx = 0, dy = 0;
};
struct Name {
  std::string value;
};
struct Health {
  int hp = 100;
};
struct Tag {};
struct Frozen {};
struct Hidden {};

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

struct Unique {
  std::unique_ptr<int> p;
  Unique() = default;
  explicit Unique(int v) : p(std::make_unique<int>(v)) {}
};

using reg_t = ecs::registry;
using entity_t = reg_t::entity_type;

struct fixture {
  reg_t reg;
  fixture() {
    reg.register_component<Position>({.name = "Position"});
    reg.register_component<Velocity>({.name = "Velocity"});
    reg.register_component<Name>({.name = "Name"});
    reg.register_component<Health>({.name = "Health"});
    reg.register_component<Tag>({.name = "Tag"});
    reg.register_component<Frozen>({.name = "Frozen"});
    reg.register_component<Hidden>({.name = "Hidden"});
  }
};

std::vector<entity_t> sorted(std::vector<entity_t> v) {
  std::sort(v.begin(), v.end(),
            [](entity_t a, entity_t b) { return a.raw() < b.raw(); });
  return v;
}

}

TEST_CASE("ecs: a fresh registry is empty") {
  const reg_t reg;
  CHECK(reg.empty());
  CHECK(reg.size() == 0);
  CHECK_FALSE(reg.alive(reg_t::null_entity));
  CHECK_FALSE(reg.alive(entity_t{}));

  const ecs::registry_statistics st = reg.statistics();
  CHECK(st.entity_count == 0);
  CHECK(st.entity_slot_count == 0);
  CHECK(st.relation_count == 0);
}

TEST_CASE("ecs: create yields distinct live entities") {
  reg_t reg;
  const entity_t a = reg.create();
  const entity_t b = reg.create();
  const entity_t c = reg.create();

  CHECK(reg.size() == 3);
  CHECK_FALSE(reg.empty());
  CHECK(reg.alive(a));
  CHECK(reg.alive(b));
  CHECK(reg.alive(c));
  CHECK(a != b);
  CHECK(b != c);
  CHECK(a.index() != b.index());

  CHECK_FALSE(reg.alive(entity_t::from_parts(9999, 0)));
  CHECK_FALSE(reg.alive(entity_t{}));
}

TEST_CASE("ecs: destroy retires the handle and recycles the slot") {
  reg_t reg;
  const entity_t a = reg.create();
  const entity_t b = reg.create();

  reg.destroy(a);
  CHECK(reg.size() == 1);
  CHECK_FALSE(reg.alive(a));
  CHECK(reg.alive(b));

  reg.destroy(a);
  reg.destroy(entity_t{});
  reg.destroy(entity_t::from_parts(5000, 0));
  CHECK(reg.size() == 1);

  const entity_t c = reg.create();
  CHECK(c.index() == a.index());
  CHECK(c.version() != a.version());
  CHECK(reg.alive(c));
  CHECK_FALSE(reg.alive(a));
  CHECK(reg.size() == 2);
}

TEST_CASE("ecs: create/destroy churn keeps alive handles resolvable") {
  reg_t reg;
  std::vector<entity_t> live;

  u64 state = 0x5EED1234ABCDEF01ull;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  for (int step = 0; step < 20000; ++step) {
    const u64 r = next();
    if (live.empty() || r % 3 != 0) {
      live.push_back(reg.create());
    } else {
      const usize pick = static_cast<usize>(r >> 8) % live.size();
      const entity_t dead = live[pick];
      reg.destroy(dead);
      REQUIRE_FALSE(reg.alive(dead));
      live[pick] = live.back();
      live.pop_back();
    }
    REQUIRE(reg.size() == live.size());
  }

  for (const entity_t e : live)
    REQUIRE(reg.alive(e));

  std::set<u32> raws;
  for (const entity_t e : live)
    raws.insert(e.raw());
  CHECK(raws.size() == live.size());
}

TEST_CASE("ecs: clear removes every entity but keeps registrations") {
  fixture f;
  const entity_t a = f.reg.create();
  const entity_t b = f.reg.create();
  f.reg.emplace<Position>(a, Position{1, 2});
  f.reg.emplace<Position>(b, Position{3, 4});
  REQUIRE(f.reg.set_parent(b, a));

  f.reg.clear();

  CHECK(f.reg.empty());
  CHECK(f.reg.size() == 0);
  CHECK_FALSE(f.reg.alive(a));
  CHECK_FALSE(f.reg.alive(b));
  CHECK(f.reg.statistics().relation_count == 0);

  CHECK(f.reg.is_component_registered<Position>());
  const entity_t c = f.reg.create();
  CHECK(f.reg.alive(c));
  CHECK_FALSE(f.reg.alive(a));
  CHECK_FALSE(f.reg.alive(b));
  f.reg.emplace<Position>(c, Position{5, 6});
  CHECK(f.reg.get<Position>(c).x == 5);

  int seen = 0;
  f.reg.view<Position>().each([&](entity_t, Position &) { ++seen; });
  CHECK(seen == 1);
}

TEST_CASE("ecs: reserve does not disturb existing entities") {
  reg_t reg;
  const entity_t a = reg.create();
  reg.reserve(10000);
  CHECK(reg.alive(a));
  CHECK(reg.size() == 1);
  for (int i = 0; i < 1000; ++i)
    (void)reg.create();
  CHECK(reg.alive(a));
  CHECK(reg.size() == 1001);
}

TEST_CASE("ecs: register_component records metadata and derived flags") {
  reg_t reg;
  CHECK_FALSE(reg.is_component_registered<Position>());

  const ecs::component_metadata &md =
      reg.register_component<Position>({.name = "Position"});
  CHECK(reg.is_component_registered<Position>());
  CHECK(md.name == "Position");
  CHECK(md.size == sizeof(Position));
  CHECK(md.alignment == alignof(Position));
  CHECK(md.storage != nullptr);
  CHECK_FALSE(ecs::has_flag(md.flags, ecs::component_flags::empty));
  CHECK_FALSE(ecs::has_flag(md.flags, ecs::component_flags::move_only));

  CHECK(md.lifecycle.default_construct != nullptr);
  CHECK(md.lifecycle.destroy != nullptr);
  CHECK(md.lifecycle.move_construct != nullptr);
  CHECK(md.lifecycle.copy_construct != nullptr);

  CHECK(&reg.component_metadata_of<Position>() == &md);
  CHECK(reg.component_metadata_of<Position>().id == md.id);

  const auto &tag = reg.register_component<Tag>({.name = "Tag"});
  CHECK(ecs::has_flag(tag.flags, ecs::component_flags::empty));
  CHECK(tag.size == sizeof(Tag));

  const auto &uniq = reg.register_component<Unique>({.name = "Unique"});
  CHECK(ecs::has_flag(uniq.flags, ecs::component_flags::move_only));
  CHECK(uniq.lifecycle.copy_construct == nullptr);
  CHECK(uniq.lifecycle.move_construct != nullptr);

  const auto &named = reg.register_component<Health>(
      {.name = "Health", .flags = ecs::component_flags::move_only});
  CHECK(ecs::has_flag(named.flags, ecs::component_flags::move_only));

  CHECK_FALSE(reg.is_component_registered<Velocity>());
}

TEST_CASE("ecs: component_flags compose") {
  const ecs::component_flags both =
      ecs::component_flags::empty | ecs::component_flags::move_only;
  CHECK(ecs::has_flag(both, ecs::component_flags::empty));
  CHECK(ecs::has_flag(both, ecs::component_flags::move_only));
  CHECK_FALSE(
      ecs::has_flag(ecs::component_flags::none, ecs::component_flags::empty));
  CHECK_FALSE(ecs::has_flag(ecs::component_flags::empty,
                            ecs::component_flags::move_only));
  static_assert(ecs::has_flag(ecs::component_flags::empty |
                                  ecs::component_flags::move_only,
                              ecs::component_flags::empty));
}

TEST_CASE("ecs: component ids are stable per type and unique across types") {
  reg_t a, b;
  a.register_component<Position>();
  a.register_component<Velocity>();
  b.register_component<Position>();
  b.register_component<Velocity>();

  CHECK(a.component_metadata_of<Position>().id ==
        b.component_metadata_of<Position>().id);
  CHECK(a.component_metadata_of<Velocity>().id ==
        b.component_metadata_of<Velocity>().id);
  CHECK(a.component_metadata_of<Position>().id !=
        a.component_metadata_of<Velocity>().id);
}

TEST_CASE("ecs: emplace constructs a component and get returns it") {
  fixture f;
  const entity_t e = f.reg.create();

  Position &p = f.reg.emplace<Position>(e, Position{1, 2});
  CHECK(p.x == 1);
  CHECK(p.y == 2);
  CHECK(f.reg.has<Position>(e));
  CHECK(f.reg.get<Position>(e).x == 1);

  p.x = 10;
  CHECK(f.reg.get<Position>(e).x == 10);

  f.reg.emplace<Name>(e, Name{"hello"});
  CHECK(f.reg.get<Name>(e).value == "hello");

  f.reg.emplace<Health>(e);
  CHECK(f.reg.get<Health>(e).hp == 100);

  f.reg.emplace<Tag>(e);
  CHECK(f.reg.has<Tag>(e));

  const reg_t &cr = f.reg;
  CHECK(cr.get<Position>(e).x == 10);
}

TEST_CASE("ecs: get with several types returns a tuple") {
  fixture f;
  const entity_t e = f.reg.create();
  f.reg.emplace<Position>(e, Position{1, 2});
  f.reg.emplace<Velocity>(e, Velocity{3, 4});

  auto [p, v] = f.reg.get<Position, Velocity>(e);
  CHECK(p.x == 1);
  CHECK(v.dx == 3);
  p.x = 9;
  CHECK(f.reg.get<Position>(e).x == 9);

  const reg_t &cr = f.reg;
  auto [cp, cv] = cr.get<Position, Velocity>(e);
  CHECK(cp.x == 9);
  CHECK(cv.dy == 4);
}

TEST_CASE("ecs: try_get and has are safe for absent, stale and unregistered") {
  fixture f;
  const entity_t e = f.reg.create();
  f.reg.emplace<Position>(e, Position{1, 2});

  REQUIRE(f.reg.try_get<Position>(e) != nullptr);
  CHECK(f.reg.try_get<Position>(e)->x == 1);
  CHECK(f.reg.try_get<Velocity>(e) == nullptr);

  const entity_t other = f.reg.create();
  CHECK(f.reg.try_get<Position>(other) == nullptr);

  f.reg.destroy(e);
  CHECK(f.reg.try_get<Position>(e) == nullptr);
  CHECK_FALSE(f.reg.has<Position>(e));

  reg_t bare;
  const entity_t x = bare.create();
  CHECK(bare.try_get<Position>(x) == nullptr);
  CHECK_FALSE(bare.has<Position>(x));

  const reg_t &cr = f.reg;
  CHECK(cr.try_get<Position>(e) == nullptr);
}

TEST_CASE("ecs: all_of and any_of combine the presence checks") {
  fixture f;
  const entity_t e = f.reg.create();
  f.reg.emplace<Position>(e, Position{});
  f.reg.emplace<Velocity>(e, Velocity{});

  CHECK(f.reg.all_of<Position>(e));
  CHECK(f.reg.all_of<Position, Velocity>(e));
  CHECK_FALSE(f.reg.all_of<Position, Velocity, Health>(e));

  CHECK(f.reg.any_of<Position>(e));
  CHECK(f.reg.any_of<Health, Position>(e));
  CHECK_FALSE(f.reg.any_of<Health, Name>(e));

  const entity_t bare = f.reg.create();
  CHECK_FALSE(f.reg.all_of<Position>(bare));
  CHECK_FALSE(f.reg.any_of<Position, Velocity>(bare));
}

TEST_CASE("ecs: emplace_or_replace adds then overwrites") {
  fixture f;
  const entity_t e = f.reg.create();

  Position &first = f.reg.emplace_or_replace<Position>(e, Position{1, 1});
  CHECK(first.x == 1);
  CHECK(f.reg.has<Position>(e));

  Position &second = f.reg.emplace_or_replace<Position>(e, Position{2, 2});
  CHECK(second.x == 2);
  CHECK(f.reg.get<Position>(e).x == 2);

  f.reg.insert_or_assign<Position>(e, Position{3, 3});
  CHECK(f.reg.get<Position>(e).x == 3);

  usize count = 0;
  f.reg.view<Position>().each([&](entity_t, Position &) { ++count; });
  CHECK(count == 1);
}

TEST_CASE("ecs: try_emplace_or_replace asks rather than asserts") {
  fixture f;
  const entity_t live = f.reg.create();

  Position *added =
      f.reg.try_emplace_or_replace<Position>(live, Position{1, 1});
  REQUIRE(added != nullptr);
  CHECK(added->x == 1);
  CHECK(f.reg.try_emplace_or_replace<Position>(live, Position{2, 2})->x == 2);
  CHECK(f.reg.get<Position>(live).x == 2);

  const entity_t stale = f.reg.create();
  f.reg.destroy(stale);
  CHECK(f.reg.try_emplace_or_replace<Position>(stale, Position{9, 9}) ==
        nullptr);

  usize count = 0;
  f.reg.view<Position>().each([&](entity_t, Position &) { ++count; });
  CHECK(count == 1);
}

TEST_CASE("ecs: try_emplace_or_replace declines a recycled handle") {
  fixture f;
  const entity_t old = f.reg.create();
  f.reg.destroy(old);
  const entity_t fresh = f.reg.create();
  CHECK(f.reg.try_emplace_or_replace<Position>(old, Position{5, 5}) == nullptr);
  CHECK_FALSE(f.reg.has<Position>(fresh));
}

TEST_CASE("ecs: replace and patch mutate an existing component") {
  fixture f;
  const entity_t e = f.reg.create();
  f.reg.emplace<Position>(e, Position{1, 1});

  Position &r = f.reg.replace<Position>(e, Position{5, 6});
  CHECK(r.x == 5);
  CHECK(f.reg.get<Position>(e).y == 6);

  Position &p = f.reg.patch<Position>(e, [](Position &pos) {
    pos.x += 10;
    pos.y += 20;
  });
  CHECK(p.x == 15);
  CHECK(f.reg.get<Position>(e).y == 26);
}

TEST_CASE("ecs: remove and erase drop a component") {
  fixture f;
  const entity_t e = f.reg.create();
  f.reg.emplace<Position>(e, Position{});
  f.reg.emplace<Velocity>(e, Velocity{});

  CHECK(f.reg.remove<Position>(e));
  CHECK_FALSE(f.reg.has<Position>(e));
  CHECK(f.reg.has<Velocity>(e));

  CHECK_FALSE(f.reg.remove<Position>(e));
  reg_t bare;
  CHECK_FALSE(bare.remove<Position>(bare.create()));

  const entity_t dead = f.reg.create();
  f.reg.destroy(dead);
  CHECK_FALSE(f.reg.remove<Position>(dead));

  f.reg.erase<Velocity>(e);
  CHECK_FALSE(f.reg.has<Velocity>(e));
}

TEST_CASE("ecs: remove_all strips every component from one entity") {
  fixture f;
  const entity_t a = f.reg.create();
  const entity_t b = f.reg.create();
  for (const entity_t e : {a, b}) {
    f.reg.emplace<Position>(e, Position{});
    f.reg.emplace<Velocity>(e, Velocity{});
    f.reg.emplace<Tag>(e);
  }

  f.reg.remove_all(a);
  CHECK_FALSE(f.reg.has<Position>(a));
  CHECK_FALSE(f.reg.has<Velocity>(a));
  CHECK_FALSE(f.reg.has<Tag>(a));
  CHECK(f.reg.alive(a));

  CHECK(f.reg.has<Position>(b));
  CHECK(f.reg.has<Velocity>(b));
  CHECK(f.reg.has<Tag>(b));
}

TEST_CASE("ecs: clear<T> removes every component of one type") {
  fixture f;
  std::vector<entity_t> entities;
  for (int i = 0; i < 50; ++i) {
    const entity_t e = f.reg.create();
    f.reg.emplace<Position>(e, Position{});
    f.reg.emplace<Velocity>(e, Velocity{});
    entities.push_back(e);
  }

  f.reg.clear<Position>();
  for (const entity_t e : entities) {
    REQUIRE_FALSE(f.reg.has<Position>(e));
    REQUIRE(f.reg.has<Velocity>(e));
    REQUIRE(f.reg.alive(e));
  }

  reg_t bare;
  bare.clear<Position>();
  CHECK(bare.empty());
}

TEST_CASE("ecs: destroying an entity releases its components") {
  fixture f;
  f.reg.register_component<Counted>({.name = "Counted"});
  g_live = 0;

  std::vector<entity_t> entities;
  for (int i = 0; i < 100; ++i) {
    const entity_t e = f.reg.create();
    f.reg.emplace<Counted>(e, i);
    entities.push_back(e);
  }
  CHECK(g_live == 100);

  f.reg.destroy(entities[0]);
  CHECK(g_live == 99);

  for (usize i = 1; i < entities.size(); ++i)
    f.reg.destroy(entities[i]);
  CHECK(g_live == 0);
  CHECK(f.reg.empty());
}

TEST_CASE("ecs: the registry destructor releases every component") {
  g_live = 0;
  {
    reg_t reg;
    reg.register_component<Counted>({.name = "Counted"});
    for (int i = 0; i < 500; ++i)
      reg.emplace<Counted>(reg.create(), i);
    CHECK(g_live == 500);
  }
  CHECK(g_live == 0);
}

TEST_CASE("ecs: clear() releases components too") {
  g_live = 0;
  reg_t reg;
  reg.register_component<Counted>({.name = "Counted"});
  for (int i = 0; i < 200; ++i)
    reg.emplace<Counted>(reg.create(), i);
  CHECK(g_live == 200);
  reg.clear();
  CHECK(g_live == 0);
}

TEST_CASE("ecs: release_components drops storage and permits registration again") {
  g_live = 0;
  reg_t reg;
  reg.register_component<Counted>({.name = "Counted"});
  reg.emplace<Counted>(reg.create(), 7);
  CHECK(g_live == 1);

  reg.release_components();

  CHECK(g_live == 0);
  CHECK(reg.empty());
  CHECK_FALSE(reg.is_component_registered<Counted>());
  CHECK(reg.statistics().component_type_count == 0u);

  reg.register_component<Counted>({.name = "Counted"});
  reg.emplace<Counted>(reg.create(), 9);
  CHECK(g_live == 1);
  reg.clear();
  CHECK(g_live == 0);
}

TEST_CASE("ecs: move-only components are supported") {
  reg_t reg;
  reg.register_component<Unique>({.name = "Unique"});

  const entity_t a = reg.create();
  const entity_t b = reg.create();
  reg.emplace<Unique>(a, 11);
  reg.emplace<Unique>(b, 22);

  REQUIRE(reg.get<Unique>(a).p != nullptr);
  CHECK(*reg.get<Unique>(a).p == 11);
  CHECK(*reg.get<Unique>(b).p == 22);

  CHECK(reg.remove<Unique>(a));
  CHECK_FALSE(reg.has<Unique>(a));
  REQUIRE(reg.has<Unique>(b));
  CHECK(*reg.get<Unique>(b).p == 22);
}

TEST_CASE("ecs: many components on many entities stay correctly associated") {
  fixture f;
  constexpr int n = 2000;
  std::vector<entity_t> entities;
  for (int i = 0; i < n; ++i) {
    const entity_t e = f.reg.create();
    f.reg.emplace<Position>(e, Position{static_cast<float>(i), 0});
    if (i % 2 == 0)
      f.reg.emplace<Velocity>(e, Velocity{static_cast<float>(i), 0});
    if (i % 3 == 0)
      f.reg.emplace<Health>(e, Health{i});
    entities.push_back(e);
  }

  for (int i = 0; i < n; ++i) {
    const entity_t e = entities[static_cast<usize>(i)];
    REQUIRE(f.reg.get<Position>(e).x == static_cast<float>(i));
    REQUIRE(f.reg.has<Velocity>(e) == (i % 2 == 0));
    REQUIRE(f.reg.has<Health>(e) == (i % 3 == 0));
    if (i % 3 == 0)
      REQUIRE(f.reg.get<Health>(e).hp == i);
  }

  for (int i = 0; i < n; i += 2)
    REQUIRE(f.reg.remove<Position>(entities[static_cast<usize>(i)]));
  for (int i = 0; i < n; ++i) {
    const entity_t e = entities[static_cast<usize>(i)];
    REQUIRE(f.reg.has<Position>(e) == (i % 2 == 1));
    if (i % 2 == 1)
      REQUIRE(f.reg.get<Position>(e).x == static_cast<float>(i));
  }
}

TEST_CASE("ecs: on_construct fires after a component is added") {
  fixture f;
  std::vector<entity_t> seen;
  f.reg.on_construct<Position>([&](reg_t &r, entity_t e) {
    CHECK(r.has<Position>(e));
    seen.push_back(e);
  });

  const entity_t a = f.reg.create();
  const entity_t b = f.reg.create();
  f.reg.emplace<Position>(a, Position{});
  f.reg.emplace<Position>(b, Position{});
  REQUIRE(seen.size() == 2);
  CHECK(seen[0] == a);
  CHECK(seen[1] == b);

  f.reg.emplace_or_replace<Position>(a, Position{1, 1});
  CHECK(seen.size() == 2);
  const entity_t c = f.reg.create();
  f.reg.emplace_or_replace<Position>(c, Position{});
  CHECK(seen.size() == 3);
}

TEST_CASE("ecs: on_destroy fires before a component is removed") {
  fixture f;
  std::vector<entity_t> seen;
  f.reg.on_destroy<Position>([&](reg_t &r, entity_t e) {
    CHECK(r.has<Position>(e));
    seen.push_back(e);
  });

  const entity_t a = f.reg.create();
  const entity_t b = f.reg.create();
  f.reg.emplace<Position>(a, Position{});
  f.reg.emplace<Position>(b, Position{});
  CHECK(seen.empty());

  f.reg.remove<Position>(a);
  REQUIRE(seen.size() == 1);
  CHECK(seen[0] == a);

  f.reg.destroy(b);
  REQUIRE(seen.size() == 2);
  CHECK(seen[1] == b);

  const entity_t c = f.reg.create();
  const entity_t d = f.reg.create();
  f.reg.emplace<Position>(c, Position{});
  f.reg.emplace<Position>(d, Position{});
  f.reg.clear<Position>();
  CHECK(seen.size() == 4);
}

TEST_CASE("ecs: on_update fires for replace, patch and an overwriting insert") {
  fixture f;
  int updates = 0;
  f.reg.on_update<Position>([&](reg_t &, entity_t) { ++updates; });

  const entity_t e = f.reg.create();
  f.reg.emplace<Position>(e, Position{});
  CHECK(updates == 0);

  f.reg.replace<Position>(e, Position{1, 1});
  CHECK(updates == 1);

  f.reg.patch<Position>(e, [](Position &p) { p.x = 2; });
  CHECK(updates == 2);

  f.reg.emplace_or_replace<Position>(e, Position{3, 3});
  CHECK(updates == 3);
}

TEST_CASE("ecs: several callbacks per event all fire, in order") {
  fixture f;
  std::vector<int> order;
  f.reg.on_construct<Position>([&](reg_t &, entity_t) { order.push_back(1); });
  f.reg.on_construct<Position>([&](reg_t &, entity_t) { order.push_back(2); });
  f.reg.on_construct<Position>([&](reg_t &, entity_t) { order.push_back(3); });

  f.reg.emplace<Position>(f.reg.create(), Position{});
  CHECK(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("ecs: hooks on one type do not fire for another") {
  fixture f;
  int pos_hooks = 0;
  f.reg.on_construct<Position>([&](reg_t &, entity_t) { ++pos_hooks; });

  const entity_t e = f.reg.create();
  f.reg.emplace<Velocity>(e, Velocity{});
  f.reg.emplace<Health>(e, Health{});
  CHECK(pos_hooks == 0);
  f.reg.emplace<Position>(e, Position{});
  CHECK(pos_hooks == 1);
}

TEST_CASE("ecs: a single-component view visits every holder") {
  fixture f;
  std::vector<entity_t> with_position;
  for (int i = 0; i < 20; ++i) {
    const entity_t e = f.reg.create();
    if (i % 2 == 0) {
      f.reg.emplace<Position>(e, Position{static_cast<float>(i), 0});
      with_position.push_back(e);
    }
  }

  std::vector<entity_t> seen;
  f.reg.view<Position>().each([&](entity_t e, Position &p) {
    CHECK(p.x == static_cast<float>(e.index() * 0 + p.x));
    seen.push_back(e);
  });
  CHECK(sorted(seen) == sorted(with_position));

  f.reg.view<Position>().each([](entity_t, Position &p) { p.y = 7; });
  for (const entity_t e : with_position)
    CHECK(f.reg.get<Position>(e).y == 7);
}

TEST_CASE("ecs: a multi-component view visits only the intersection") {
  fixture f;
  std::vector<entity_t> both;
  for (int i = 0; i < 60; ++i) {
    const entity_t e = f.reg.create();
    const bool p = i % 2 == 0;
    const bool v = i % 3 == 0;
    if (p)
      f.reg.emplace<Position>(e, Position{static_cast<float>(i), 0});
    if (v)
      f.reg.emplace<Velocity>(e, Velocity{static_cast<float>(i), 0});
    if (p && v)
      both.push_back(e);
  }

  std::vector<entity_t> seen;
  f.reg.view<Position, Velocity>().each(
      [&](entity_t e, Position &p, Velocity &v) {
        CHECK(p.x == v.dx);
        seen.push_back(e);
      });
  CHECK(sorted(seen) == sorted(both));

  std::vector<entity_t> reversed;
  f.reg.view<Velocity, Position>().each(
      [&](entity_t e, Velocity &, Position &) { reversed.push_back(e); });
  CHECK(sorted(reversed) == sorted(both));

  const entity_t all = f.reg.create();
  f.reg.emplace<Position>(all, Position{});
  f.reg.emplace<Velocity>(all, Velocity{});
  f.reg.emplace<Health>(all, Health{});
  int triples = 0;
  f.reg.view<Position, Velocity, Health>().each(
      [&](entity_t e, Position &, Velocity &, Health &) {
        CHECK(e == all);
        ++triples;
      });
  CHECK(triples == 1);
}

TEST_CASE("ecs: an empty view iterates nothing") {
  fixture f;
  int visits = 0;
  f.reg.view<Position>().each([&](entity_t, Position &) { ++visits; });
  CHECK(visits == 0);
  CHECK(f.reg.view<Position>().empty());
  CHECK(f.reg.view<Position>().size_hint() == 0);

  (void)f.reg.create();
  (void)f.reg.create();
  f.reg.view<Position>().each([&](entity_t, Position &) { ++visits; });
  CHECK(visits == 0);

  const entity_t e = f.reg.create();
  f.reg.emplace<Position>(e, Position{});
  f.reg.view<Position, Velocity>().each(
      [&](entity_t, Position &, Velocity &) { ++visits; });
  CHECK(visits == 0);
}

TEST_CASE("ecs: size_hint is the smallest storage and bounds the matches") {
  fixture f;
  for (int i = 0; i < 100; ++i) {
    const entity_t e = f.reg.create();
    f.reg.emplace<Position>(e, Position{});
    if (i < 10)
      f.reg.emplace<Velocity>(e, Velocity{});
  }

  CHECK(f.reg.view<Position>().size_hint() == 100);
  CHECK(f.reg.view<Velocity>().size_hint() == 10);
  CHECK(f.reg.view<Position, Velocity>().size_hint() == 10);
  CHECK(f.reg.view<Velocity, Position>().size_hint() == 10);
  CHECK_FALSE(f.reg.view<Position>().empty());

  usize matched = 0;
  f.reg.view<Position, Velocity>().each(
      [&](entity_t, Position &, Velocity &) { ++matched; });
  CHECK(matched == 10);
  CHECK(matched <= f.reg.view<Position, Velocity>().size_hint());
}

TEST_CASE("ecs: view exclude narrows the matched set") {
  fixture f;
  std::vector<entity_t> expected;
  for (int i = 0; i < 40; ++i) {
    const entity_t e = f.reg.create();
    f.reg.emplace<Position>(e, Position{});
    if (i % 4 == 0)
      f.reg.emplace<Frozen>(e);
    else
      expected.push_back(e);
  }

  std::vector<entity_t> seen;
  f.reg.view<Position>().exclude<Frozen>(f.reg).each(
      [&](entity_t e, Position &) { seen.push_back(e); });
  CHECK(sorted(seen) == sorted(expected));

  const entity_t hidden = f.reg.create();
  f.reg.emplace<Position>(hidden, Position{});
  f.reg.emplace<Hidden>(hidden);

  usize count = 0;
  f.reg.view<Position>().exclude<Frozen, Hidden>(f.reg).each(
      [&](entity_t e, Position &) {
        CHECK(e != hidden);
        ++count;
      });
  CHECK(count == expected.size());

  usize all = 0;
  f.reg.view<Position>().exclude<Name>(f.reg).each(
      [&](entity_t, Position &) { ++all; });
  CHECK(all == 41);
}

TEST_CASE("ecs: a const registry yields a const view") {
  fixture f;
  const entity_t e = f.reg.create();
  f.reg.emplace<Position>(e, Position{1, 2});
  f.reg.emplace<Velocity>(e, Velocity{3, 4});

  const reg_t &cr = f.reg;
  int visits = 0;
  cr.view<Position, Velocity>().each(
      [&](entity_t ent, const Position &p, const Velocity &v) {
        CHECK(ent == e);
        CHECK(p.x == 1);
        CHECK(v.dy == 4);
        ++visits;
      });
  CHECK(visits == 1);
}

TEST_CASE("ecs: a view over an empty (tag) component works") {
  fixture f;
  std::vector<entity_t> tagged;
  for (int i = 0; i < 20; ++i) {
    const entity_t e = f.reg.create();
    f.reg.emplace<Position>(e, Position{});
    if (i % 5 == 0) {
      f.reg.emplace<Tag>(e);
      tagged.push_back(e);
    }
  }

  std::vector<entity_t> seen;
  f.reg.view<Tag>().each([&](entity_t e, Tag &) { seen.push_back(e); });
  CHECK(sorted(seen) == sorted(tagged));

  std::vector<entity_t> pairs;
  f.reg.view<Position, Tag>().each(
      [&](entity_t e, Position &, Tag &) { pairs.push_back(e); });
  CHECK(sorted(pairs) == sorted(tagged));
}

TEST_CASE("ecs: a cached query matches the same set as a view") {
  fixture f;
  std::vector<entity_t> both;
  for (int i = 0; i < 50; ++i) {
    const entity_t e = f.reg.create();
    if (i % 2 == 0)
      f.reg.emplace<Position>(e, Position{static_cast<float>(i), 0});
    if (i % 3 == 0)
      f.reg.emplace<Velocity>(e, Velocity{});
    if (i % 2 == 0 && i % 3 == 0)
      both.push_back(e);
  }

  auto q = f.reg.query<Position, Velocity>();
  CHECK(q.size() == both.size());
  CHECK_FALSE(q.empty());

  std::vector<entity_t> from_span(q.entities().begin(), q.entities().end());
  CHECK(sorted(from_span) == sorted(both));

  std::vector<entity_t> from_each;
  q.each([&](entity_t e, Position &, Velocity &) { from_each.push_back(e); });
  CHECK(sorted(from_each) == sorted(both));
}

TEST_CASE("ecs: a cached query rebuilds when a tracked type changes") {
  fixture f;
  auto q = f.reg.query<Position>();
  CHECK(q.size() == 0);
  CHECK(q.empty());

  const entity_t a = f.reg.create();
  f.reg.emplace<Position>(a, Position{});
  CHECK(q.size() == 1);

  const entity_t b = f.reg.create();
  f.reg.emplace<Position>(b, Position{});
  CHECK(q.size() == 2);

  CHECK(f.reg.remove<Position>(a));
  CHECK(q.size() == 1);
  CHECK(q.entities()[0] == b);

  f.reg.destroy(b);
  CHECK(q.size() == 0);

  const entity_t c = f.reg.create();
  f.reg.emplace<Position>(c, Position{});
  CHECK(q.size() == 1);
  f.reg.clear();
  CHECK(q.size() == 0);
}

TEST_CASE("ecs: a cached query is reused for the same type set") {
  fixture f;
  for (int i = 0; i < 10; ++i) {
    const entity_t e = f.reg.create();
    f.reg.emplace<Position>(e, Position{});
    f.reg.emplace<Velocity>(e, Velocity{});
  }

  const usize before = f.reg.statistics().cached_query_count;
  auto a = f.reg.query<Position, Velocity>();
  const usize after_first = f.reg.statistics().cached_query_count;
  CHECK(after_first == before + 1);

  auto b = f.reg.query<Position, Velocity>();
  auto c = f.reg.query<Velocity, Position>();
  CHECK(f.reg.statistics().cached_query_count == after_first);
  CHECK(a.size() == 10);
  CHECK(b.size() == 10);
  CHECK(c.size() == 10);

  auto d = f.reg.query<Position>();
  CHECK(f.reg.statistics().cached_query_count == after_first + 1);
  CHECK(d.size() == 10);
}

TEST_CASE("ecs: hierarchy changes do not invalidate a cached query") {
  fixture f;
  const entity_t a = f.reg.create();
  const entity_t b = f.reg.create();
  f.reg.emplace<Position>(a, Position{});
  f.reg.emplace<Position>(b, Position{});

  auto q = f.reg.query<Position>();
  CHECK(q.size() == 2);

  REQUIRE(f.reg.set_parent(b, a));
  CHECK(q.size() == 2);
  f.reg.remove_parent(b);
  CHECK(q.size() == 2);
}

TEST_CASE("ecs: a cached query over an untouched type stays empty") {
  fixture f;
  auto q = f.reg.query<Name>();
  CHECK(q.size() == 0);
  CHECK(q.empty());
  CHECK(q.entities().empty());

  f.reg.emplace<Position>(f.reg.create(), Position{});
  CHECK(q.size() == 0);
}

TEST_CASE("ecs: count is how many hold a component") {
  fixture f;
  CHECK(f.reg.count<Position>() == 0u);

  std::vector<entity_t> entities;
  for (int i = 0; i < 20; ++i) {
    const entity_t e = f.reg.create();
    f.reg.emplace<Position>(e, Position{static_cast<float>(i), 0});
    if (i % 2 == 0)
      f.reg.emplace<Velocity>(e, Velocity{});
    entities.push_back(e);
  }
  CHECK(f.reg.count<Position>() == 20u);
  CHECK(f.reg.count<Velocity>() == 10u);

  f.reg.remove<Position>(entities[0]);
  CHECK(f.reg.count<Position>() == 19u);

  f.reg.destroy(entities[1]);
  CHECK(f.reg.count<Position>() == 18u);
}

TEST_CASE("ecs: each_run hands over the entities and their components") {
  fixture f;
  std::vector<entity_t> entities;
  for (int i = 0; i < 20; ++i) {
    const entity_t e = f.reg.create();
    f.reg.emplace<Position>(e, Position{static_cast<float>(i), 0});
    entities.push_back(e);
  }

  usize runs = 0;
  usize seen = 0;
  f.reg.each_run<Position>(
      [&](const std::span<const entity_t> ents, Position *ps) {
        ++runs;
        for (usize i = 0; i < ents.size(); ++i) {
          CHECK(f.reg.get<Position>(ents[i]).x == ps[i].x);
          ++seen;
        }
      });
  CHECK(runs == 1u);
  CHECK(seen == 20u);

  f.reg.each_run<Position>(
      [](const std::span<const entity_t> ents, Position *ps) {
        for (usize i = 0; i < ents.size(); ++i)
          ps[i].y = 7.f;
      });
  for (const entity_t e : entities)
    CHECK(f.reg.get<Position>(e).y == 7.f);
}

TEST_CASE("ecs: a type nothing holds has no runs") {
  fixture f;
  usize runs = 0;
  f.reg.each_run<Position>([&](auto, auto *) { ++runs; });
  CHECK(runs == 0u);

  const entity_t e = f.reg.create();
  f.reg.emplace<Position>(e, Position{1.f, 2.f});
  f.reg.remove<Position>(e);
  f.reg.each_run<Position>([&](auto, auto *) { ++runs; });
  CHECK(runs == 0u);
}

TEST_CASE("ecs: a const registry hands out const components") {
  fixture f;
  const entity_t e = f.reg.create();
  f.reg.emplace<Position>(e, Position{3.f, 4.f});

  const auto &creg = f.reg;
  usize seen = 0;
  creg.each_run<Position>(
      [&](const std::span<const entity_t> ents, const Position *ps) {
        static_assert(std::is_const_v<std::remove_reference_t<decltype(*ps)>>);
        CHECK(ents.size() == 1u);
        CHECK(ps[0].x == 3.f);
        ++seen;
      });
  CHECK(seen == 1u);
  CHECK(creg.count<Position>() == 1u);
}

TEST_CASE("ecs: set_parent links a child and updates the counts") {
  fixture f;
  const entity_t parent = f.reg.create();
  const entity_t child = f.reg.create();

  CHECK_FALSE(f.reg.has_parent(child));
  CHECK(f.reg.parent(child) == reg_t::null_entity);
  CHECK(f.reg.child_count(parent) == 0);

  CHECK(f.reg.set_parent(child, parent));
  CHECK(f.reg.parent(child) == parent);
  CHECK(f.reg.has_parent(child));
  CHECK(f.reg.child_count(parent) == 1);
  CHECK(f.reg.statistics().relation_count == 1);

  const entity_t child2 = f.reg.create();
  CHECK(f.reg.set_parent(child2, parent));
  CHECK(f.reg.child_count(parent) == 2);
  CHECK(f.reg.statistics().relation_count == 2);
}

TEST_CASE("ecs: set_parent rejects invalid, self and cyclic links") {
  fixture f;
  const entity_t a = f.reg.create();
  const entity_t b = f.reg.create();
  const entity_t dead = f.reg.create();
  f.reg.destroy(dead);

  CHECK_FALSE(f.reg.set_parent(a, a));
  CHECK_FALSE(f.reg.set_parent(a, entity_t{}));
  CHECK_FALSE(f.reg.set_parent(entity_t{}, a));
  CHECK_FALSE(f.reg.set_parent(a, dead));
  CHECK_FALSE(f.reg.set_parent(dead, a));
  CHECK(f.reg.statistics().relation_count == 0);

  REQUIRE(f.reg.set_parent(b, a));
  CHECK_FALSE(f.reg.set_parent(a, b));
  CHECK(f.reg.parent(b) == a);
  CHECK(f.reg.parent(a) == reg_t::null_entity);

  const entity_t c = f.reg.create();
  REQUIRE(f.reg.set_parent(c, b));
  CHECK_FALSE(f.reg.set_parent(a, c));
  CHECK(f.reg.parent(a) == reg_t::null_entity);
  CHECK(f.reg.statistics().relation_count == 2);
}

TEST_CASE("ecs: re-parenting replaces the previous link") {
  fixture f;
  const entity_t p1 = f.reg.create();
  const entity_t p2 = f.reg.create();
  const entity_t child = f.reg.create();

  REQUIRE(f.reg.set_parent(child, p1));
  CHECK(f.reg.child_count(p1) == 1);

  REQUIRE(f.reg.set_parent(child, p2));
  CHECK(f.reg.parent(child) == p2);
  CHECK(f.reg.child_count(p1) == 0);
  CHECK(f.reg.child_count(p2) == 1);
  CHECK(f.reg.statistics().relation_count == 1);
}

TEST_CASE("ecs: remove_parent detaches a child") {
  fixture f;
  const entity_t parent = f.reg.create();
  const entity_t child = f.reg.create();
  REQUIRE(f.reg.set_parent(child, parent));

  f.reg.remove_parent(child);
  CHECK_FALSE(f.reg.has_parent(child));
  CHECK(f.reg.parent(child) == reg_t::null_entity);
  CHECK(f.reg.child_count(parent) == 0);
  CHECK(f.reg.statistics().relation_count == 0);

  f.reg.remove_parent(child);
  f.reg.remove_parent(f.reg.create());
  CHECK(f.reg.statistics().relation_count == 0);
}

TEST_CASE("ecs: each_child visits exactly the direct children") {
  fixture f;
  const entity_t parent = f.reg.create();
  std::vector<entity_t> children;
  for (int i = 0; i < 5; ++i) {
    const entity_t c = f.reg.create();
    REQUIRE(f.reg.set_parent(c, parent));
    children.push_back(c);
  }
  const entity_t grand = f.reg.create();
  REQUIRE(f.reg.set_parent(grand, children[0]));

  std::vector<entity_t> seen;
  f.reg.each_child(parent, [&](entity_t c) { seen.push_back(c); });
  CHECK(sorted(seen) == sorted(children));
  CHECK(f.reg.child_count(parent) == 5);

  int leaf_visits = 0;
  f.reg.each_child(grand, [&](entity_t) { ++leaf_visits; });
  CHECK(leaf_visits == 0);
}

TEST_CASE("ecs: children keep the order they were added") {
  fixture f;
  const entity_t parent = f.reg.create();
  std::vector<entity_t> added;
  for (int i = 0; i < 4; ++i) {
    const entity_t c = f.reg.create();
    REQUIRE(f.reg.set_parent(c, parent));
    added.push_back(c);
  }

  std::vector<entity_t> seen;
  f.reg.each_child(parent, [&](entity_t c) { seen.push_back(c); });
  CHECK(seen == added);

  f.reg.remove_parent(added[1]);
  REQUIRE(f.reg.set_parent(added[1], parent));
  seen.clear();
  f.reg.each_child(parent, [&](entity_t c) { seen.push_back(c); });
  CHECK(seen == std::vector<entity_t>{added[0], added[2], added[3], added[1]});

  f.reg.remove_parent(added[1]);
  const entity_t last = f.reg.create();
  REQUIRE(f.reg.set_parent(last, parent));
  seen.clear();
  f.reg.each_child(parent, [&](entity_t c) { seen.push_back(c); });
  CHECK(seen == std::vector<entity_t>{added[0], added[2], added[3], last});
}

TEST_CASE("ecs: each_child tolerates destroying the visited child") {
  fixture f;
  const entity_t parent = f.reg.create();
  std::vector<entity_t> children;
  for (int i = 0; i < 6; ++i) {
    const entity_t c = f.reg.create();
    REQUIRE(f.reg.set_parent(c, parent));
    children.push_back(c);
  }

  int visited = 0;
  f.reg.each_child(parent, [&](entity_t c) {
    ++visited;
    f.reg.destroy(c);
  });
  CHECK(visited == 6);
  CHECK(f.reg.child_count(parent) == 0);
  for (const entity_t c : children)
    CHECK_FALSE(f.reg.alive(c));
}

TEST_CASE("ecs: each_descendant walks the whole subtree") {
  fixture f;
  const entity_t root = f.reg.create();
  const entity_t a = f.reg.create();
  const entity_t b = f.reg.create();
  const entity_t a1 = f.reg.create();
  const entity_t a2 = f.reg.create();
  const entity_t b1 = f.reg.create();
  REQUIRE(f.reg.set_parent(a, root));
  REQUIRE(f.reg.set_parent(b, root));
  REQUIRE(f.reg.set_parent(a1, a));
  REQUIRE(f.reg.set_parent(a2, a));
  REQUIRE(f.reg.set_parent(b1, b));

  std::vector<entity_t> seen;
  f.reg.each_descendant(root, [&](entity_t e) { seen.push_back(e); });
  CHECK(sorted(seen) == sorted({a, b, a1, a2, b1}));
  CHECK(seen.size() == 5);

  std::vector<entity_t> under_a;
  f.reg.each_descendant(a, [&](entity_t e) { under_a.push_back(e); });
  CHECK(sorted(under_a) == sorted({a1, a2}));

  int leaf = 0;
  f.reg.each_descendant(a1, [&](entity_t) { ++leaf; });
  CHECK(leaf == 0);
}

TEST_CASE("ecs: is_descendant_of answers along the ancestor chain") {
  fixture f;
  const entity_t root = f.reg.create();
  const entity_t mid = f.reg.create();
  const entity_t leaf = f.reg.create();
  const entity_t other = f.reg.create();
  REQUIRE(f.reg.set_parent(mid, root));
  REQUIRE(f.reg.set_parent(leaf, mid));

  CHECK(f.reg.is_descendant_of(mid, root));
  CHECK(f.reg.is_descendant_of(leaf, mid));
  CHECK(f.reg.is_descendant_of(leaf, root));
  CHECK_FALSE(f.reg.is_descendant_of(root, leaf));
  CHECK_FALSE(f.reg.is_descendant_of(root, root));
  CHECK_FALSE(f.reg.is_descendant_of(other, root));
  CHECK_FALSE(f.reg.is_descendant_of(root, other));
}

TEST_CASE("ecs: destroy with detach_children promotes them to roots") {
  fixture f;
  const entity_t grand = f.reg.create();
  const entity_t parent = f.reg.create();
  const entity_t c1 = f.reg.create();
  const entity_t c2 = f.reg.create();
  REQUIRE(f.reg.set_parent(parent, grand));
  REQUIRE(f.reg.set_parent(c1, parent));
  REQUIRE(f.reg.set_parent(c2, parent));

  f.reg.destroy(parent, ecs::destroy_policy::detach_children);

  CHECK_FALSE(f.reg.alive(parent));
  CHECK(f.reg.alive(c1));
  CHECK(f.reg.alive(c2));
  CHECK_FALSE(f.reg.has_parent(c1));
  CHECK_FALSE(f.reg.has_parent(c2));
  CHECK(f.reg.child_count(grand) == 0);
  CHECK(f.reg.statistics().relation_count == 0);
}

TEST_CASE("ecs: destroy with destroy_descendants removes the whole subtree") {
  fixture f;
  const entity_t root = f.reg.create();
  const entity_t a = f.reg.create();
  const entity_t b = f.reg.create();
  const entity_t a1 = f.reg.create();
  const entity_t survivor = f.reg.create();
  REQUIRE(f.reg.set_parent(a, root));
  REQUIRE(f.reg.set_parent(b, root));
  REQUIRE(f.reg.set_parent(a1, a));

  f.reg.destroy(root, ecs::destroy_policy::destroy_descendants);

  CHECK_FALSE(f.reg.alive(root));
  CHECK_FALSE(f.reg.alive(a));
  CHECK_FALSE(f.reg.alive(b));
  CHECK_FALSE(f.reg.alive(a1));
  CHECK(f.reg.alive(survivor));
  CHECK(f.reg.size() == 1);
  CHECK(f.reg.statistics().relation_count == 0);
}

TEST_CASE("ecs: destroy with reparent_to_grandparent relinks the children") {
  fixture f;
  const entity_t grand = f.reg.create();
  const entity_t parent = f.reg.create();
  const entity_t c1 = f.reg.create();
  const entity_t c2 = f.reg.create();
  REQUIRE(f.reg.set_parent(parent, grand));
  REQUIRE(f.reg.set_parent(c1, parent));
  REQUIRE(f.reg.set_parent(c2, parent));

  f.reg.destroy(parent, ecs::destroy_policy::reparent_to_grandparent);

  CHECK_FALSE(f.reg.alive(parent));
  CHECK(f.reg.parent(c1) == grand);
  CHECK(f.reg.parent(c2) == grand);
  CHECK(f.reg.child_count(grand) == 2);

  const entity_t lone = f.reg.create();
  const entity_t k = f.reg.create();
  REQUIRE(f.reg.set_parent(k, lone));
  f.reg.destroy(lone, ecs::destroy_policy::reparent_to_grandparent);
  CHECK(f.reg.alive(k));
  CHECK_FALSE(f.reg.has_parent(k));
}

TEST_CASE("ecs: a deep chain builds and unwinds correctly") {
  fixture f;
  constexpr int depth = 200;
  std::vector<entity_t> chain;
  chain.push_back(f.reg.create());
  for (int i = 1; i < depth; ++i) {
    const entity_t e = f.reg.create();
    REQUIRE(f.reg.set_parent(e, chain.back()));
    chain.push_back(e);
  }

  CHECK(f.reg.statistics().relation_count == depth - 1);
  CHECK(f.reg.is_descendant_of(chain.back(), chain.front()));
  CHECK_FALSE(f.reg.is_descendant_of(chain.front(), chain.back()));

  int descendants = 0;
  f.reg.each_descendant(chain.front(), [&](entity_t) { ++descendants; });
  CHECK(descendants == depth - 1);

  CHECK_FALSE(f.reg.set_parent(chain.front(), chain.back()));
  CHECK_FALSE(f.reg.set_parent(chain[depth / 2], chain.back()));

  f.reg.destroy(chain.front(), ecs::destroy_policy::destroy_descendants);
  CHECK(f.reg.empty());
  CHECK(f.reg.statistics().relation_count == 0);
}

TEST_CASE("ecs: hierarchy survives a long randomised workload") {
  fixture f;
  std::vector<entity_t> live;
  for (int i = 0; i < 200; ++i)
    live.push_back(f.reg.create());

  u64 state = 0xDEC0DE1234567890ull;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  for (int step = 0; step < 20000 && live.size() > 1; ++step) {
    const u64 r = next();
    const usize i = static_cast<usize>(r >> 8) % live.size();
    const usize j = static_cast<usize>(r >> 24) % live.size();

    switch (r % 4) {
    case 0:
      (void)f.reg.set_parent(live[i], live[j]);
      break;
    case 1:
      f.reg.remove_parent(live[i]);
      break;
    case 2:
      live.push_back(f.reg.create());
      break;
    default: {
      const entity_t victim = live[i];
      f.reg.destroy(victim, ecs::destroy_policy::detach_children);
      live[i] = live.back();
      live.pop_back();
      break;
    }
    }
  }

  for (const entity_t e : live) {
    REQUIRE(f.reg.alive(e));
    usize hops = 0;
    entity_t cur = f.reg.parent(e);
    while (cur.valid()) {
      REQUIRE(f.reg.alive(cur));
      REQUIRE(++hops <= live.size());
      cur = f.reg.parent(cur);
    }

    usize counted = 0;
    f.reg.each_child(e, [&](entity_t c) {
      REQUIRE(f.reg.alive(c));
      REQUIRE(f.reg.parent(c) == e);
      ++counted;
    });
    REQUIRE(counted == f.reg.child_count(e));
  }
}

TEST_CASE("ecs: statistics track entities, storages and relations") {
  fixture f;
  const ecs::registry_statistics base = f.reg.statistics();
  CHECK(base.entity_count == 0);
  CHECK(base.component_type_count == 7);
  CHECK(base.component_storage_count == 7);

  const entity_t a = f.reg.create();
  const entity_t b = f.reg.create();
  f.reg.emplace<Position>(a, Position{});
  REQUIRE(f.reg.set_parent(b, a));

  const ecs::registry_statistics st = f.reg.statistics();
  CHECK(st.entity_count == 2);
  CHECK(st.entity_slot_count >= 2);
  CHECK(st.free_slot_count == 0);
  CHECK(st.relation_count == 1);
  CHECK(st.approx_memory_bytes > 0);

  f.reg.destroy(b);
  const ecs::registry_statistics after = f.reg.statistics();
  CHECK(after.entity_count == 1);
  CHECK(after.free_slot_count == 1);
  CHECK(after.relation_count == 0);
}

TEST_CASE("ecs: component_statistics report per-type counts") {
  fixture f;
  for (int i = 0; i < 25; ++i)
    f.reg.emplace<Position>(f.reg.create(), Position{});

  const ecs::component_type_statistics st =
      f.reg.component_statistics<Position>();
  CHECK(st.name == "Position");
  CHECK(st.size == sizeof(Position));
  CHECK(st.alignment == alignof(Position));
  CHECK(st.count == 25);
  CHECK(st.capacity >= 25);
  CHECK(st.id == f.reg.component_metadata_of<Position>().id);

  const ecs::component_type_statistics empty =
      f.reg.component_statistics<Velocity>();
  CHECK(empty.count == 0);
}

TEST_CASE("ecs: the registry is move-only and moving transfers everything") {
  static_assert(!std::is_copy_constructible_v<reg_t>);
  static_assert(!std::is_copy_assignable_v<reg_t>);
  static_assert(std::is_move_constructible_v<reg_t>);
  static_assert(std::is_move_assignable_v<reg_t>);

  g_live = 0;
  {
    reg_t src;
    src.register_component<Position>({.name = "Position"});
    src.register_component<Counted>({.name = "Counted"});

    const entity_t a = src.create();
    const entity_t b = src.create();
    src.emplace<Position>(a, Position{1, 2});
    src.emplace<Counted>(a, 7);
    src.emplace<Position>(b, Position{3, 4});
    REQUIRE(src.set_parent(b, a));
    CHECK(g_live == 1);

    reg_t dst = std::move(src);
    CHECK(dst.size() == 2);
    CHECK(dst.alive(a));
    CHECK(dst.get<Position>(a).x == 1);
    CHECK(dst.get<Counted>(a).value == 7);
    CHECK(dst.parent(b) == a);
    CHECK(dst.is_component_registered<Position>());
    CHECK(g_live == 1);

    CHECK(src.size() == 0);

    reg_t other;
    other.register_component<Counted>({.name = "Counted2"});
    other.emplace<Counted>(other.create(), 99);
    CHECK(g_live == 2);
    other = std::move(dst);
    CHECK(g_live == 1);
    CHECK(other.size() == 2);
    CHECK(other.get<Position>(a).x == 1);

    reg_t *alias = &other;
    other = std::move(*alias);
    CHECK(other.size() == 2);
    CHECK(other.get<Position>(a).x == 1);
  }
  CHECK(g_live == 0);
}

TEST_CASE("ecs: a 64-bit-handle registry behaves the same") {
  ecs::registry64 reg;
  reg.register_component<Position>({.name = "Position"});

  const auto a = reg.create();
  const auto b = reg.create();
  reg.emplace<Position>(a, Position{1, 2});
  reg.emplace<Position>(b, Position{3, 4});
  CHECK(reg.size() == 2);
  CHECK(reg.get<Position>(a).x == 1);

  REQUIRE(reg.set_parent(b, a));
  CHECK(reg.parent(b) == a);

  reg.destroy(a, ecs::destroy_policy::destroy_descendants);
  CHECK(reg.empty());

  const auto c = reg.create();
  CHECK(c.index() == a.index());
  CHECK(c.version() != a.version());
  CHECK_FALSE(reg.alive(a));
}

TEST_CASE("ecs: get_allocator is callable") {
  const reg_t reg;
  CHECK(reg.get_allocator() == reg.get_allocator());
}

TEST_CASE("ecs: a simulation-style workload keeps every invariant") {
  fixture f;
  constexpr int n = 500;
  std::vector<entity_t> entities;

  for (int i = 0; i < n; ++i) {
    const entity_t e = f.reg.create();
    f.reg.emplace<Position>(e, Position{static_cast<float>(i), 0});
    if (i % 2 == 0)
      f.reg.emplace<Velocity>(e, Velocity{1, 2});
    if (i % 7 == 0)
      f.reg.emplace<Frozen>(e);
    entities.push_back(e);
  }

  for (int step = 0; step < 10; ++step) {
    f.reg.view<Position, Velocity>().exclude<Frozen>(f.reg).each(
        [](entity_t, Position &p, Velocity &v) {
          p.x += v.dx;
          p.y += v.dy;
        });
  }

  for (int i = 0; i < n; ++i) {
    const entity_t e = entities[static_cast<usize>(i)];
    const bool moving = (i % 2 == 0) && (i % 7 != 0);
    const float expected_x = static_cast<float>(i) + (moving ? 10.0f : 0.0f);
    const float expected_y = moving ? 20.0f : 0.0f;
    REQUIRE(f.reg.get<Position>(e).x == expected_x);
    REQUIRE(f.reg.get<Position>(e).y == expected_y);
  }

  auto q = f.reg.query<Position, Velocity>();
  usize view_count = 0;
  f.reg.view<Position, Velocity>().each(
      [&](entity_t, Position &, Velocity &) { ++view_count; });
  CHECK(q.size() == view_count);

  for (int i = 0; i < n; i += 2)
    f.reg.destroy(entities[static_cast<usize>(i)]);
  CHECK(f.reg.size() == n / 2);

  usize remaining = 0;
  f.reg.view<Position>().each([&](entity_t e, Position &) {
    REQUIRE(f.reg.alive(e));
    ++remaining;
  });
  CHECK(remaining == static_cast<usize>(n / 2));
  CHECK(q.size() == 0);
}

TEST_CASE("ecs: a cached query sees type-erased adds and removes") {
  fixture f;
  auto q = f.reg.query<Position>();
  const entity_t a = f.reg.create();
  CHECK(q.size() == 0);

  auto *const storage =
      static_cast<ecs::detail::storage_base<entity_t, reg_t::allocator_type> *>(
          f.reg.component_metadata_of<Position>().storage);
  REQUIRE(storage != nullptr);
  REQUIRE(storage->emplace_default(f.reg, a) != nullptr);
  CHECK(q.size() == 1);
  CHECK(storage->remove(f.reg, a));
  CHECK(q.size() == 0);
}

TEST_CASE("ecs: an on_destroy callback may destroy its own entity") {
  fixture f;
  int fired = 0;
  f.reg.on_destroy<Position>([&](reg_t &r, entity_t e) {
    ++fired;
    r.destroy(e);
  });
  const entity_t a = f.reg.create();
  f.reg.emplace<Position>(a, Position{});
  f.reg.emplace<Velocity>(a, Velocity{});
  f.reg.remove<Position>(a);
  CHECK(fired == 1);
  CHECK_FALSE(f.reg.alive(a));

  const entity_t b = f.reg.create();
  f.reg.emplace<Position>(b, Position{});
  f.reg.destroy(b);
  CHECK(fired == 2);
  CHECK_FALSE(f.reg.alive(b));
  CHECK(f.reg.size() == 0);

  const entity_t c = f.reg.create();
  const entity_t d = f.reg.create();
  CHECK(c != d);
  CHECK(f.reg.alive(c));
  CHECK(f.reg.alive(d));
}

TEST_CASE("ecs: emplace returns the component where callbacks left it") {
  fixture f;
  bool adding = false;
  f.reg.on_construct<Position>([&](reg_t &r, entity_t) {
    if (adding)
      return;
    adding = true;
    for (int i = 0; i < 1000; ++i)
      r.emplace<Position>(r.create(), Position{});
    adding = false;
  });
  const entity_t a = f.reg.create();
  Position &p = f.reg.emplace<Position>(a, Position{3.f, 4.f});
  CHECK(&p == &f.reg.get<Position>(a));
  CHECK(p == Position{3.f, 4.f});
}

TEST_CASE("ecs: a view's each may destroy the entity it is given") {
  fixture f;
  for (int i = 0; i < 10; ++i)
    f.reg.emplace<Position>(f.reg.create(), Position{});
  int seen = 0;
  f.reg.view<Position>().each([&](entity_t e, Position &) {
    ++seen;
    f.reg.destroy(e);
  });
  CHECK(seen == 10);
  CHECK(f.reg.size() == 0);
}

TEST_CASE("ecs: a cached query skips entities its callback destroyed") {
  fixture f;
  nx::vector<entity_t> made;
  for (int i = 0; i < 6; ++i) {
    made.push_back(f.reg.create());
    f.reg.emplace<Position>(made.back(), Position{});
  }
  auto q = f.reg.query<Position>();
  REQUIRE(q.size() == 6u);
  int seen = 0;
  q.each([&](entity_t e, Position &) {
    ++seen;
    const auto at = std::find(made.begin(), made.end(), e);
    if (at + 1 < made.end() && f.reg.alive(*(at + 1)))
      f.reg.destroy(*(at + 1));
  });
  CHECK(seen == 3);
  CHECK(q.size() == 3u);
}

TEST_CASE("ecs: an unregistered component type throws in every build") {
  struct Unregistered {
    int value = 0;
  };
  fixture f;
  const entity_t e = f.reg.create();
  CHECK_THROWS_AS(f.reg.emplace<Unregistered>(e, Unregistered{}),
                  std::logic_error);
}
