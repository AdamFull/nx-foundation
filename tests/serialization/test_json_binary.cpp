
#include "framework/nxtest.h"

#include "core/foundation/serialization/asset_policy.h"
#include "core/foundation/serialization/json.h"
#include "core/foundation/serialization/json_binary.h"
#include "core/foundation/serialization/json_document.h"
#include "core/foundation/serialization/nva.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/vfs/vfs.h"

#include "../vfs/vfs_fixture.h"

namespace {

[[nodiscard]] nx::json::value roundtrip(const nx::json::value &v) {
  nx::nva::Writer w;
  nx::json::write_binary(v, w);
  nx::nva::Reader r(w.span());
  return nx::json::read_binary(r);
}

} // namespace

TEST_CASE("json binary: a value tree round-trips, matching the source") {
  const auto doc = nx::json::parse(R"({
    "nil": null, "flag": true, "neg": -42, "big": 4000000000,
    "pi": 3.14159, "text": "hello world",
    "empty_arr": [], "empty_obj": {},
    "arr": [1, 2.5, "three", false, null],
    "obj": {"nested": {"deep": [true, {"x": 1}]}}
  })");
  REQUIRE(doc);

  nx::nva::Writer w;
  nx::json::write_binary(doc.value(), w);
  nx::nva::Reader r(w.span());
  const nx::json::value decoded = nx::json::read_binary(r);

  CHECK(r.ok());
  CHECK(decoded == doc.value());
}

TEST_CASE("json binary: the three number kinds keep their representation") {
  CHECK(roundtrip(nx::json::value(i64{-5})).is_i64());
  CHECK(roundtrip(nx::json::value(u64{5})).is_u64());

  const nx::json::value real = roundtrip(nx::json::value(2.5));
  CHECK(real.is_double());
  CHECK(real.as_double() == 2.5);

  CHECK(roundtrip(nx::json::value(u64{0xFFFFFFFFFFFFFFFFull})).as_u64() ==
        0xFFFFFFFFFFFFFFFFull);
}

TEST_CASE("json binary: a truncated buffer fails rather than reading junk") {
  const auto doc =
      nx::json::parse(R"({"a": [1, 2, 3, 4, 5], "b": "some text"})");
  REQUIRE(doc);
  nx::nva::Writer w;
  nx::json::write_binary(doc.value(), w);

  const std::span<const u8> full = w.span();
  nx::nva::Reader r(full.subspan(0, full.size() / 2));
  (void)nx::json::read_binary(r);
  CHECK_FALSE(r.ok());
}

TEST_CASE("json binary: an impossible collection count is rejected") {
  nx::nva::Writer w;
  w.u8v(nx::cast<u8>(nx::json::type::array));
  w.u32v(~u32{0});

  nx::nva::Reader r(w.span());
  (void)nx::json::read_binary(r);
  CHECK_FALSE(r.ok());
}

TEST_CASE(
    "json binary: a repeated key keeps its last value in a large object") {
  constexpr u32 KEYS = 4096;
  nx::nva::Writer w;
  w.u8v(nx::cast<u8>(nx::json::type::object));
  w.u32v(KEYS + 2);
  for (u32 i = 0; i < KEYS; ++i) {
    w.str(nx::format("key.{}", i).view());
    w.u8v(nx::cast<u8>(nx::json::type::unsigned_integer));
    w.u64v(i);
  }
  for (const u32 repeated : {u32{3}, KEYS - 1}) {
    w.str(nx::format("key.{}", repeated).view());
    w.u8v(nx::cast<u8>(nx::json::type::unsigned_integer));
    w.u64v(KEYS + repeated);
  }

  nx::nva::Reader r(w.span());
  const nx::json::value decoded = nx::json::read_binary(r);
  REQUIRE(r.ok());
  REQUIRE(decoded.size() == KEYS);
  CHECK(decoded["key.3"].as_u64() == KEYS + 3);
  CHECK(decoded["key.4095"].as_u64() == KEYS + KEYS - 1);
  CHECK(decoded["key.2048"].as_u64() == 2048);
}

TEST_CASE("json binary: nesting has a finite decode depth") {
  nx::nva::Writer w;
  for (u32 i = 0; i < 130; ++i) {
    w.u8v(nx::cast<u8>(nx::json::type::array));
    w.u32v(1);
  }
  w.u8v(nx::cast<u8>(nx::json::type::null));

  nx::nva::Reader r(w.span());
  (void)nx::json::read_binary(r);
  CHECK_FALSE(r.ok());
}

