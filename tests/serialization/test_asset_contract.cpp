#include "framework/nxtest.h"

#include "core/foundation/serialization/asset_contract.h"
#include "core/foundation/serialization/nva.h"

namespace {

using namespace nx::asset_contract;

constexpr nx::string_view RESERVED[] = {".meta", ".slang", ".md"};
constexpr Format IMAGES[] = {
    {".png", ".tex.nxb", nx::nva::fourcc('T', 'E', 'X', ' '), true},
    {".webp", ".tex.nxb", nx::nva::fourcc('T', 'E', 'X', ' '), true},
};
constexpr Format DOCUMENTS[] = {
    {".nxui", ".nxui.nxb", nx::nva::fourcc('N', 'X', 'U', 'I'), true},
};
constexpr Format SHADER = {".slang", ".nxb",
                           nx::nva::fourcc('S', 'H', 'D', 'R')};

[[nodiscard]] Registry sample_registry() {
  Registry registry(RESERVED);
  REQUIRE(registry.add("images", IMAGES) == RegistryError::None);
  REQUIRE(registry.add("documents", DOCUMENTS) == RegistryError::None);
  REQUIRE(registry.add_directory("shaders", SHADER) == RegistryError::None);
  registry.freeze();
  return registry;
}

} // namespace

TEST_CASE("asset contract: each source maps to its family's published format") {
  const Registry registry = sample_registry();
  const SourceMatch png = registry.classify_source("characters/hero.png");
  REQUIRE(png.cookable());
  CHECK(registry.handler_name(png.handler) == "images");
  CHECK(png.format->published_suffix == ".tex.nxb");
  CHECK(png.format->identity_bearing);
  CHECK(registry.classify_source("hero.webp").format->source_suffix == ".webp");
  CHECK(registry.published_format("characters/hero.tex.nxb") ==
        nx::nva::fourcc('T', 'E', 'X', ' '));
  CHECK(registry.handler_count() == 2u);
  CHECK(registry.format_count() == 3u);
}

TEST_CASE("asset contract: a directory owns the format of what it publishes") {
  const Registry registry = sample_registry();
  CHECK(registry.published_format("shaders/sprite.nxb") ==
        nx::nva::fourcc('S', 'H', 'D', 'R'));
  CHECK(registry.published_format("shaders\\video\\yuv.nxb") ==
        nx::nva::fourcc('S', 'H', 'D', 'R'));
  CHECK_FALSE(registry.published_format("shadersx/sprite.nxb"));
  CHECK_FALSE(registry.published_format("ui/shaders/sprite.nxb"));
  CHECK_FALSE(registry.published_format("shaders"));

  Registry other;
  CHECK(other.add_directory({}, SHADER) == RegistryError::EmptyName);
  constexpr Format raw{".raw", ".raw", nx::nva::fourcc('R', 'A', 'W', ' ')};
  CHECK(other.add_directory("raw", raw) ==
        RegistryError::InvalidPublishedSuffix);
  REQUIRE(other.add_directory("shaders", SHADER) == RegistryError::None);
  CHECK(other.add_directory("shaders", SHADER) == RegistryError::DuplicateName);
  other.freeze();
  CHECK(other.add_directory("late", SHADER) == RegistryError::Frozen);
}

TEST_CASE("asset contract: a cooked path takes the place of the source's "
          "suffix") {
  using nx::asset_contract::cooked_path;
  CHECK(cooked_path("/ui/hero.png", ".tex.nxb") == "/ui/hero.tex.nxb");
  CHECK(cooked_path("/ui/my.hero.webp", ".tex.nxb") == "/ui/my.hero.tex.nxb");
  CHECK(cooked_path("/m/hero.scene3d.d/a1.nxmesh", ".mesh.nxb") ==
        "/m/hero.scene3d.d/a1.mesh.nxb");
  // A dot in a folder is not the file's suffix.
  CHECK(cooked_path("/v1.2/readme", ".tex.nxb") == "/v1.2/readme.tex.nxb");
  CHECK(cooked_path("C:\\a.b\\hero", ".font.nxb") == "C:\\a.b\\hero.font.nxb");
  CHECK(cooked_path("hero", ".audio.nxb") == "hero.audio.nxb");
}

