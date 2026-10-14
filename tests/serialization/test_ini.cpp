#include "framework/nxtest.h"

#include "core/foundation/serialization/ini.h"
#include "core/foundation/vfs/vfs.h"

#include "../vfs/vfs_fixture.h"

TEST_CASE("ini: sections, comments and whitespace parse") {
  constexpr nx::string_view source =
      "\xef\xbb\xbf; engine settings\r\n"
      "root = value\r\n"
      "\r\n"
      "[logging] # comment\r\n"
      "level = debug ; selected level\r\n"
      "[graphics]\r\n"
      "resolution = 1920x1080\r\n";

  const auto parsed = nx::ini::parse(source);
  REQUIRE(parsed);
  REQUIRE(parsed->find({}, "root") != nullptr);
  CHECK(parsed->find({}, "root")->view() == "value");
  REQUIRE(parsed->find("logging", "level") != nullptr);
  CHECK(parsed->find("logging", "level")->view() == "debug");
  REQUIRE(parsed->find("graphics", "resolution") != nullptr);
  CHECK(parsed->find("graphics", "resolution")->view() == "1920x1080");
}

TEST_CASE("ini: quoted values preserve comments, whitespace and escapes") {
  constexpr nx::string_view source =
      "[paths]\n"
      "cache = \" C:\\\\cache;shared #1 \"\n"
      "message = \"line one\\nline two\\t\\\"ok\\\"\"\n";

  const auto parsed = nx::ini::parse(source);
  REQUIRE(parsed);
  REQUIRE(parsed->find("paths", "cache") != nullptr);
  CHECK(parsed->find("paths", "cache")->view() ==
        " C:\\cache;shared #1 ");
  REQUIRE(parsed->find("paths", "message") != nullptr);
  CHECK(parsed->find("paths", "message")->view() ==
        "line one\nline two\t\"ok\"");
}

TEST_CASE("ini: repeated keys are last-value-wins") {
  const auto parsed = nx::ini::parse("[render]\nmsaa=2\nmsaa=4\n");
  REQUIRE(parsed);
  CHECK(parsed->entries().size() == 1);
  REQUIRE(parsed->find("render", "msaa") != nullptr);
  CHECK(parsed->find("render", "msaa")->view() == "4");
}

TEST_CASE("ini: serialization is deterministic and round trips") {
  nx::ini::Document document;
  REQUIRE(document.set({}, "version", "1"));
  REQUIRE(document.set("logging", "level", "warn"));
  REQUIRE(document.set("paths", "special", " #value; "));

  const nx::string text = document.serialize();
  CHECK(text.view() == "version = 1\n\n[logging]\nlevel = warn\n\n[paths]\n"
                       "special = \" #value; \"\n");
  const auto reparsed = nx::ini::parse(text.view());
  REQUIRE(reparsed);
  REQUIRE(reparsed->find("paths", "special") != nullptr);
  CHECK(reparsed->find("paths", "special")->view() == " #value; ");
}

TEST_CASE("ini: malformed documents report their source location") {
  const auto missing = nx::ini::parse("[ok]\nnot an assignment\n");
  REQUIRE(!missing);
  CHECK(missing.error().kind == nx::ini::ParseErrorKind::MissingAssignment);
  CHECK(missing.error().line == 2);
  CHECK(missing.error().column == 1);

  const auto escape = nx::ini::parse("value = \"bad\\q\"\n");
  REQUIRE(!escape);
  CHECK(escape.error().kind == nx::ini::ParseErrorKind::InvalidEscape);
  CHECK(escape.error().line == 1);
}

TEST_CASE("ini: document rejects unsafe names") {
  nx::ini::Document document;
  CHECK(!document.set("bad]section", "key", "value"));
  CHECK(!document.set("section", "", "value"));
  CHECK(!document.set("section", " bad", "value"));
  CHECK(!document.set("section", "bad=key", "value"));
  CHECK(document.empty());
}

