
#include "framework/nxtest.h"

#include "core/foundation/serialization/json_util.h"

namespace {

struct Vec2 {
  f32 x = 0.f, y = 0.f;
};
struct Vec4 {
  f32 x = 0.f, y = 0.f, z = 0.f, w = 0.f;
};

enum class Mode { Loop, Once, PingPong };

[[nodiscard]] nx::json::value doc(const nx::string_view text) {
  auto parsed = nx::json::parse(text);
  REQUIRE(parsed.has_value());
  return parsed.value();
}

}

NX_REFLECT_ENUM(Mode, (Loop, "loop"), (Once, "once"), (PingPong, "pingpong"),
                (PingPong, "ping_pong"))

TEST_CASE("json util: a missing key reads as the fallback") {
  const nx::json::value v = doc(R"({ "a": 3 })");
  CHECK(nx::json::f32_or(v, "a", 1.f) == 3.f);
  CHECK(nx::json::f32_or(v, "b", 1.f) == 1.f);
  CHECK(nx::json::u32_or(v, "b", 7u) == 7u);
  CHECK(nx::json::i32_or(v, "b", -7) == -7);
}

TEST_CASE("json util: a key of the wrong type reads as the fallback") {
  const nx::json::value v = doc(R"({ "a": "not a number", "b": [1, 2] })");
  CHECK(nx::json::f32_or(v, "a", 1.5f) == 1.5f);
  CHECK(nx::json::f32_or(v, "b", 1.5f) == 1.5f);
}

TEST_CASE("json util: reading past the end of an array is the fallback") {
  const nx::json::value v = doc(R"({ "a": [10] })");
  CHECK(nx::json::at_or(v["a"], 0, -1.0) == 10.0);
  CHECK(nx::json::at_or(v["a"], 1, -1.0) == -1.0);
  CHECK(nx::json::at_or(v["missing"], 0, -1.0) == -1.0);
}

TEST_CASE("json util: a short vector keeps the rest of its fallback") {
  const nx::json::value v = doc(R"({ "p": [3], "q": [1, 2, 3, 4] })");

  const Vec2 p = nx::json::read_vec2(v, "p", Vec2{9.f, 9.f});
  CHECK(p.x == 3.f);
  CHECK(p.y == 9.f);

  const Vec4 q = nx::json::read_vec4(v, "q", Vec4{});
  CHECK(q.x == 1.f);
  CHECK(q.w == 4.f);

  const Vec2 absent = nx::json::read_vec2(v, "nope", Vec2{5.f, 6.f});
  CHECK(absent.x == 5.f);
  CHECK(absent.y == 6.f);
}

TEST_CASE("json util: vectors round-trip through write and read") {
  nx::json::value root = nx::json::value::make_object();
  root["p"] = nx::json::write_vec2(Vec2{1.5f, -2.5f});
  root["q"] = nx::json::write_vec4(Vec4{1.f, 2.f, 3.f, 4.f});

  const Vec2 p = nx::json::read_vec2(root, "p", Vec2{});
  CHECK(p.x == 1.5f);
  CHECK(p.y == -2.5f);
  const Vec4 q = nx::json::read_vec4(root, "q", Vec4{});
  CHECK(q.z == 3.f);
  CHECK(q.w == 4.f);
}

TEST_CASE("json util: a version at or below the reader's is accepted") {
  CHECK(nx::json::version_ok(doc(R"({ "version": 1 })"), 1));
  CHECK(nx::json::version_ok(doc(R"({ "version": 1 })"), 2));
  CHECK(nx::json::version_ok(doc(R"({ "version": 2 })"), 2));
}

TEST_CASE("json util: a document with no version is accepted") {
  CHECK(nx::json::version_ok(doc(R"({ "a": 1 })"), 1));
  CHECK(nx::json::version_ok(doc(R"({ "version": "one" })"), 1));
  CHECK(nx::json::version_ok(doc(R"([])"), 1));
}

TEST_CASE("json util: a newer version is refused") {
  CHECK_FALSE(nx::json::version_ok(doc(R"({ "version": 2 })"), 1));
  CHECK_FALSE(nx::json::version_ok(doc(R"({ "version": 99 })"), 1));
}

TEST_CASE("json util: an enum parses from any of its spellings") {
  Mode m = Mode::Loop;
  CHECK(nx::json::parse_enum("once", m));
  CHECK(m == Mode::Once);

  CHECK(nx::json::parse_enum("pingpong", m));
  CHECK(m == Mode::PingPong);
  m = Mode::Loop;
  CHECK(nx::json::parse_enum("ping_pong", m));
  CHECK(m == Mode::PingPong);
}

TEST_CASE("json util: an unknown spelling leaves the caller's default alone") {
  Mode m = Mode::Once;
  CHECK_FALSE(nx::json::parse_enum("sideways", m));
  CHECK(m == Mode::Once);
  CHECK_FALSE(nx::json::parse_enum("", m));
  CHECK(m == Mode::Once);
}

TEST_CASE("json util: an enum writes back as its first spelling") {
  CHECK(nx::json::enum_text(Mode::Loop) == "loop");
  CHECK(nx::json::enum_text(Mode::PingPong) == "pingpong");
}

TEST_CASE("json util: an integer the field cannot hold reads as the fallback") {
  const nx::json::value v = doc(
      R"({ "big": 4294967296, "neg": -1, "wide": 2147483648, "low": -2147483649, "ok": -7 })");
  CHECK(nx::json::u32_or(v, "big", 5u) == 5u);
  CHECK(nx::json::u32_or(v, "neg", 5u) == 5u);
  CHECK(nx::json::i32_or(v, "wide", 5) == 5);
  CHECK(nx::json::i32_or(v, "low", 5) == 5);
  CHECK(nx::json::i32_or(v, "ok", 5) == -7);
  CHECK(nx::json::u32_or(v, "big", 0u) == 0u);
}
