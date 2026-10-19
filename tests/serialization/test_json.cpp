#include "framework/nxtest.h"

#include "core/foundation/serialization/json.h"
#include "core/foundation/serialization/reflect_json.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace js = nx::json;

namespace {

js::value ok(nx::string_view text, const js::parse_options &opts = {}) {
  js::result<js::value> r = js::parse(text, opts);
  if (!r) {
    FAIL("expected a successful parse");
    return js::value{};
  }
  return std::move(*r);
}

bool fails_with(nx::string_view text, js::error_code code,
                const js::parse_options &opts = {}) {
  const js::result<js::value> r = js::parse(text, opts);
  return !r && r.error().code == code;
}

struct Vec3 {
  f32 x = 0, y = 0, z = 0;
  bool operator==(const Vec3 &o) const {
    return x == o.x && y == o.y && z == o.z;
  }
};

struct Tag {
  nx::string name;
  int priority = 0;
  bool operator==(const Tag &o) const {
    return name == o.name && priority == o.priority;
  }
};

void to_json(js::value &out, const Tag &t) {
  out = js::value::make_object();
  out.insert("name", js::to_value(t.name));
  out.insert("priority", js::to_value(t.priority));
}

bool from_json(const js::value &in, Tag &out) {
  return in.is_object() && in.get_to("name", out.name) &&
         in.get_to("priority", out.priority);
}

}

template <> struct nx::json::serializer<Vec3> {
  static void to_json(js::value &out, const Vec3 &v) {
    out = js::value::make_array();
    out.reserve(3);
    out.push_back(v.x);
    out.push_back(v.y);
    out.push_back(v.z);
  }
  static bool from_json(const js::value &in, Vec3 &out) {
    if (!in.is_array() || in.size() != 3)
      return false;
    for (usize i = 0; i < 3; ++i)
      if (!in[i].is_number())
        return false;
    out.x = static_cast<f32>(in[0].get_double());
    out.y = static_cast<f32>(in[1].get_double());
    out.z = static_cast<f32>(in[2].get_double());
    return true;
  }
};

TEST_CASE("json::value: default is null and every kind reports itself") {
  const js::value n;
  CHECK(n.is_null());
  CHECK(n.kind() == js::type::null);
  CHECK(n.size() == 0);
  CHECK(n.empty());

  const js::value nn(nullptr);
  CHECK(nn.is_null());

  const js::value b(true);
  CHECK(b.is_bool());
  CHECK(b.as_bool());

  const js::value i(-42);
  CHECK(i.is_i64());
  CHECK(i.is_number());
  CHECK(i.as_i64() == -42);

  const js::value u(42u);
  CHECK(u.is_u64());
  CHECK(u.as_u64() == 42u);

  const js::value d(1.5);
  CHECK(d.is_double());
  CHECK(d.as_double() == 1.5);

  const js::value s("hello");
  CHECK(s.is_string());
  CHECK(s.as_string() == "hello");

  const js::value a = js::value::make_array();
  CHECK(a.is_array());
  CHECK(a.empty());

  const js::value o = js::value::make_object();
  CHECK(o.is_object());
  CHECK(o.empty());
}

TEST_CASE("json::value: forgiving accessors convert or fall back") {
  const js::value i(7);
  CHECK(i.get_i64() == 7);
  CHECK(i.get_u64() == 7u);
  CHECK(i.get_double() == 7.0);
  CHECK(i.get_bool(true));
  CHECK(i.get_string("x") == "x");

  const js::value neg(-1);
  CHECK(neg.get_i64() == -1);
  CHECK(neg.get_u64(99u) == 99u);

  const js::value big(18446744073709551615ull);
  CHECK(big.get_u64() == 18446744073709551615ull);
  CHECK(big.get_i64(-5) == -5);

  const js::value whole(4.0);
  CHECK(whole.get_i64() == 4);
  const js::value frac(4.5);
  CHECK(frac.get_i64(-1) == -1);

  const js::value nul;
  CHECK(nul.get_double(2.5) == 2.5);
  CHECK(nul.get_i64(3) == 3);

  CHECK(i.try_string() == nullptr);
  CHECK(i.try_array() == nullptr);
  CHECK(i.try_object() == nullptr);
  const js::value s("t");
  CHECK(s.try_string() != nullptr);
}

TEST_CASE("json::value: fits reports exact integral representability") {
  const js::value small(200);
  CHECK(small.fits<i32>());
  CHECK(small.fits<u8>());
  CHECK_FALSE(small.fits<i8>());

  const js::value neg(-1);
  CHECK(neg.fits<i8>());
  CHECK_FALSE(neg.fits<u64>());

  const js::value big(4000000000u);
  CHECK(big.fits<u32>());
  CHECK_FALSE(big.fits<i32>());
  CHECK(big.fits<i64>());

  const js::value whole(2.0);
  CHECK_FALSE(whole.fits<i32>());
  const js::value b(true);
  CHECK(b.fits<bool>());
  CHECK_FALSE(b.fits<i32>());
  CHECK_FALSE(small.fits<bool>());
}

TEST_CASE("json::value: object access, insertion and erasure") {
  js::value o = js::value::make_object();
  o.insert("a", 1);
  o.insert("b", "two");
  CHECK(o.size() == 2);
  CHECK(o.contains("a"));
  CHECK_FALSE(o.contains("zz"));
  CHECK(o["a"].as_i64() == 1);
  CHECK(o["b"].as_string() == "two");

  o.insert("a", 9);
  CHECK(o.size() == 2);
  CHECK(o["a"].as_i64() == 9);
  CHECK(o.as_object()[0].key == "a");

  const js::value &co = o;
  CHECK(co["missing"].is_null());
  CHECK(o.size() == 2);
  CHECK(co["missing"]["deeper"].is_null());
  CHECK(co["missing"].get_i64(5) == 5);

  o["c"] = 3;
  CHECK(o.size() == 3);
  CHECK(o["c"].as_i64() == 3);

  CHECK(o.erase("a"));
  CHECK_FALSE(o.contains("a"));
  CHECK(o.size() == 2);
  CHECK_FALSE(o.erase("a"));

  CHECK(o.find("b") != nullptr);
  CHECK(o.find("a") == nullptr);
}