namespace {

[[nodiscard]] nx::string assigned(const nx::string_view text,
                                  const nx::string_view section,
                                  const nx::string_view key,
                                  const nx::string_view value) {
  auto out = nx::ini::assign(text, section, key, value);
  REQUIRE(out);
  // Whatever was written reads back as the value.
  const auto parsed = nx::ini::parse(out->view());
  REQUIRE(parsed);
  REQUIRE(parsed->find(section, key) != nullptr);
  CHECK(parsed->find(section, key)->view() == value);
  return std::move(*out);
}

} // namespace

TEST_CASE("ini: assign rewrites a value in place, keeping every other line") {
  constexpr nx::string_view source = "; the app's settings\n"
                                     "[app]\n"
                                     "; which language\n"
                                     "  language = english ; the UI's\n"
                                     "other = 1\n"
                                     "\n"
                                     "[llm]\n"
                                     "provider = mock\n";
  CHECK(assigned(source, "app", "language", "japanese") ==
        "; the app's settings\n"
        "[app]\n"
        "; which language\n"
        "  language = japanese ; the UI's\n"
        "other = 1\n"
        "\n"
        "[llm]\n"
        "provider = mock\n");
  // A value the grammar has to quote, and one that empties the key.
  CHECK(assigned(source, "llm", "provider", "a ; b")
            .ends_with("provider = \"a ; b\"\n"));
  CHECK(assigned(source, "llm", "provider", "").ends_with("provider = \n"));
}

TEST_CASE("ini: assign sets every assignment of a key") {
  constexpr nx::string_view source = "[a]\n"
                                     "k = 1\n"
                                     "[b]\n"
                                     "k = 2\n"
                                     "[a]\n"
                                     "k = 3 # last wins\n";
  CHECK(assigned(source, "a", "k", "9") == "[a]\n"
                                           "k = 9\n"
                                           "[b]\n"
                                           "k = 2\n"
                                           "[a]\n"
                                           "k = 9 # last wins\n");
}

TEST_CASE("ini: assign adds a missing key to its section") {
  constexpr nx::string_view source = "[app]\n"
                                     "language = english\n"
                                     "; about the next one\n"
                                     "\n"
                                     "[llm]\n"
                                     "provider = mock";
  CHECK(assigned(source, "app", "look", "true") == "[app]\n"
                                                   "language = english\n"
                                                   "look = true\n"
                                                   "; about the next one\n"
                                                   "\n"
                                                   "[llm]\n"
                                                   "provider = mock");
  // After the last line, which has no ending of its own.
  CHECK(assigned(source, "llm", "model", "qwen") == "[app]\n"
                                                    "language = english\n"
                                                    "; about the next one\n"
                                                    "\n"
                                                    "[llm]\n"
                                                    "provider = mock\n"
                                                    "model = qwen\n");
  // A section with no assignment yet.
  CHECK(assigned("[empty]\n[x]\n", "empty", "k", "v") == "[empty]\n"
                                                         "k = v\n"
                                                         "[x]\n");
  // Outside any section: before the first header.
  CHECK(assigned("[x]\ny = 1\n", {}, "top", "1") == "top = 1\n"
                                                    "[x]\n"
                                                    "y = 1\n");
}

TEST_CASE("ini: assign adds a missing section at the end") {
  CHECK(assigned("[app]\nlanguage = english\n", "tts", "provider", "off") ==
        "[app]\n"
        "language = english\n"
        "\n"
        "[tts]\n"
        "provider = off\n");
  CHECK(assigned("", "tts", "provider", "off") == "[tts]\n"
                                                  "provider = off\n");
  CHECK(assigned("a = 1", "tts", "provider", "off") == "a = 1\n"
                                                       "\n"
                                                       "[tts]\n"
                                                       "provider = off\n");
}

TEST_CASE("ini: assign follows the text's line endings") {
  CHECK(assigned("[app]\r\nlanguage = english\r\n", "app", "look", "true") ==
        "[app]\r\n"
        "language = english\r\n"
        "look = true\r\n");
  CHECK(assigned("[app]\r\nlanguage = english\r\n", "llm", "p", "x") ==
        "[app]\r\n"
        "language = english\r\n"
        "\r\n"
        "[llm]\r\n"
        "p = x\r\n");
  CHECK(assigned("[app]\r\nlanguage = english\r\n", "app", "language",
                 "korean") == "[app]\r\nlanguage = korean\r\n");
}

