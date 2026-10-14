
#include "framework/nxtest.h"

#include "core/foundation/reflection/layout.h"

struct Base {
  f32 x = 0.f;
  f32 y = 0.f;
  u32 flags = 0;
};

struct Reordered {
  u32 flags = 0;
  f32 x = 0.f;
  f32 y = 0.f;
};

struct Renamed {
  f32 x = 0.f;
  f32 y = 0.f;
  u32 mask = 0;
};

struct Extended {
  f32 x = 0.f;
  f32 y = 0.f;
  u32 flags = 0;
  u32 extra = 0;
};

struct Retyped {
  f32 x = 0.f;
  f32 y = 0.f;
  f32 flags = 0.f;
};

struct Padded {
  f32 x = 0.f;
  f64 y = 0.0;
  u32 flags = 0;
};

TEST_CASE("layout: a type digests the same every time") {
  const u64 once = nx::reflect::layout_digest<Base>();
  CHECK(once == nx::reflect::layout_digest<Base>());
  CHECK(once != 0u);
}

TEST_CASE("layout: reordering the fields changes it") {
  CHECK(sizeof(Base) == sizeof(Reordered));
  CHECK(nx::reflect::layout_digest<Base>() !=
        nx::reflect::layout_digest<Reordered>());
}

TEST_CASE("layout: renaming a field changes it") {
  CHECK(sizeof(Base) == sizeof(Renamed));
  CHECK(nx::reflect::layout_digest<Base>() !=
        nx::reflect::layout_digest<Renamed>());
}

TEST_CASE("layout: adding a field changes it") {
  CHECK(nx::reflect::layout_digest<Base>() !=
        nx::reflect::layout_digest<Extended>());
}

TEST_CASE("layout: a field changing type changes it") {
  CHECK(sizeof(Base) == sizeof(Retyped));
  CHECK(nx::reflect::layout_digest<Base>() !=
        nx::reflect::layout_digest<Retyped>());
}

TEST_CASE("layout: padding is part of it") {
  CHECK(nx::reflect::layout_digest<Base>() !=
        nx::reflect::layout_digest<Padded>());
}

TEST_CASE("layout: describe names every field with where it sits") {
  const nx::string text =
      nx::reflect::describe_layout(nx::reflect::type_of<Base>());

  CHECK(text.find("Base") != nx::string::npos);
  for (const nx::string_view field : {"x", "y", "flags"})
    CHECK(text.find(field) != nx::string::npos);
  CHECK(text.find("+0") != nx::string::npos);
  CHECK(text.find("+4") != nx::string::npos);
  CHECK(text.find("+8") != nx::string::npos);
}

TEST_CASE("layout: describing a padded type shows the hole") {
  const nx::string text =
      nx::reflect::describe_layout(nx::reflect::type_of<Padded>());
  CHECK(text.find("+0") != nx::string::npos);
  CHECK(text.find("+8") != nx::string::npos);
}

namespace {

using field = nx::reflect::field_info;
using described = nx::reflect::type_info;

[[nodiscard]] described same_name_holding(std::span<const field> fields) {
  described t{};
  t.name = "Held";
  t.size = 8;
  t.align = 4;
  t.kind = nx::reflect::type_kind::aggregate;
  t.fields = fields;
  return t;
}

[[nodiscard]] field at(const nx::string_view name, const u32 offset,
                       const described &type) {
  field f{};
  f.name = name;
  f.offset = offset;
  f.type = &type;
  return f;
}

}

TEST_CASE("layout: one name, a field moved, is a different layout") {
  const described &f = nx::reflect::type_of<f32>();
  const field before[] = {at("x", 0, f), at("y", 4, f)};
  const field after[] = {at("x", 4, f), at("y", 0, f)};

  CHECK(nx::reflect::layout_digest(same_name_holding(before)) !=
        nx::reflect::layout_digest(same_name_holding(after)));
}

TEST_CASE("layout: one name, a field renamed, is a different layout") {
  const described &f = nx::reflect::type_of<f32>();
  const field before[] = {at("x", 0, f), at("y", 4, f)};
  const field after[] = {at("x", 0, f), at("z", 4, f)};

  CHECK(nx::reflect::layout_digest(same_name_holding(before)) !=
        nx::reflect::layout_digest(same_name_holding(after)));
}

TEST_CASE("layout: one name, a field dropped, is a different layout") {
  const described &f = nx::reflect::type_of<f32>();
  const field before[] = {at("x", 0, f), at("y", 4, f)};
  const field after[] = {at("x", 0, f)};

  CHECK(nx::reflect::layout_digest(same_name_holding(before)) !=
        nx::reflect::layout_digest(same_name_holding(after)));
}

TEST_CASE("layout: one name, a field's width changed, is a different layout") {
  const described &narrow = nx::reflect::type_of<f32>();
  const described &wide = nx::reflect::type_of<f64>();
  const field before[] = {at("x", 0, narrow)};
  const field after[] = {at("x", 0, wide)};

  CHECK(nx::reflect::layout_digest(same_name_holding(before)) !=
        nx::reflect::layout_digest(same_name_holding(after)));
}

TEST_CASE("layout: the same fields under the same name digest alike") {
  const described &f = nx::reflect::type_of<f32>();
  const field one[] = {at("x", 0, f), at("y", 4, f)};
  const field two[] = {at("x", 0, f), at("y", 4, f)};

  CHECK(nx::reflect::layout_digest(same_name_holding(one)) ==
        nx::reflect::layout_digest(same_name_holding(two)));
}