TEST_CASE("json document: a value tree round-trips through its container") {
  const auto source = nx::json::parse(
      R"({"name":"shipping","values":[1,true,null,{"x":2.5}]})");
  REQUIRE(source);
  const nx::vector<u8> bytes = nx::json::encode_document(source.value());

  nx::json::value decoded;
  REQUIRE(nx::json::decode_document({bytes.data(), bytes.size()}, decoded));
  CHECK(decoded == source.value());
}

TEST_CASE("json document: asset parsing removes recursive authoring comments") {
  auto parsed = nx::json::parse_asset_document(R"({
    // Syntax comments are accepted but never represented.
    "_": ["root note"],
    "kept": {
      "value": 7,
      "_note": "nested note",
      "$comment": "another reserved spelling",
    },
    "items": [
      {"name": "first", "_debug": true},
      {"name": "second"},
    ],
  })");
  REQUIRE(parsed);
  CHECK_FALSE(parsed->contains("_"));
  REQUIRE(parsed->contains("kept"));
  CHECK(parsed->operator[]("kept").operator[]("value").get_u64() == 7u);
  CHECK_FALSE(parsed->operator[]("kept").contains("_note"));
  CHECK_FALSE(parsed->operator[]("kept").contains("$comment"));
  REQUIRE(parsed->operator[]("items").size() == 2u);
  CHECK_FALSE(parsed->operator[]("items")[0].contains("_debug"));
  CHECK(parsed->operator[]("items")[1].operator[]("name").get_string() ==
        "second");
}

TEST_CASE("json document: normalized asset JSON is strict and comment-free") {
  const auto normalized = nx::json::normalize_asset_document(
      R"({"value": 3, /* author note */ "_comment": "drop",})");
  REQUIRE(normalized);
  CHECK(normalized->find("comment") == nx::string::npos);
  const auto strict = nx::json::parse(normalized->view());
  REQUIRE(strict);
  CHECK(strict->operator[]("value").get_u64() == 3u);

  const auto formatted = nx::json::normalize_asset_document(
      R"({"nested":{"value":3},"_comment":"drop"})", {.indent = 2});
  REQUIRE(formatted);
  CHECK(formatted->find("comment") == nx::string::npos);
  CHECK(formatted->find("3\n  }") != nx::string::npos);
}

TEST_CASE(
    "json document: malformed bytes are rejected without changing output") {
  const auto source = nx::json::parse(R"({"ok":true})");
  REQUIRE(source);
  nx::vector<u8> bytes = nx::json::encode_document(source.value());

  nx::json::value untouched("kept");
  REQUIRE(bytes.size() > 1u);
  CHECK_FALSE(
      nx::json::decode_document({bytes.data(), bytes.size() - 1u}, untouched));
  CHECK(untouched.get_string() == "kept");
}

TEST_CASE(
    "asset policy: development may use source only when cooked is absent") {
  CHECK(nx::asset_policy::authored_source_fallback_enabled);
  CHECK(nx::asset_policy::can_fallback_to_authored_source(false));
  CHECK_FALSE(nx::asset_policy::can_fallback_to_authored_source(true));
}

TEST_CASE(
    "json document: file loading prefers cooked and rejects a broken cook") {
  vfs_fixture::scoped_vfs guard;
  nx::vfs::MemoryDevice *const memory = nx::vfs::make_memory_device();
  const auto cooked_source = nx::json::parse(R"({"from":"cooked"})");
  REQUIRE(cooked_source);
  const nx::vector<u8> cooked =
      nx::json::encode_document(cooked_source.value());
  memory->add("/config.nxdoc.nxb",
              nx::blob<u8>({cooked.data(), cooked.size()}));
  memory->add("/config.nxdoc", vfs_fixture::blob_of(R"({"from":"source"})"));
  memory->add("/fallback.nxdoc", vfs_fixture::blob_of(R"({"from":"source"})"));
  nx::vfs::mount("/", memory);

  nx::json::value loaded;
  REQUIRE(nx::json::load_document_file("/config.nxdoc", loaded));
  CHECK(loaded["from"].get_string() == "cooked");

  REQUIRE(nx::json::load_document_file("/fallback.nxdoc", loaded));
  CHECK(loaded["from"].get_string() == "source");

  memory->add("/config.nxdoc.nxb", vfs_fixture::blob_of("broken"));
  CHECK_FALSE(nx::json::load_document_file("/config.nxdoc", loaded));
  CHECK(loaded["from"].get_string() == "source");
  CHECK(nx::json::document_cooked_path("/config.nxdoc") == "/config.nxdoc.nxb");
}