TEST_CASE("json::value: array access and mutation") {
  js::value a = js::value::make_array();
  a.push_back(1);
  a.push_back("two");
  a.push_back(3.5);
  CHECK(a.size() == 3);
  CHECK(a[0].as_i64() == 1);
  CHECK(a[1].as_string() == "two");
  CHECK(a[2].as_double() == 3.5);

  a[0] = 10;
  CHECK(a[0].as_i64() == 10);

  js::value &back = a.emplace_back();
  CHECK(back.is_null());
  CHECK(a.size() == 4);

  a.clear();
  CHECK(a.empty());
  CHECK(a.is_array());
}

TEST_CASE("json::value: a null value becomes a container on first use") {
  js::value o;
  o["k"] = 1;
  CHECK(o.is_object());
  CHECK(o["k"].as_i64() == 1);

  js::value a;
  a.push_back(1);
  CHECK(a.is_array());
  CHECK(a.size() == 1);

  js::value i;
  i.insert("x", 2);
  CHECK(i.is_object());
}

TEST_CASE("json::value: every alternative survives copy and move") {
  const js::value samples[] = {
      js::value{},
      js::value(true),
      js::value(false),
      js::value(-9223372036854775807LL - 1),
      js::value(18446744073709551615ull),
      js::value(-1.25),
      js::value("string alternative long enough to live on the heap"),
      js::value("short"),
      ok("[1,[2],{\"k\":3}]"),
      ok(R"({"a":1,"b":{"c":[true,null]}})"),
  };

  for (const js::value &original : samples) {
    const js::value copied = original;
    REQUIRE(copied.kind() == original.kind());
    REQUIRE(copied == original);

    for (const js::value &seed : samples) {
      js::value dst = seed;
      dst = original;
      REQUIRE(dst.kind() == original.kind());
      REQUIRE(dst == original);
    }

    js::value movable = original;
    const js::value moved = std::move(movable);
    REQUIRE(moved.kind() == original.kind());
    REQUIRE(moved == original);
    REQUIRE(movable.is_null());

    for (const js::value &seed : samples) {
      js::value src = original;
      js::value dst = seed;
      dst = std::move(src);
      REQUIRE(dst.kind() == original.kind());
      REQUIRE(dst == original);
      REQUIRE(src.is_null());
    }

    js::value a = original;
    js::value b("sentinel");
    a.swap(b);
    REQUIRE(b == original);
    REQUIRE(a.as_string() == "sentinel");
    a.swap(b);
    REQUIRE(a == original);
  }
}

TEST_CASE("json::value: copy, move and swap") {
  js::value a = js::value::make_object();
  a.insert("nested", js::value::make_array());
  a["nested"].push_back(1);
  a.insert("s", "text");

  const js::value copy = a;
  CHECK(copy == a);
  CHECK(copy["nested"].size() == 1);

  a["nested"].push_back(2);
  CHECK(copy["nested"].size() == 1);
  CHECK(a["nested"].size() == 2);

  js::value moved = std::move(a);
  CHECK(moved["s"].as_string() == "text");
  CHECK(a.is_null());

  js::value x(1);
  js::value y("two");
  x.swap(y);
  CHECK(x.as_string() == "two");
  CHECK(y.as_i64() == 1);

  js::value s = js::value::make_object();
  s.insert("k", 1);
  const js::value *alias = &s;
  s = *alias;
  CHECK(s["k"].as_i64() == 1);
  js::value *malias = &s;
  s = std::move(*malias);
  CHECK(s["k"].as_i64() == 1);
}

TEST_CASE("json::value: equality is deep and number-aware") {
  CHECK(js::value(1) == js::value(1));
  CHECK(js::value(1) == js::value(1u));
  CHECK(js::value(1) == js::value(1.0));
  CHECK_FALSE(js::value(1) == js::value(2));
  CHECK_FALSE(js::value(-1) == js::value(18446744073709551615ull));

  CHECK(js::value("a") == js::value("a"));
  CHECK_FALSE(js::value("a") == js::value("b"));
  CHECK_FALSE(js::value("1") == js::value(1));
  CHECK(js::value() == js::value());
  CHECK(js::value(true) == js::value(true));
  CHECK_FALSE(js::value(true) == js::value(false));

  CHECK(ok(R"({"a":1,"b":2})") == ok(R"({"b":2,"a":1})"));
  CHECK_FALSE(ok(R"({"a":1})") == ok(R"({"a":1,"b":2})"));
  CHECK_FALSE(ok("[1,2]") == ok("[2,1]"));
  CHECK(ok("[1,[2,{}]]") == ok("[1,[2,{}]]"));
}

TEST_CASE("json::parse: the primitive literals") {
  CHECK(ok("null").is_null());
  CHECK(ok("true").as_bool());
  CHECK_FALSE(ok("false").as_bool());
  CHECK(ok("  \t\r\n null \t ").is_null());
  CHECK(ok("\"\"").as_string().empty());
  CHECK(ok("\"abc\"").as_string() == "abc");
}

