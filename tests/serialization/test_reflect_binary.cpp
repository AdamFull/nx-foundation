
#include "framework/nxtest.h"

#include "core/foundation/reflection/attributes.h"
#include "core/foundation/reflection/layout.h"
#include "core/foundation/serialization/reflect_binary.h"
#include "core/foundation/strings/id_string.h"
#include "core/foundation/strings/utf8_string.h"

#include <vector>

namespace nxtest_reflect_binary {

enum class Facing : u8 { Left, Right, Up };

struct Inner {
  f64 weight = 0.0;
  bool flagged = false;
};

struct Vec3 {
  f32 x = 0.f;
  f32 y = 0.f;
  f32 z = 0.f;
};

struct Sample {
  bool on = false;
  i32 count = 0;
  u32 mask = 0;
  f32 size = 0.f;
  f64 precise = 0.0;
  nx::string name;
  nx::id_string tag;
  Facing facing = Facing::Left;
  Inner inner;
  Vec3 tint;
  i32 slots[3] = {};
  std::vector<f32> values;
};

} // namespace nxtest_reflect_binary

NX_REFLECT_ENUM(nxtest_reflect_binary::Facing, (Left, "left"), (Right, "right"),
                (Up, "up"))

using namespace nxtest_reflect_binary;

TEST_CASE("reflect binary: a struct round-trips through nva bytes") {
  Sample a;
  a.on = true;
  a.count = -12345;
  a.mask = 0xDEADBEEFu;
  a.size = 3.5f;
  a.precise = 2.718281828;
  a.name = "hello world";
  a.tag = nx::id_string("dissolve");
  a.facing = Facing::Up;
  a.inner = {9.5, true};
  a.tint = {0.1f, 0.2f, 0.3f};
  a.slots[0] = 7;
  a.slots[1] = 8;
  a.slots[2] = 9;
  a.values = {1.f, 2.f, 3.f, 4.f};

  nx::nva::Writer w;
  nx::reflect::write_binary(a, w);

  nx::nva::Reader r(w.span());
  Sample b;
  REQUIRE(nx::reflect::read_binary(r, b));
  CHECK(r.ok());

  CHECK(b.on == true);
  CHECK(b.count == -12345);
  CHECK(b.mask == 0xDEADBEEFu);
  CHECK(b.size == 3.5f);
  CHECK(b.precise == 2.718281828);
  CHECK(b.name.view() == "hello world");
  CHECK(b.tag.view() == "dissolve");
  CHECK(b.facing == Facing::Up);
  CHECK(b.inner.weight == 9.5);
  CHECK(b.inner.flagged == true);
  CHECK(b.tint.x == 0.1f);
  CHECK(b.tint.y == 0.2f);
  CHECK(b.tint.z == 0.3f);
  CHECK(b.slots[0] == 7);
  CHECK(b.slots[2] == 9);
  REQUIRE(b.values.size() == 4u);
  CHECK(b.values[3] == 4.f);
}

TEST_CASE("reflect binary: schema digest ignores native aggregate layout") {
  const nx::reflect::type_info &native = nx::reflect::type_of<Sample>();
  nx::vector<nx::reflect::field_info> moved(native.fields.begin(),
                                            native.fields.end());
  for (nx::reflect::field_info &field : moved)
    field.offset += 16;

  nx::reflect::type_info other = native;
  other.size += 64;
  other.align *= 2;
  other.fields = {moved.data(), moved.size()};

  CHECK(nx::reflect::layout_digest(native) !=
        nx::reflect::layout_digest(other));
  CHECK(nx::reflect::binary_schema_digest(native) ==
        nx::reflect::binary_schema_digest(other));
}

TEST_CASE("reflect binary: schema digest changes with the wire fields") {
  const nx::reflect::type_info &native = nx::reflect::type_of<Sample>();
  nx::vector<nx::reflect::field_info> shortened(native.fields.begin(),
                                                native.fields.end() - 1);
  nx::reflect::type_info other = native;
  other.fields = {shortened.data(), shortened.size()};

  CHECK(nx::reflect::binary_schema_digest(native) !=
        nx::reflect::binary_schema_digest(other));
}

TEST_CASE("reflect binary: a truncated buffer fails rather than reading junk") {
  Sample a;
  a.values = {1.f, 2.f, 3.f};
  nx::nva::Writer w;
  nx::reflect::write_binary(a, w);

  const std::span<const u8> full = w.span();
  nx::nva::Reader r(full.subspan(0, full.size() / 2));
  Sample b;
  CHECK(nx::reflect::read_binary(r, b) == false);
}

TEST_CASE("reflect binary: an impossible sequence count fails before resize") {
  Sample source;
  nx::nva::Writer w;
  nx::reflect::write_binary(source, w);
  nx::vector<u8> bytes = w.take();
  REQUIRE(bytes.size() >= sizeof(u32));
  for (usize i = bytes.size() - sizeof(u32); i < bytes.size(); ++i)
    bytes[i] = 0xffu;

  Sample decoded;
  decoded.values = {9.f};
  nx::nva::Reader r({bytes.data(), bytes.size()});
  CHECK_FALSE(nx::reflect::read_binary(r, decoded));
  CHECK_FALSE(r.ok());
  REQUIRE(decoded.values.size() == 1u);
  CHECK(decoded.values[0] == 9.f);
}