TEST_CASE("ini: assign refuses what it cannot write safely") {
  CHECK_FALSE(nx::ini::assign("[broken\n", "a", "k", "v"));
  CHECK_FALSE(nx::ini::assign("[a]\nk = \"open\n", "a", "k", "v"));
  const auto key = nx::ini::assign("[a]\n", "a", "k=x", "v");
  REQUIRE_FALSE(key);
  CHECK(key.error().kind == nx::ini::ParseErrorKind::InvalidKey);
  const auto section = nx::ini::assign("[a]\n", "a]", "k", "v");
  REQUIRE_FALSE(section);
  CHECK(section.error().kind == nx::ini::ParseErrorKind::InvalidSection);
  CHECK_FALSE(nx::ini::assign("[a]\n", " a", "k", "v"));
}

TEST_CASE("ini: a cooked document holds every entry in order, and a "
          "truncated or foreign one is refused") {
  const auto parsed = nx::ini::parse("root = 1\n[store]\napp_id = 480\n"
                                     "name = \"a = b\"\n");
  REQUIRE(parsed);
  const nx::vector<u8> cooked = nx::ini::encode(*parsed);
  const std::optional<nx::ini::Document> decoded =
      nx::ini::decode({cooked.data(), cooked.size()});
  REQUIRE(decoded.has_value());
  REQUIRE(decoded->entries().size() == 3u);
  CHECK(decoded->entries()[1].section.view() == "store");
  CHECK(decoded->entries()[1].key.view() == "app_id");
  CHECK(decoded->find("store", "name")->view() == "a = b");
  CHECK(decoded->serialize() == parsed->serialize());

  for (usize cut = 0; cut < cooked.size(); ++cut)
    CHECK_FALSE(nx::ini::decode({cooked.data(), cut}).has_value());
  nx::vector<u8> foreign = cooked;
  foreign[0] ^= 0xFFu;
  CHECK_FALSE(nx::ini::decode({foreign.data(), foreign.size()}).has_value());
}

TEST_CASE("ini: load prefers the cooked sibling, falls back to the source "
          "outside Shipping, and does not hide a broken cook") {
  vfs_fixture::scoped_vfs guard;
  nx::vfs::MemoryDevice *const memory = nx::vfs::make_memory_device();
  const auto cooked_source = nx::ini::parse("[a]\nfrom = cooked\n");
  REQUIRE(cooked_source);
  const nx::vector<u8> cooked = nx::ini::encode(*cooked_source);
  memory->add("/config/a.ini.nxb",
              nx::blob<u8>({cooked.data(), cooked.size()}));
  memory->add("/config/a.ini", vfs_fixture::blob_of("[a]\nfrom = source\n"));
  memory->add("/config/b.ini", vfs_fixture::blob_of("[a]\nfrom = source\n"));
  memory->add("/config/bad.ini", vfs_fixture::blob_of("[unterminated\n"));
  nx::vfs::mount("/", memory);

  const auto a = nx::ini::load("/config/a.ini");
  REQUIRE(a);
  CHECK(a->find("a", "from")->view() == "cooked");
  const auto b = nx::ini::load("/config/b.ini");
  REQUIRE(b);
  CHECK(b->find("a", "from")->view() == "source");
  const auto missing = nx::ini::load("/config/none.ini");
  REQUIRE_FALSE(missing);
  CHECK(missing.error().kind == nx::ini::LoadErrorKind::NotFound);
  const auto bad = nx::ini::load("/config/bad.ini");
  REQUIRE_FALSE(bad);
  CHECK(bad.error().kind == nx::ini::LoadErrorKind::Invalid);
  CHECK(bad.error().parse.line == 1u);

  memory->add("/config/a.ini.nxb", vfs_fixture::blob_of("broken"));
  const auto broken = nx::ini::load("/config/a.ini");
  REQUIRE_FALSE(broken);
  CHECK(broken.error().kind == nx::ini::LoadErrorKind::Malformed);
}