TEST_CASE("json::parse: integers keep their exact value and signedness") {
  CHECK(ok("0").as_i64() == 0);
  CHECK(ok("-0").as_i64() == 0);
  CHECK(ok("123").as_i64() == 123);
  CHECK(ok("-123").as_i64() == -123);
  CHECK(ok("9223372036854775807").as_i64() == 9223372036854775807LL);
  CHECK(ok("-9223372036854775808").as_i64() == -9223372036854775807LL - 1);

  const js::value big = ok("18446744073709551615");
  CHECK(big.is_u64());
  CHECK(big.as_u64() == 18446744073709551615ull);

  const js::value id = ok("9007199254740993");
  CHECK(id.is_i64());
  CHECK(id.as_i64() == 9007199254740993LL);
  CHECK(js::write(id) == "9007199254740993");
}

TEST_CASE("json::parse: reals") {
  CHECK(ok("1.5").as_double() == 1.5);
  CHECK(ok("-1.5").as_double() == -1.5);
  CHECK(ok("0.0").as_double() == 0.0);
  CHECK(ok("1e3").as_double() == 1000.0);
  CHECK(ok("1E3").as_double() == 1000.0);
  CHECK(ok("1e+3").as_double() == 1000.0);
  CHECK(ok("1e-3").as_double() == 0.001);
  CHECK(ok("-1.5e2").as_double() == -150.0);
  CHECK(ok("1.0").is_double());
  CHECK(ok("0.5").as_double() == 0.5);

  const js::value huge = ok("1e400");
  CHECK(huge.is_double());
  CHECK(huge.as_double() > 1e308);
}

TEST_CASE("json::parse: arrays") {
  CHECK(ok("[]").size() == 0);
  CHECK(ok("[]").is_array());
  CHECK(ok("  [ ] ").is_array());

  const js::value a = ok("[1, 2, 3]");
  REQUIRE(a.size() == 3);
  CHECK(a[0].as_i64() == 1);
  CHECK(a[2].as_i64() == 3);

  const js::value mixed = ok(R"([null, true, 1, "s", [], {}])");
  REQUIRE(mixed.size() == 6);
  CHECK(mixed[0].is_null());
  CHECK(mixed[1].is_bool());
  CHECK(mixed[2].is_number());
  CHECK(mixed[3].is_string());
  CHECK(mixed[4].is_array());
  CHECK(mixed[5].is_object());

  const js::value nested = ok("[[[[1]]]]");
  CHECK(nested[0][0][0][0].as_i64() == 1);
}

TEST_CASE("json::parse: objects") {
  CHECK(ok("{}").is_object());
  CHECK(ok("{}").size() == 0);

  const js::value o = ok(R"({"a": 1, "b": [2], "c": {"d": null}})");
  REQUIRE(o.size() == 3);
  CHECK(o["a"].as_i64() == 1);
  CHECK(o["b"][0].as_i64() == 2);
  CHECK(o["c"]["d"].is_null());

  const js::value ordered = ok(R"({"z":1,"a":2,"m":3})");
  REQUIRE(ordered.size() == 3);
  CHECK(ordered.as_object()[0].key == "z");
  CHECK(ordered.as_object()[1].key == "a");
  CHECK(ordered.as_object()[2].key == "m");

  const js::value keys = ok(R"({"": 1, "with space": 2, "\u00e9": 3})");
  CHECK(keys[""].as_i64() == 1);
  CHECK(keys["with space"].as_i64() == 2);
  CHECK(keys["\xC3\xA9"].as_i64() == 3);
}

TEST_CASE("json::parse: string escapes") {
  CHECK(ok(R"("\"")").as_string() == "\"");
  CHECK(ok(R"("\\")").as_string() == "\\");
  CHECK(ok(R"("\/")").as_string() == "/");
  CHECK(ok(R"("\b")").as_string() == "\b");
  CHECK(ok(R"("\f")").as_string() == "\f");
  CHECK(ok(R"("\n")").as_string() == "\n");
  CHECK(ok(R"("\r")").as_string() == "\r");
  CHECK(ok(R"("\t")").as_string() == "\t");

  CHECK(ok(R"("\u0041")").as_string() == "A");
  CHECK(ok(R"("\u0000")").as_string().size() == 1);
  CHECK(ok(R"("\u00e9")").as_string() == "\xC3\xA9");
  CHECK(ok(R"("\u20ac")").as_string() == "\xE2\x82\xAC");
  CHECK(ok(R"("\uD83D\uDE00")").as_string() == "\xF0\x9F\x98\x80");

  CHECK(ok(R"("a\nb\tc")").as_string() == "a\nb\tc");
  const nx::string long_run(2000, 'x');
  const nx::string quoted = nx::string("\"") + long_run + "\"";
  CHECK(ok(quoted.view()).as_string() == long_run);

  CHECK(ok("\"caf\xC3\xA9\"").as_string() == "caf\xC3\xA9");
}

TEST_CASE("json::parse: a realistic document") {
  constexpr const char *text = R"({
    "name": "nx2d",
    "version": {"major": 0, "minor": 1, "patch": 0},
    "features": ["vulkan", "ecs", "json"],
    "limits": {"max_entities": 1048576, "gravity": -9.81},
    "debug": false,
    "extra": null
  })";

  const js::value d = ok(text);
  CHECK(d["name"].as_string() == "nx2d");
  CHECK(d["version"]["minor"].as_i64() == 1);
  CHECK(d["features"].size() == 3);
  CHECK(d["features"][2].as_string() == "json");
  CHECK(d["limits"]["max_entities"].as_i64() == 1048576);
  CHECK(d["limits"]["gravity"].as_double() == -9.81);
  CHECK_FALSE(d["debug"].as_bool());
  CHECK(d["extra"].is_null());

  CHECK(ok(js::write(d).view()) == d);
  CHECK(ok(js::write(d, {.indent = 2}).view()) == d);
}

TEST_CASE("json::parse: empty and whitespace-only input is rejected") {
  CHECK(fails_with("", js::error_code::unexpected_end));
  CHECK(fails_with("   ", js::error_code::unexpected_end));
  CHECK(fails_with("\n\t ", js::error_code::unexpected_end));
}

