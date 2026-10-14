
#include "framework/nxtest.h"

#include "core/foundation/reflection/aggregate.h"
#include "core/foundation/strings/utf8_string.h"

#include <vector>

namespace nxtest_aggregate {

struct Empty {};

struct One {
  f32 only = 0.f;
};

struct AlphaFirst {
  f32 alpha = 0.f;
  bool on = false;
};
struct BetaFirst {
  f32 beta = 0.f;
  bool on = false;
};

struct Nested {
  f64 weight = 1.0;
  bool flagged = false;
};

struct Outer {
  Nested inner;
  i32 x = 0;
};

struct ArrayFirst {
  i32 slots[3] = {};
  f32 after = 0.f;
};
struct ArrayMiddle {
  bool before = false;
  i32 slots[4] = {};
  f32 after = 0.f;
};
struct ArrayLast {
  f32 before = 0.f;
  i32 slots[5] = {};
};
struct TwoArrays {
  i32 a[2] = {};
  i32 b[3] = {};
};
struct Grid {
  i32 cells[2][3] = {};
  Nested inner;
};

class Handle {
public:
  Handle() = default;
  explicit Handle(u32 v) noexcept : m_value(v) {}
  [[nodiscard]] u32 value() const noexcept { return m_value; }

private:
  u32 m_value = 0;
};

struct Mixed {
  nx::string text;
  Handle handle;
  std::vector<f32> values;
  f32 size = 1.f;
  bool visible = true;
};

}

namespace {

struct NoDefault {
  explicit NoDefault(i32);
};

struct InternalLinkage {
  NoDefault value;
};

static_assert(!std::is_default_constructible_v<InternalLinkage>);

}

using namespace nxtest_aggregate;

TEST_CASE("aggregate: field count for the shapes the engine holds") {
  CHECK(nx::reflect::field_count<Empty> == 0u);
  CHECK(nx::reflect::field_count<One> == 1u);
  CHECK(nx::reflect::field_count<Nested> == 2u);
  CHECK(nx::reflect::field_count<Outer> == 2u);
  CHECK(nx::reflect::field_count<Mixed> == 5u);
}

TEST_CASE("aggregate: a raw array is one field, not one per element") {
  CHECK(nx::reflect::field_count<ArrayFirst> == 2u);
  CHECK(nx::reflect::field_count<ArrayMiddle> == 3u);
  CHECK(nx::reflect::field_count<ArrayLast> == 2u);
  CHECK(nx::reflect::field_count<TwoArrays> == 2u);
  CHECK(nx::reflect::field_count<Grid> == 2u);

  CHECK(nx::reflect::field_name<ArrayMiddle, 1>() == "slots");
  CHECK(nx::reflect::field_name<ArrayMiddle, 2>() == "after");
}

TEST_CASE("aggregate: tie hands out the whole array, not its first element") {
  ArrayMiddle m;
  const usize width =
      nx::reflect::tie(m, [](auto &before, auto &slots, auto &after) {
        before = true;
        slots[3] = 9;
        after = 2.f;
        return sizeof(slots) / sizeof(slots[0]);
      });

  CHECK(width == 4u);
  CHECK(m.before);
  CHECK(m.slots[3] == 9);
  CHECK(m.after == 2.f);
}

TEST_CASE("aggregate: tie hands out a reference to each field") {
  Mixed m;
  nx::reflect::tie(
      m, [](auto &text, auto &handle, auto &values, auto &size, auto &visible) {
        text = "hello";
        handle = Handle{7};
        values.push_back(2.5f);
        size = 3.f;
        visible = false;
      });

  CHECK(m.text == "hello");
  CHECK(m.handle.value() == 7u);
  REQUIRE(m.values.size() == 1u);
  CHECK(m.values[0] == 2.5f);
  CHECK(m.size == 3.f);
  CHECK_FALSE(m.visible);
}

TEST_CASE("aggregate: tie on a const object hands out const references") {
  const Nested n{2.0, true};
  const f64 weight = nx::reflect::tie(
      n, [](const auto &w, const auto &) { return static_cast<f64>(w); });
  CHECK(weight == 2.0);
}

TEST_CASE("aggregate: a field knows the name it was declared with") {
  CHECK(nx::reflect::field_name<Nested, 0>() == "weight");
  CHECK(nx::reflect::field_name<Nested, 1>() == "flagged");

  CHECK(nx::reflect::field_name<Mixed, 0>() == "text");
  CHECK(nx::reflect::field_name<Mixed, 1>() == "handle");
  CHECK(nx::reflect::field_name<Mixed, 2>() == "values");
  CHECK(nx::reflect::field_name<Mixed, 3>() == "size");
  CHECK(nx::reflect::field_name<Mixed, 4>() == "visible");

  CHECK(nx::reflect::field_name<Outer, 0>() == "inner");
  CHECK(nx::reflect::field_name<Outer, 1>() == "x");
}

TEST_CASE("aggregate: names are available to a constant expression") {
  static_assert(nx::reflect::field_name<One, 0>() == "only");
  static_assert(nx::reflect::field_name<InternalLinkage, 0>() == "value");
  static_assert(nx::reflect::field_count<Mixed> == 5u);
  CHECK(true);
}

TEST_CASE("aggregate: only aggregates qualify") {
  CHECK(nx::reflect::plain_aggregate<Nested>);
  CHECK(nx::reflect::plain_aggregate<Mixed>);
  CHECK_FALSE(nx::reflect::plain_aggregate<Handle>);
  CHECK_FALSE(nx::reflect::plain_aggregate<int>);
}

TEST_CASE("aggregate: two types of the same shape keep their own names") {
  CHECK(nx::reflect::field_name<AlphaFirst, 0>() == "alpha");
  CHECK(nx::reflect::field_name<BetaFirst, 0>() == "beta");
  CHECK(nx::reflect::field_name<AlphaFirst, 1>() == "on");
  CHECK(nx::reflect::field_name<BetaFirst, 1>() == "on");
}
