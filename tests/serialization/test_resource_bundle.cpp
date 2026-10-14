#include "framework/nxtest.h"

#include "core/foundation/serialization/nva.h"
#include "core/foundation/serialization/resource_bundle.h"

#include <algorithm>

namespace {

constexpr u32 FORMAT = nx::nva::fourcc('T', 'B', 'N', 'D');
constexpr u16 VERSION = 3;

[[nodiscard]] std::span<const u8> bytes(const nx::string_view text) {
  return {reinterpret_cast<const u8 *>(text.data()), text.size()};
}

[[nodiscard]] bool same(const std::span<const u8> lhs,
                        const std::span<const u8> rhs) {
  return lhs.size() == rhs.size() &&
         std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

} // namespace

TEST_CASE("resource bundle: deterministic entries and metadata round-trip") {
  const nx::resource_bundle::Source source[] = {
      {"z/data.bin", bytes("last")},
      {"manifest.json", bytes("first")},
  };
  const auto encoded =
      nx::resource_bundle::encode(FORMAT, VERSION, bytes("meta"), source);
  REQUIRE(encoded);
  const auto view = nx::resource_bundle::View::open(
      {encoded->data(), encoded->size()}, FORMAT, VERSION);
  REQUIRE(view);
  CHECK(view->size() == 2u);
  CHECK(same(view->metadata(), bytes("meta")));
  CHECK(same(view->find("manifest.json"), bytes("first")));
  CHECK(same(view->find("z/data.bin"), bytes("last")));
  CHECK(view->find("missing").empty());

  const nx::resource_bundle::Source reversed[] = {source[1], source[0]};
  CHECK(nx::resource_bundle::encode(FORMAT, VERSION, bytes("meta"), reversed) ==
        encoded);
}

TEST_CASE("resource bundle: unsafe and duplicate names are rejected") {
  CHECK(nx::resource_bundle::valid_name("folder/file.bin"));
  CHECK_FALSE(nx::resource_bundle::valid_name("/absolute.bin"));
  CHECK_FALSE(nx::resource_bundle::valid_name("../escape.bin"));
  CHECK_FALSE(nx::resource_bundle::valid_name("folder\\file.bin"));
  CHECK_FALSE(nx::resource_bundle::valid_name("C:/drive.bin"));
  CHECK_FALSE(nx::resource_bundle::valid_name("folder/bad?.bin"));
  CHECK_FALSE(nx::resource_bundle::valid_name("folder/trailing. "));
  CHECK_FALSE(nx::resource_bundle::valid_name("folder/CON.txt"));
  CHECK_FALSE(nx::resource_bundle::valid_name("folder/lpt9"));
  const char invalid_utf8[] = {'b', 'a', 'd', static_cast<char>(0xff)};
  CHECK_FALSE(nx::resource_bundle::valid_name(
      nx::string_view(invalid_utf8, sizeof(invalid_utf8))));

  const nx::resource_bundle::Source duplicate[] = {{"same", bytes("one")},
                                                   {"same", bytes("two")}};
  CHECK_FALSE(
      nx::resource_bundle::encode(FORMAT, VERSION, {}, duplicate).has_value());
  const nx::resource_bundle::Source empty[] = {{"empty", {}}};
  CHECK_FALSE(
      nx::resource_bundle::encode(FORMAT, VERSION, {}, empty).has_value());
}

TEST_CASE("resource bundle: wrong versions, truncation and limits fail") {
  const nx::resource_bundle::Source source[] = {{"file", bytes("payload")}};
  const auto encoded = nx::resource_bundle::encode(FORMAT, VERSION, {}, source);
  REQUIRE(encoded);
  CHECK_FALSE(nx::resource_bundle::View::open(
                  {encoded->data(), encoded->size()}, FORMAT, VERSION + 1)
                  .has_value());
  CHECK_FALSE(nx::resource_bundle::View::open(
                  {encoded->data(), encoded->size() - 1}, FORMAT, VERSION)
                  .has_value());
  CHECK_FALSE(nx::resource_bundle::View::open(
                  {encoded->data(), encoded->size()}, FORMAT, VERSION,
                  {.max_entries = 1,
                   .max_name_bytes = 2,
                   .max_metadata_bytes = 1,
                   .max_data_bytes = 4})
                  .has_value());
}