TEST_CASE("json::parse: malformed literals are rejected") {
  CHECK(fails_with("tru", js::error_code::unexpected_token));
  CHECK(fails_with("truex", js::error_code::trailing_content));
  CHECK(fails_with("nul", js::error_code::unexpected_token));
  CHECK(fails_with("False", js::error_code::unexpected_token));
  CHECK(fails_with("undefined", js::error_code::unexpected_token));
  CHECK(fails_with("'single'", js::error_code::unexpected_token));
}

TEST_CASE("json::parse: malformed numbers are rejected") {
  CHECK(fails_with("01", js::error_code::invalid_number));
  CHECK(fails_with("-", js::error_code::invalid_number));
  CHECK(fails_with("+1", js::error_code::unexpected_token));
  CHECK(fails_with("1.", js::error_code::invalid_number));
  CHECK(fails_with(".1", js::error_code::unexpected_token));
  CHECK(fails_with("1e", js::error_code::invalid_number));
  CHECK(fails_with("1e+", js::error_code::invalid_number));
  CHECK(fails_with("1.2.3", js::error_code::invalid_number));
  CHECK(fails_with("--1", js::error_code::unexpected_token));
  CHECK(fails_with("1x", js::error_code::invalid_number));
  CHECK(fails_with("0x10", js::error_code::invalid_number));
  CHECK(fails_with("Infinity", js::error_code::unexpected_token));
  CHECK(fails_with("NaN", js::error_code::unexpected_token));
}

TEST_CASE("json::parse: malformed strings are rejected") {
  CHECK(fails_with("\"unterminated", js::error_code::invalid_string));
  CHECK(fails_with("\"", js::error_code::invalid_string));
  CHECK(fails_with("\"a\nb\"", js::error_code::invalid_string));
  CHECK(fails_with(nx::string_view("\"a\x01\x62\"", 5),
                   js::error_code::invalid_string));

  CHECK(fails_with(R"("\q")", js::error_code::invalid_escape));
  CHECK(fails_with(R"("\")", js::error_code::invalid_string));
  CHECK(fails_with(R"("\u00")", js::error_code::invalid_escape));
  CHECK(fails_with(R"("\uZZZZ")", js::error_code::invalid_escape));
  CHECK(fails_with("\"\\u00", js::error_code::unexpected_end));
}

TEST_CASE("json::parse: unpaired surrogates are rejected") {
  CHECK(fails_with(R"("\uD800")", js::error_code::invalid_surrogate));
  CHECK(fails_with(R"("\uDC00")", js::error_code::invalid_surrogate));
  CHECK(fails_with(R"("\uD800x")", js::error_code::invalid_surrogate));
  CHECK(fails_with(R"("\uD800\u0041")", js::error_code::invalid_surrogate));
  CHECK(fails_with(R"("\uD800\uD800")", js::error_code::invalid_surrogate));
  CHECK(ok(R"("\uD800\uDC00")").as_string().size() == 4);
}

TEST_CASE("json::parse: malformed arrays and objects are rejected") {
  CHECK(fails_with("[", js::error_code::unexpected_end));
  CHECK(fails_with("[1", js::error_code::unexpected_end));
  CHECK(fails_with("[1,", js::error_code::unexpected_end));
  CHECK(fails_with("[1,]", js::error_code::unexpected_token));
  CHECK(fails_with("[,1]", js::error_code::unexpected_token));
  CHECK(fails_with("[1 2]", js::error_code::unexpected_token));
  CHECK(fails_with("]", js::error_code::unexpected_token));

  CHECK(fails_with("{", js::error_code::unexpected_end));
  CHECK(fails_with("{\"a\"", js::error_code::unexpected_end));
  CHECK(fails_with("{\"a\":", js::error_code::unexpected_end));
  CHECK(fails_with("{\"a\":1", js::error_code::unexpected_end));
  CHECK(fails_with("{\"a\":1,}", js::error_code::unexpected_token));
  CHECK(fails_with("{a:1}", js::error_code::unexpected_token));
  CHECK(fails_with("{1:2}", js::error_code::unexpected_token));
  CHECK(fails_with("{\"a\" 1}", js::error_code::unexpected_token));
  CHECK(fails_with("{\"a\":1 \"b\":2}", js::error_code::unexpected_token));
  CHECK(fails_with("}", js::error_code::unexpected_token));
}

TEST_CASE("json::parse: trailing content is rejected") {
  CHECK(fails_with("1 2", js::error_code::trailing_content));
  CHECK(fails_with("{} {}", js::error_code::trailing_content));
  CHECK(fails_with("[1] x", js::error_code::trailing_content));
  CHECK(fails_with("null null", js::error_code::trailing_content));
  CHECK(ok("1  \n ").as_i64() == 1);
}

TEST_CASE("json::parse: nesting past the limit is a clean error") {
  nx::string deep;
  constexpr int depth = 20000;
  for (int i = 0; i < depth; ++i)
    deep.push_back('[');
  for (int i = 0; i < depth; ++i)
    deep.push_back(']');
  CHECK(fails_with(deep.view(), js::error_code::depth_exceeded));

  nx::string deep_obj;
  for (int i = 0; i < depth; ++i)
    deep_obj.append("{\"a\":");
  deep_obj.push_back('1');
  for (int i = 0; i < depth; ++i)
    deep_obj.push_back('}');
  CHECK(fails_with(deep_obj.view(), js::error_code::depth_exceeded));

  js::parse_options opts;
  opts.max_depth = 40;
  nx::string shallow;
  for (int i = 0; i < 30; ++i)
    shallow.push_back('[');
  for (int i = 0; i < 30; ++i)
    shallow.push_back(']');
  CHECK(js::parse(shallow.view(), opts).has_value());

  opts.max_depth = 10;
  CHECK(fails_with(shallow.view(), js::error_code::depth_exceeded, opts));
}