TEST_CASE("asset contract: authoring and unsupported files cannot publish") {
  const Registry registry = sample_registry();
  using enum SourceKind;
  CHECK(registry.classify_source("hero.png.meta").kind == AuthoringOnly);
  CHECK(registry.classify_source("sprite.slang").kind == AuthoringOnly);
  CHECK(registry.classify_source("LICENCE.md").kind == AuthoringOnly);
  CHECK(registry.classify_source("movie.webm").kind == Unsupported);
  CHECK(registry.classify_source("already.nxb").kind == PreCooked);
  CHECK_FALSE(registry.published_format("already.nxb"));
  CHECK_FALSE(registry.published_format("movie.webm"));

  const Registry plain;
  CHECK(plain.classify_source("sprite.slang").kind == Unsupported);
}

TEST_CASE("asset contract: module bundles own their embedded source suffixes") {
  using namespace nx::asset_contract;
  static constexpr Format bundle[] = {
      {".model.json", ".model.json.nxb", nx::nva::fourcc('B', 'N', 'D', 'L')},
  };
  static constexpr nx::string_view embedded[] = {".moc3", ".motion3.json"};
  static constexpr Format conflicting[] = {
      {".moc3", ".moc3.nxb", nx::nva::fourcc('M', 'O', 'C', '3')},
  };

  Registry registry;
  REQUIRE(registry.add("bundle", bundle, nullptr, embedded) ==
          RegistryError::None);
  CHECK(registry.embedded_source_count() == 2u);
  CHECK(registry.add("conflict", conflicting) ==
        RegistryError::DuplicateSourceSuffix);
  CHECK(registry.handler_count() == 1u);
  registry.freeze();
  CHECK(registry.classify_source("hero.model.json").cookable());
  CHECK(registry.classify_source("hero.moc3").kind ==
        SourceKind::AuthoringOnly);
  CHECK(registry.classify_source("idle.motion3.json").kind ==
        SourceKind::AuthoringOnly);
  CHECK(registry.classify_source("notes.json").kind == SourceKind::Unsupported);
}

TEST_CASE("asset contract: registration is transactional and rejects clashes") {
  using namespace nx::asset_contract;
  static constexpr Format first[] = {
      {".one", ".one.nxb", nx::nva::fourcc('O', 'N', 'E', ' ')},
  };
  static constexpr Format duplicate_batch[] = {
      {".two", ".two.nxb", nx::nva::fourcc('T', 'W', 'O', ' ')},
      {".two", ".other.nxb", nx::nva::fourcc('O', 'T', 'H', 'R')},
  };
  static constexpr Format duplicate_published[] = {
      {".different", ".one.nxb", nx::nva::fourcc('D', 'I', 'F', 'F')},
  };

  Registry registry;
  CHECK(registry.add("first", first) == RegistryError::None);
  CHECK(registry.add("first", first) == RegistryError::DuplicateName);
  CHECK(registry.add("batch", duplicate_batch) ==
        RegistryError::DuplicateSourceSuffix);
  CHECK(registry.add("published", duplicate_published) ==
        RegistryError::DuplicatePublishedSuffix);
  CHECK(registry.handler_count() == 1u);
  CHECK(registry.format_count() == 1u);
}

TEST_CASE("asset contract: formats share a published suffix only when they "
          "share its container") {
  using namespace nx::asset_contract;
  static constexpr Format first[] = {
      {".one", ".same.nxb", nx::nva::fourcc('S', 'A', 'M', 'E')},
      {".two", ".same.nxb", nx::nva::fourcc('S', 'A', 'M', 'E')},
  };
  static constexpr Format importer[] = {
      {".three", ".same.nxb", nx::nva::fourcc('S', 'A', 'M', 'E')},
  };
  static constexpr Format other[] = {
      {".four", ".same.nxb", nx::nva::fourcc('O', 'T', 'H', 'R')},
  };
  static constexpr Format other_batch[] = {
      {".five", ".pair.nxb", nx::nva::fourcc('S', 'A', 'M', 'E')},
      {".six", ".pair.nxb", nx::nva::fourcc('O', 'T', 'H', 'R')},
  };

  Registry registry;
  CHECK(registry.add("first", first) == RegistryError::None);
  CHECK(registry.add("importer", importer) == RegistryError::None);
  CHECK(registry.add("other", other) ==
        RegistryError::DuplicatePublishedSuffix);
  CHECK(registry.add("other_batch", other_batch) ==
        RegistryError::DuplicatePublishedSuffix);
  registry.freeze();
  CHECK(registry.published_format("asset.same.nxb") ==
        nx::nva::fourcc('S', 'A', 'M', 'E'));
}

