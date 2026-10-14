
#include "framework/nxtest.h"

#include "core/foundation/reflection/type.h"
#include "core/foundation/strings/utf8_string.h"

#include <vector>

namespace nxtest_reflect {

struct Vec2 {
  f32 x = 0.f;
  f32 y = 0.f;
};
struct Vec3 {
  f32 x = 0.f;
  f32 y = 0.f;
  f32 z = 0.f;
};
struct Vec4 {
  f32 x = 0.f;
  f32 y = 0.f;
  f32 z = 0.f;
  f32 w = 0.f;
};

struct IntPair {
  i32 x = 0;
  i32 y = 0;
};

class Handle {
public:
  Handle() = default;
  explicit Handle(u32 v) noexcept : m_value(v) {}

private:
  [[maybe_unused]] u32 m_value = 0;
};

enum class Mood : u8 { Calm, Restless };

struct Inner {
  f32 weight = 1.f;
  bool flagged = false;
};

struct Thing {
  bool on = true;
  i32 count = 0;
  u32 mask = 0;
  f32 scale = 1.f;
  f64 precise = 0.0;
  nx::string label;
  nx::id_string tag;
  Vec2 offset;
  Vec3 heading;
  Vec4 tint;
  Mood mood = Mood::Calm;
  Inner inner;
  nx::vector<f32> scores;
  nx::vector<Inner> parts;
  i32 slots[4] = {};
  Handle handle;
};

}

namespace {

using nx::reflect::type_kind;
using namespace nxtest_reflect;

[[nodiscard]] const nx::reflect::field_info &named(const nx::string_view n) {
  const auto *f = nx::reflect::type_of<Thing>().field(n);
  REQUIRE(f != nullptr);
  return *f;
}

}

TEST_CASE("type: a type knows its own name without RTTI") {
  const auto &t = nx::reflect::type_of<Thing>();
  CHECK(t.name == "nxtest_reflect::Thing");
  CHECK(t.size == sizeof(Thing));
  CHECK(t.align == alignof(Thing));
  CHECK(t.kind == type_kind::aggregate);
  CHECK(t.id != 0u);
  CHECK(t.id != nx::reflect::type_of<Inner>().id);
}

TEST_CASE("type: one description per type, not one per lookup") {
  CHECK(&nx::reflect::type_of<Thing>() == &nx::reflect::type_of<Thing>());
  CHECK(&nx::reflect::type_of<Inner>() == &nx::reflect::type_of<const Inner>());
}

TEST_CASE("type: every field reports the kind its own type implies") {
  CHECK(named("on").type->kind == type_kind::boolean);
  CHECK(named("count").type->kind == type_kind::integer);
  CHECK(named("mask").type->kind == type_kind::unsigned_integer);
  CHECK(named("scale").type->kind == type_kind::floating);
  CHECK(named("precise").type->kind == type_kind::floating);
  CHECK(named("label").type->kind == type_kind::string);
  CHECK(named("tag").type->kind == type_kind::id);
  CHECK(named("offset").type->kind == type_kind::floats);
  CHECK(named("mood").type->kind == type_kind::enumeration);
  CHECK(named("inner").type->kind == type_kind::aggregate);
  CHECK(named("scores").type->kind == type_kind::sequence);
  CHECK(named("slots").type->kind == type_kind::sequence);
  CHECK(named("handle").type->kind == type_kind::opaque);
}

TEST_CASE("type: a float vector is a width, not a case") {
  CHECK(named("offset").type->components() == 2u);
  CHECK(named("heading").type->components() == 3u);
  CHECK(named("tint").type->components() == 4u);
  CHECK_FALSE(nx::reflect::float_vector<IntPair>);
  CHECK(nx::reflect::type_of<IntPair>().kind == type_kind::aggregate);
}

TEST_CASE("type: a field's description is its own type's description") {
  CHECK(named("inner").type == &nx::reflect::type_of<Inner>());
  CHECK(named("scale").type == &nx::reflect::type_of<f32>());
  CHECK(named("inner").type->fields.size() == 2u);
  CHECK(named("inner").type->field("weight") != nullptr);
}

TEST_CASE("type: offsets address the field and not its neighbour") {
  Thing t;
  auto *base = reinterpret_cast<std::byte *>(&t);
  *reinterpret_cast<i32 *>(base + named("count").offset) = 42;
  *reinterpret_cast<f32 *>(base + named("scale").offset) = 2.5f;
  *reinterpret_cast<bool *>(base + named("on").offset) = false;

  CHECK(t.count == 42);
  CHECK(t.scale == 2.5f);
  CHECK_FALSE(t.on);
  CHECK(t.mask == 0u);
  CHECK(t.precise == 0.0);
}

TEST_CASE("type: a list says what it holds and how to walk it") {
  const auto &scores = *named("scores").type;
  REQUIRE(scores.sequence != nullptr);
  CHECK(scores.sequence->element == &nx::reflect::type_of<f32>());
  CHECK_FALSE(scores.sequence->fixed);

  const auto &parts = *named("parts").type;
  REQUIRE(parts.sequence != nullptr);
  CHECK(parts.sequence->element == &nx::reflect::type_of<Inner>());
  CHECK(parts.sequence->element->fields.size() == 2u);

  Thing t;
  void *p = reinterpret_cast<std::byte *>(&t) + named("scores").offset;
  scores.sequence->resize(p, 2);
  *static_cast<f32 *>(scores.sequence->at(p, 1)) = 7.f;
  REQUIRE(t.scores.size() == 2u);
  CHECK(t.scores[1] == 7.f);
}

TEST_CASE("type: a C array is a list that cannot grow") {
  CHECK(nx::reflect::type_of<Thing>().fields.size() == 16u);

  const auto &slots = *named("slots").type;
  REQUIRE(slots.sequence != nullptr);
  CHECK(slots.sequence->fixed);
  CHECK(slots.sequence->element == &nx::reflect::type_of<i32>());

  Thing t;
  void *p = reinterpret_cast<std::byte *>(&t) + named("slots").offset;
  CHECK(slots.sequence->size(p) == 4u);
  slots.sequence->resize(p, 9);
  CHECK(slots.sequence->size(p) == 4u);
}

TEST_CASE("type: an enum carries its width and signedness with no spellings") {
  const auto &mood = *named("mood").type;
  REQUIRE(mood.enumeration != nullptr);
  CHECK(mood.size == sizeof(u8));
  CHECK_FALSE(mood.enumeration->is_signed);
  CHECK(mood.enumeration->entries.empty());
}

TEST_CASE("type: a description can make, copy and unmake an instance") {
  const auto &t = nx::reflect::type_of<Inner>();
  REQUIRE(t.ops != nullptr);
  REQUIRE(t.ops->default_construct != nullptr);
  REQUIRE(t.ops->copy_assign != nullptr);
  REQUIRE(t.defaults != nullptr);

  alignas(Inner) std::byte storage[sizeof(Inner)];
  t.ops->default_construct(storage);
  auto *made = reinterpret_cast<Inner *>(storage);
  CHECK(made->weight == 1.f);

  made->weight = 5.f;
  t.ops->copy_assign(storage, t.defaults);
  CHECK(made->weight == 1.f);
  t.ops->destroy(storage);
}