TEST_CASE("json::parse: errors carry a useful position") {
  const js::result<js::value> r = js::parse("{\n  \"a\": tru\n}");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == js::error_code::unexpected_token);
  CHECK(r.error().line == 2);
  CHECK(r.error().column == 8);
  CHECK(r.error().offset == 9);
  CHECK(r.error().message().view().starts_with("2:8: "));

  const js::result<js::value> r2 = js::parse("@");
  REQUIRE_FALSE(r2.has_value());
  CHECK(r2.error().line == 1);
  CHECK(r2.error().column == 1);
  CHECK(r2.error().offset == 0);

  for (u32 i = 0; i < static_cast<u32>(js::error_code::count); ++i)
    CHECK_FALSE(js::to_string(static_cast<js::error_code>(i)).empty());
}

TEST_CASE("json::parse: trailing commas are opt-in") {
  js::parse_options opts;
  opts.allow_trailing_commas = true;

  CHECK(js::parse("[1,2,]", opts).has_value());
  CHECK(js::parse("{\"a\":1,}", opts).has_value());
  CHECK(ok("[1,2,]", opts).size() == 2);
  CHECK(fails_with("[1,2,]", js::error_code::unexpected_token));
  CHECK(!js::parse("[,]", opts).has_value());
}

TEST_CASE("json::parse: comments are opt-in") {
  js::parse_options opts;
  opts.allow_comments = true;

  CHECK(ok("[1] // trailing", opts).size() == 1);
  CHECK(ok("// leading\n[1]", opts).size() == 1);
  CHECK(ok("[1 /* inline */, 2]", opts).size() == 2);
  CHECK(ok("{/*a*/\"k\"/*b*/:/*c*/1/*d*/}", opts).size() == 1);
  CHECK(ok("[1/*x*/,2]", opts).size() == 2);
  CHECK(ok("/*only a comment*/ 5", opts).as_i64() == 5);

  CHECK(fails_with("[1] // x", js::error_code::trailing_content));
  CHECK(!js::parse("[1] /", opts).has_value());
}

TEST_CASE("json::parse: duplicate keys, last one wins unless rejected") {
  const js::value d = ok(R"({"a":1,"a":2})");
  CHECK(d.size() == 1);
  CHECK(d["a"].as_i64() == 2);

  js::parse_options opts;
  opts.reject_duplicate_keys = true;
  CHECK(fails_with(R"({"a":1,"a":2})", js::error_code::duplicate_key, opts));
  CHECK(js::parse(R"({"a":1,"b":2})", opts).has_value());
}

TEST_CASE("json::parse: UTF-8 validation is opt-in") {
  const char bad[] = {'"', '\xC3', '"', '\0'};
  js::parse_options opts;
  opts.validate_utf8 = true;
  CHECK(
      fails_with(nx::string_view(bad, 3), js::error_code::invalid_utf8, opts));
  CHECK(js::parse("\"caf\xC3\xA9\"", opts).has_value());
}

TEST_CASE("json::write: compact output of every kind") {
  CHECK(js::write(js::value()) == "null");
  CHECK(js::write(js::value(true)) == "true");
  CHECK(js::write(js::value(false)) == "false");
  CHECK(js::write(js::value(42)) == "42");
  CHECK(js::write(js::value(-42)) == "-42");
  CHECK(js::write(js::value(18446744073709551615ull)) ==
        "18446744073709551615");
  CHECK(js::write(js::value("hi")) == "\"hi\"");
  CHECK(js::write(js::value::make_array()) == "[]");
  CHECK(js::write(js::value::make_object()) == "{}");
  CHECK(js::write(ok("[1,2,3]")) == "[1,2,3]");
  CHECK(js::write(ok(R"({"a":1,"b":[2,{}]})")) == R"({"a":1,"b":[2,{}]})");
}

TEST_CASE("json::write: reals keep their type across a round trip") {
  CHECK(js::write(js::value(1.0)) == "1.0");
  CHECK(js::write(js::value(-2.0)) == "-2.0");
  CHECK(ok(js::write(js::value(1.0)).view()).is_double());

  CHECK(js::write(js::value(1.5)) == "1.5");
  CHECK(js::write(js::value(0.0)) == "0.0");

  for (const f64 d : {0.1, 1.0 / 3.0, 1e-300, 1e300, -123.456789012345}) {
    const nx::string text = js::write(js::value(d));
    const js::value back = ok(text.view());
    REQUIRE(back.is_double());
    REQUIRE(back.as_double() == d);
  }

  const f64 inf = std::numeric_limits<f64>::infinity();
  CHECK(js::write(js::value(inf)) == "null");
  CHECK(js::write(js::value(-inf)) == "null");
  CHECK(js::write(js::value(std::numeric_limits<f64>::quiet_NaN())) == "null");

  CHECK(js::write(js::value(1.0 / 3.0), {.real_precision = 3}) == "0.333");
}

TEST_CASE("json::write: strings are escaped correctly") {
  CHECK(js::write(js::value("a\"b")) == R"("a\"b")");
  CHECK(js::write(js::value("a\\b")) == R"("a\\b")");
  CHECK(js::write(js::value("\b\f\n\r\t")) == R"("\b\f\n\r\t")");

  const nx::string ctrl("\x01\x1f", usize(2));
  CHECK(js::write(js::value(ctrl)) == R"("\u0001\u001f")");

  CHECK(js::write(js::value("a/b")) == "\"a/b\"");
  CHECK(js::write(js::value("a/b"), {.escape_forward_slash = true}) ==
        R"("a\/b")");

  CHECK(js::write(js::value("caf\xC3\xA9")) == "\"caf\xC3\xA9\"");
  CHECK(js::write(js::value("caf\xC3\xA9"), {.escape_non_ascii = true}) ==
        R"("caf\u00e9")");
  CHECK(js::write(js::value("\xF0\x9F\x98\x80"), {.escape_non_ascii = true}) ==
        R"("\ud83d\ude00")");
  CHECK(ok(js::write(js::value("\xF0\x9F\x98\x80"), {.escape_non_ascii = true})
               .view())
            .as_string() == "\xF0\x9F\x98\x80");

  const nx::string with_nul("a\0b", usize(3));
  CHECK(js::write(js::value(with_nul)) == R"("a\u0000b")");
  CHECK(ok(js::write(js::value(with_nul)).view()).as_string().size() == 3);
}