TEST_CASE("asset contract: malformed descriptors are rejected") {
  using namespace nx::asset_contract;
  static constexpr Format empty_source[] = {
      {{}, ".empty.nxb", nx::nva::fourcc('E', 'M', 'P', 'T')},
  };
  static constexpr Format reserved_source[] = {
      {".raw.nxb", ".raw.nxb.nxb", nx::nva::fourcc('R', 'A', 'W', ' ')},
  };
  static constexpr Format authoring_source[] = {
      {".custom.slang", ".custom.slang.nxb",
       nx::nva::fourcc('S', 'L', 'N', 'G')},
  };
  static constexpr Format raw_output[] = {
      {".raw", ".raw", nx::nva::fourcc('R', 'A', 'W', ' ')},
  };
  static constexpr Format broad_output[] = {
      {".broad", ".nxb", nx::nva::fourcc('A', 'L', 'L', ' ')},
  };
  static constexpr Format no_tag[] = {
      {".zero", ".zero.nxb", 0},
  };

  Registry registry(RESERVED);
  CHECK(registry.add({}, empty_source) == RegistryError::EmptyName);
  CHECK(registry.add("none", {}) == RegistryError::EmptyFormats);
  CHECK(registry.add("empty", empty_source) == RegistryError::EmptySuffix);
  CHECK(registry.add("reserved", reserved_source) ==
        RegistryError::InvalidSourceSuffix);
  CHECK(registry.add("authoring", authoring_source) ==
        RegistryError::InvalidSourceSuffix);
  CHECK(registry.add("output", raw_output) ==
        RegistryError::InvalidPublishedSuffix);
  CHECK(registry.add("broad", broad_output) ==
        RegistryError::InvalidPublishedSuffix);
  CHECK(registry.add("tag", no_tag) == RegistryError::InvalidFormat);
  CHECK(registry.handler_count() == 0u);
  CHECK(registry.format_count() == 0u);
}

TEST_CASE("asset contract: the longest matching suffix wins") {
  using namespace nx::asset_contract;
  static constexpr Format generic[] = {
      {".json", ".json.nxb", nx::nva::fourcc('J', 'S', 'O', 'N')},
  };
  static constexpr Format model[] = {
      {".model3.json", ".model3.json.nxb", nx::nva::fourcc('M', 'O', 'D', 'L')},
  };

  Registry registry;
  Handler generic_handler = INVALID_HANDLER;
  Handler model_handler = INVALID_HANDLER;
  REQUIRE(registry.add("generic", generic, &generic_handler) ==
          RegistryError::None);
  REQUIRE(registry.add("model", model, &model_handler) == RegistryError::None);
  registry.freeze();

  CHECK(registry.classify_source("hero.json").handler == generic_handler);
  CHECK(registry.classify_source("hero.model3.json").handler == model_handler);
  const std::optional<PublishedMatch> published =
      registry.find_published("hero.model3.json.nxb");
  REQUIRE(published);
  CHECK(published->handler == model_handler);
}

TEST_CASE(
    "asset contract: freezing stabilizes lookup and closes registration") {
  using namespace nx::asset_contract;
  static constexpr Format first[] = {
      {".one", ".one.nxb", nx::nva::fourcc('O', 'N', 'E', ' ')},
  };
  static constexpr Format second[] = {
      {".two", ".two.nxb", nx::nva::fourcc('T', 'W', 'O', ' ')},
  };

  Registry registry;
  REQUIRE(registry.add("first", first) == RegistryError::None);
  CHECK(registry.classify_source("asset.one").kind == SourceKind::Unsupported);
  registry.freeze();
  const SourceMatch before = registry.classify_source("asset.one");
  REQUIRE(before.cookable());
  registry.freeze();
  CHECK(registry.add("second", second) == RegistryError::Frozen);
  const SourceMatch after = registry.classify_source("asset.one");
  CHECK(after.format == before.format);
  CHECK(after.handler == before.handler);
}

TEST_CASE("asset contract: format tags may be shared by one or more families") {
  using namespace nx::asset_contract;
  static constexpr Format first[] = {
      {".one", ".one.nxb", nx::nva::fourcc('S', 'A', 'M', 'E')},
  };
  static constexpr Format second[] = {
      {".two", ".two.nxb", nx::nva::fourcc('S', 'A', 'M', 'E')},
  };

  Registry registry;
  CHECK(registry.add("first", first) == RegistryError::None);
  CHECK(registry.add("second", second) == RegistryError::None);
  registry.freeze();
  CHECK(registry.published_format("asset.one.nxb") ==
        registry.published_format("asset.two.nxb"));
}