TEST_CASE("json::write: pretty output nests and indents") {
  const js::value v = ok(R"({"a":1,"b":[1,2],"c":{"d":null}})");
  const nx::string pretty = js::write(v, {.indent = 2});

  CHECK(pretty == "{\n"
                  "  \"a\": 1,\n"
                  "  \"b\": [\n"
                  "    1,\n"
                  "    2\n"
                  "  ],\n"
                  "  \"c\": {\n"
                  "    \"d\": null\n"
                  "  }\n"
                  "}");
  CHECK(ok(pretty.view()) == v);

  CHECK(js::write(ok(R"({"a":[],"b":{}})"), {.indent = 2}) ==
        "{\n  \"a\": [],\n  \"b\": {}\n}");

  CHECK(js::write(ok("[1]"), {.indent = 4}) == "[\n    1\n]");
}

TEST_CASE("json::write_to appends rather than replacing") {
  nx::string out("prefix:");
  js::write_to(out, js::value(1));
  js::write_to(out, js::value(2));
  CHECK(out == "prefix:12");
}

TEST_CASE("json::write: round-trips a document with every feature") {
  constexpr const char *text =
      R"({"n":null,"t":true,"f":false,"i":-7,"u":18446744073709551615,)"
      R"("d":1.25,"s":"esc \" \\ \n \u00e9","a":[1,[2],{"k":3}],"o":{}})";
  const js::value once = ok(text);
  const nx::string written = js::write(once);
  const js::value twice = ok(written.view());

  CHECK(once == twice);
  CHECK(js::write(twice) == written);

  CHECK(ok(js::write(once, {.indent = 3}).view()) == once);
}

TEST_CASE("json: a trait specialization registers a custom type") {
  const Vec3 v{1.0f, 2.5f, -3.0f};
  const js::value encoded = js::to_value(v);
  REQUIRE(encoded.is_array());
  CHECK(encoded.size() == 3);
  CHECK(js::write(encoded) == "[1.0,2.5,-3.0]");

  const js::result<Vec3> decoded = js::from_value<Vec3>(encoded);
  REQUIRE(decoded.has_value());
  CHECK(*decoded == v);

  CHECK(js::write_as(v) == "[1.0,2.5,-3.0]");
  const js::result<Vec3> parsed = js::parse_as<Vec3>("[1, 2.5, -3]");
  REQUIRE(parsed.has_value());
  CHECK(*parsed == v);

  CHECK_FALSE(js::from_value<Vec3>(ok("[1,2]")).has_value());
  CHECK_FALSE(js::from_value<Vec3>(ok("{}")).has_value());
  CHECK(js::from_value<Vec3>(ok(R"([1,"x",3])")).error().code ==
        js::error_code::type_mismatch);
  CHECK_FALSE(js::parse_as<Vec3>("not json").has_value());
}

TEST_CASE("json: an ADL-found to_json/from_json registers a custom type") {
  const Tag t{nx::string("render"), 5};
  const nx::string text = js::write_as(t);
  CHECK(text == R"({"name":"render","priority":5})");

  const js::result<Tag> back = js::parse_as<Tag>(text.view());
  REQUIRE(back.has_value());
  CHECK(*back == t);

  CHECK_FALSE(js::parse_as<Tag>(R"({"name":"x"})").has_value());
  CHECK_FALSE(js::parse_as<Tag>(R"({"priority":1})").has_value());
  CHECK_FALSE(js::parse_as<Tag>(R"({"name":1,"priority":1})").has_value());

  static_assert(js::convertible<Tag>);
  static_assert(js::convertible<Vec3>);
}

TEST_CASE("json: built-in serializers cover the primitives") {
  CHECK(js::write_as(true) == "true");
  CHECK(js::write_as(42) == "42");
  CHECK(js::write_as(42u) == "42");
  CHECK(js::write_as(-1) == "-1");
  CHECK(js::write_as(1.5) == "1.5");
  CHECK(js::write_as(nx::string("s")) == "\"s\"");
  CHECK(js::write_as(std::string("s")) == "\"s\"");

  CHECK(*js::parse_as<bool>("true"));
  CHECK(*js::parse_as<i32>("-5") == -5);
  CHECK(*js::parse_as<u32>("5") == 5u);
  CHECK(*js::parse_as<f64>("1.5") == 1.5);
  CHECK(*js::parse_as<f32>("2") == 2.0f);
  CHECK(*js::parse_as<nx::string>("\"x\"") == "x");
  CHECK(*js::parse_as<std::string>("\"x\"") == "x");

  CHECK_FALSE(js::parse_as<u32>("-1").has_value());
  CHECK_FALSE(js::parse_as<i8>("300").has_value());
  CHECK_FALSE(js::parse_as<i32>("2147483648").has_value());
  CHECK(js::parse_as<i32>("2147483647").has_value());
  CHECK_FALSE(js::parse_as<i32>("1.5").has_value());
  CHECK_FALSE(js::parse_as<bool>("1").has_value());
  CHECK_FALSE(js::parse_as<nx::string>("1").has_value());
}

TEST_CASE("json: containers and optionals round-trip") {
  const nx::vector<i32> ints{1, 2, 3};
  CHECK(js::write_as(ints) == "[1,2,3]");
  const js::result<nx::vector<i32>> back =
      js::parse_as<nx::vector<i32>>("[1,2,3]");
  REQUIRE(back.has_value());
  CHECK(back->size() == 3);
  CHECK((*back)[2] == 3);

  const nx::vector<Vec3> vecs{Vec3{1, 0, 0}, Vec3{0, 1, 0}};
  const nx::string text = js::write_as(vecs);
  CHECK(text == "[[1.0,0.0,0.0],[0.0,1.0,0.0]]");
  const js::result<nx::vector<Vec3>> vback =
      js::parse_as<nx::vector<Vec3>>(text.view());
  REQUIRE(vback.has_value());
  CHECK(vback->size() == 2);
  CHECK((*vback)[1] == vecs[1]);

  CHECK_FALSE(js::parse_as<nx::vector<i32>>("[1,\"x\"]").has_value());
  CHECK_FALSE(js::parse_as<nx::vector<i32>>("{}").has_value());
  CHECK(js::parse_as<nx::vector<i32>>("[]")->empty());

  const std::optional<i32> some = 7;
  const std::optional<i32> none;
  CHECK(js::write_as(some) == "7");
  CHECK(js::write_as(none) == "null");
  CHECK(**js::parse_as<std::optional<i32>>("7") == 7);
  CHECK_FALSE(js::parse_as<std::optional<i32>>("null")->has_value());
  CHECK_FALSE(js::parse_as<std::optional<i32>>("\"x\"").has_value());

  const js::result<js::value> arr = js::parse("[1,2,3]");
  REQUIRE(arr.has_value());
  i32 fixed[3] = {};
  CHECK(js::extract(*arr, fixed));
  CHECK(fixed[2] == 3);
  CHECK_FALSE(js::extract(ok("[1,2]"), fixed));
}

TEST_CASE("json: get_to reads members through the trait system") {
  const js::value o = ok(R"({"n":5,"s":"x","v":[1,2,3],"bad":"nope"})");

  i32 n = 0;
  CHECK(o.get_to("n", n));
  CHECK(n == 5);

  nx::string s;
  CHECK(o.get_to("s", s));
  CHECK(s == "x");

  nx::vector<i32> v;
  CHECK(o.get_to("v", v));
  CHECK(v.size() == 3);

  i32 untouched = 99;
  CHECK_FALSE(o.get_to("missing", untouched));
  CHECK(untouched == 99);
  CHECK_FALSE(o.get_to("bad", untouched));
  CHECK(untouched == 99);

  CHECK_FALSE(ok("[1]").get_to("k", n));
}

TEST_CASE("json: a large generated document round-trips exactly") {
  js::value root = js::value::make_object();
  js::value items = js::value::make_array();
  items.reserve(2000);

  for (int i = 0; i < 2000; ++i) {
    js::value item = js::value::make_object();
    item.insert("id", i);
    item.insert("name", nx::string("item_") + nx::string_view("x"));
    item.insert("ratio", static_cast<f64>(i) / 7.0);
    item.insert("flag", (i % 2) == 0);
    js::value tags = js::value::make_array();
    for (int t = 0; t < i % 5; ++t)
      tags.push_back(t);
    item.insert("tags", std::move(tags));
    items.push_back(std::move(item));
  }
  root.insert("items", std::move(items));
  root.insert("count", 2000);

  const nx::string text = js::write(root);
  const js::value parsed = ok(text.view());
  CHECK(parsed == root);
  CHECK(parsed["items"].size() == 2000);
  CHECK(parsed["items"][1999]["id"].as_i64() == 1999);
  CHECK(js::write(parsed) == text);

  const nx::string pretty = js::write(root, {.indent = 2});
  CHECK(ok(pretty.view()) == root);
  CHECK(pretty.size() > text.size());
}

TEST_CASE("json: a large object parses correctly and in linear time") {
  constexpr int n = 20000;
  nx::string text("{");
  for (int i = 0; i < n; ++i) {
    if (i != 0)
      text.push_back(',');
    text.append("\"key_");
    char buf[16];
    const auto r = std::to_chars(buf, buf + sizeof(buf), i);
    text.append(buf, static_cast<usize>(r.ptr - buf));
    text.append("\":");
    const auto r2 = std::to_chars(buf, buf + sizeof(buf), i);
    text.append(buf, static_cast<usize>(r2.ptr - buf));
  }
  text.push_back('}');

  const js::value o = ok(text.view());
  REQUIRE(o.is_object());
  REQUIRE(o.size() == static_cast<usize>(n));
  for (int i = 0; i < n; ++i) {
    nx::string key("key_");
    char buf[16];
    const auto r = std::to_chars(buf, buf + sizeof(buf), i);
    key.append(buf, static_cast<usize>(r.ptr - buf));
    REQUIRE(o[key.view()].as_i64() == i);
  }
  CHECK(o["key_1"].as_i64() == 1);
  CHECK(o["key_10"].as_i64() == 10);
  CHECK(o["key_100"].as_i64() == 100);
  CHECK_FALSE(o.contains("key_"));
  CHECK_FALSE(o.contains("key_200000"));

  CHECK(ok(js::write(o).view()) == o);
}

TEST_CASE(
    "json: duplicate keys resolve the same way past the index threshold") {
  CHECK(ok(R"({"a":1,"a":2})").size() == 1);
  CHECK(ok(R"({"a":1,"a":2})")["a"].as_i64() == 2);

  nx::string text("{");
  for (int i = 0; i < 200; ++i) {
    if (i != 0)
      text.push_back(',');
    text.append("\"k");
    char buf[16];
    const auto r = std::to_chars(buf, buf + sizeof(buf), i);
    text.append(buf, static_cast<usize>(r.ptr - buf));
    text.append("\":1");
  }
  text.append(",\"k3\":99,\"k199\":98}");

  const js::value o = ok(text.view());
  CHECK(o.size() == 200);
  CHECK(o["k3"].as_i64() == 99);
  CHECK(o["k199"].as_i64() == 98);
  CHECK(o["k0"].as_i64() == 1);

  js::parse_options strict;
  strict.reject_duplicate_keys = true;
  CHECK(fails_with(text.view(), js::error_code::duplicate_key, strict));
}

TEST_CASE("json: maps and small_vector round-trip") {
  nx::flat_map<nx::string, i32> fm;
  fm.insert_or_assign(nx::string("b"), 2);
  fm.insert_or_assign(nx::string("a"), 1);
  const nx::string text = js::write_as(fm);
  CHECK(text == R"({"a":1,"b":2})");

  const js::result<nx::flat_map<nx::string, i32>> back =
      js::parse_as<nx::flat_map<nx::string, i32>>(text.view());
  REQUIRE(back.has_value());
  CHECK(back->size() == 2);
  CHECK(back->at(nx::string("a")) == 1);

  nx::hash_map<nx::string, i32> hm;
  hm.insert_or_assign(nx::string("x"), 7);
  const js::result<nx::hash_map<nx::string, i32>> hback =
      js::parse_as<nx::hash_map<nx::string, i32>>(js::write_as(hm).view());
  REQUIRE(hback.has_value());
  CHECK(hback->at(nx::string("x")) == 7);

  CHECK_FALSE(js::parse_as<nx::flat_map<nx::string, i32>>("[]").has_value());
  CHECK_FALSE(
      js::parse_as<nx::flat_map<nx::string, i32>>(R"({"a":"x"})").has_value());

  nx::small_vector<i32, 4> sv;
  sv.push_back(1);
  sv.push_back(2);
  CHECK(js::write_as(sv) == "[1,2]");
  CHECK(js::parse_as<nx::small_vector<i32, 4>>("[1,2,3,4,5,6]")->size() == 6);
}

TEST_CASE("json::write: precision is clamped, never dropped to null") {
  const nx::string s = js::write(js::value(1.5), {.real_precision = 100000});
  CHECK(s != "null");
  CHECK(s.view().starts_with("1.5"));
  CHECK(ok(s.view()).get_double() == 1.5);

  const nx::string big = js::write(js::value(1e308), {.real_precision = 40});
  CHECK(big != "null");
  CHECK(ok(big.view()).get_double() > 1e307);
}

TEST_CASE("json: parsing never leaves a document half-built on failure") {
  const char *inputs[] = {
      "[",
      "[[",
      "[[[1",
      "{\"a\":[1,",
      "{\"a\":{\"b\":",
      "[1,2,{\"c\"",
      "[\"\\u12",
      "{\"a\":tr",
      "[1e",
      "[-",
      "{\"\\ud800\":1}",
  };
  for (const char *text : inputs) {
    const js::result<js::value> r = js::parse(text);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code != js::error_code::none);
  }
}

TEST_CASE("json: mutators turn a value of the wrong kind into the right one") {
  nx::json::value number = nx::json::parse("5").value();
  number["key"] = nx::json::parse("1").value();
  REQUIRE(number.is_object());
  CHECK(number.size() == 1);

  nx::json::value text = nx::json::parse(R"("words")").value();
  text.push_back(nx::json::parse("2").value());
  REQUIRE(text.is_array());
  CHECK(text.size() == 1);

  nx::json::value list = nx::json::parse("[1, 2]").value();
  list.insert("k", nx::json::parse("true").value());
  REQUIRE(list.is_object());
  CHECK(list.contains("k"));
}

TEST_CASE("json: escaped output of invalid UTF-8 parses back") {
  nx::json::value v = nx::json::parse(R"({"s": ""})").value();
  v["s"] = nx::json::value(nx::string("a\xED\xA0\x80\xF4\x90\x80\x80z"));
  const nx::string written = nx::json::write(v, {.escape_non_ascii = true});
  CHECK(nx::json::parse(written).has_value());
}

TEST_CASE("json: validate_utf8 rejects overlong forms and surrogates") {
  for (const char *bad :
       {"\"\xC0\xAE\"", "\"\xED\xA0\x80\"", "\"\xF4\x90\x80\x80\""})
    CHECK_FALSE(nx::json::parse(bad, {.validate_utf8 = true}).has_value());
  CHECK(nx::json::parse("\"\xF0\x9F\x98\x80\"", {.validate_utf8 = true})
            .has_value());
}

TEST_CASE("json::parse: out-of-range numbers saturate the same everywhere") {
  CHECK(std::isinf(ok("1e400").as_double()));
  CHECK(ok("-1e400").as_double() < -1e308);
  CHECK(ok("1e-400").as_double() == 0.0);
}

namespace json_reflect_narrow {

enum class Shade : u8 { Dark, Light };

struct Narrow {
  u8 small = 1;
  i8 tiny = 1;
  Shade shade = Shade::Light;
};

} // namespace json_reflect_narrow

NX_REFLECT_ENUM(json_reflect_narrow::Shade, (Dark, "dark"), (Light, "light"))

TEST_CASE("json reflect: an integer the field cannot hold leaves it alone") {
  using json_reflect_narrow::Narrow;
  using json_reflect_narrow::Shade;
  Narrow n;
  nx::json::read_fields(ok(R"({ "small": 300, "tiny": -129, "shade": 256 })"),
                        n);
  CHECK(n.small == 1);
  CHECK(n.tiny == 1);
  CHECK(n.shade == Shade::Light);

  nx::json::read_fields(ok(R"({ "small": 255, "tiny": -128, "shade": 0 })"), n);
  CHECK(n.small == 255);
  CHECK(n.tiny == -128);
  CHECK(n.shade == Shade::Dark);
}
