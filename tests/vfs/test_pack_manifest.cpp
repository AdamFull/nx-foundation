#include "framework/nxtest.h"

#include "vfs_fixture.h"

#include "core/foundation/vfs/pack_manifest.h"

namespace vfs = nx::vfs;
using vfs_fixture::blob_of;
using vfs_fixture::read_eq;
using vfs_fixture::scoped_vfs;

namespace {

vfs::MemoryDevice *manifest_source(const char *json) {
  vfs::MemoryDevice *source = vfs::make_memory_device();
  source->add("data/packs.json", blob_of(json));
  return source;
}

nx::fs::io_result<vfs::Device *> missing_pack() {
  return nx::err(nx::fs::file_error{nx::fs::io_error::NotFound, 0});
}

} // namespace

TEST_CASE("pack manifest parses a versioned production manifest") {
  auto parsed = vfs::parse_pack_manifest(R"({
    "version": 1,
    "packs": [
      {"file": "base-content.nxp", "priority": 0},
      {"file": "patch-01.nxp", "priority": 100},
      {"file": "bonus_content.nxp", "priority": 200, "enabled": false}
    ]
  })");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->packs.size() == 3);
  CHECK(parsed->packs[0].file == "base-content.nxp");
  CHECK(parsed->packs[1].priority == 100);
  CHECK_FALSE(parsed->packs[2].enabled);
}

TEST_CASE("pack manifest rejects ambiguous and non-portable declarations") {
  const auto rejects = [](const nx::string_view json,
                          const vfs::PackManifestErrorCode code) {
    auto parsed = vfs::parse_pack_manifest(json);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == code);
  };

  SUBCASE("unsupported version") {
    rejects(R"({"version":2,"packs":[{"file":"a.nxp","priority":0}]})",
            vfs::PackManifestErrorCode::UnsupportedVersion);
  }
  SUBCASE("path traversal") {
    rejects(R"({"packs":[{"file":"../a.nxp","priority":0}]})",
            vfs::PackManifestErrorCode::InvalidFile);
  }
  SUBCASE("platform-dependent filename") {
    rejects(R"({"packs":[{"file":"base pack.nxp","priority":0}]})",
            vfs::PackManifestErrorCode::InvalidFile);
  }
  SUBCASE("case-insensitive duplicate filename") {
    rejects(R"({"packs":[{"file":"Base.nxp","priority":0},
                            {"file":"base.nxp","priority":1}]})",
            vfs::PackManifestErrorCode::DuplicateFile);
  }
  SUBCASE("duplicate active priority") {
    rejects(R"({"packs":[{"file":"base.nxp","priority":0},
                            {"file":"patch.nxp","priority":0}]})",
            vfs::PackManifestErrorCode::DuplicatePriority);
  }
  SUBCASE("reserved bootstrap priority") {
    rejects(R"({"packs":[{"file":"base.nxp","priority":-2147483648}]})",
            vfs::PackManifestErrorCode::InvalidPriority);
  }
  SUBCASE("invalid enabled field") {
    rejects(R"({"packs":[{"file":"base.nxp","priority":0,
                            "enabled":1}]})",
            vfs::PackManifestErrorCode::InvalidEnabled);
  }
  SUBCASE("no enabled packs") {
    rejects(R"({"packs":[{"file":"base.nxp","priority":0,
                            "enabled":false}]})",
            vfs::PackManifestErrorCode::NoEnabledPacks);
  }
  SUBCASE("duplicate JSON key") {
    rejects(R"({"packs":[],"packs":[]})",
            vfs::PackManifestErrorCode::InvalidJson);
  }
}

TEST_CASE("pack manifest permits staged packs to share a disabled priority") {
  auto parsed = vfs::parse_pack_manifest(R"({"packs":[
    {"file":"base.nxp","priority":0},
    {"file":"future-a.nxp","priority":100,"enabled":false},
    {"file":"future-b.nxp","priority":100,"enabled":false}
  ]})");
  REQUIRE(parsed.has_value());
  CHECK(parsed->packs.size() == 3);
}

TEST_CASE("pack manifest mounts overlays by declared priority") {
  scoped_vfs guard;
  REQUIRE(vfs::mount("/", manifest_source(R"({"version":1,"packs":[
        {"file":"base.nxp","priority":0},
        {"file":"patch.nxp","priority":100},
        {"file":"disabled.nxp","priority":200,"enabled":false}
      ]})"),
                     vfs::PACK_BOOTSTRAP_PRIORITY));

  usize opened = 0;
  const vfs::PackDeviceOpener opener([&opened](const nx::string_view file)
                                         -> nx::fs::io_result<vfs::Device *> {
    ++opened;
    vfs::MemoryDevice *pack = vfs::make_memory_device();
    if (file == "base.nxp") {
      pack->add("shared.txt", blob_of("BASE"));
      pack->add("base-only.txt", blob_of("BASE_ONLY"));
    } else if (file == "patch.nxp") {
      pack->add("shared.txt", blob_of("PATCH"));
      pack->add("patch-only.txt", blob_of("PATCH_ONLY"));
    } else {
      nx::release(pack);
      return missing_pack();
    }
    return pack;
  });

  auto mounted = vfs::mount_pack_manifest("/data/packs.json", opener);
  REQUIRE(mounted.has_value());
  CHECK(mounted->declared == 3);
  CHECK(mounted->enabled == 2);
  CHECK(mounted->mounted == 2);
  CHECK(mounted->highest_priority == 100);
  CHECK(opened == 2);
  CHECK(read_eq("/shared.txt", "PATCH"));
  CHECK(read_eq("/base-only.txt", "BASE_ONLY"));
  CHECK(read_eq("/patch-only.txt", "PATCH_ONLY"));
}

TEST_CASE("pack manifest mounting is transactional when a pack cannot open") {
  scoped_vfs guard;
  REQUIRE(vfs::mount("/", manifest_source(R"({"packs":[
        {"file":"base.nxp","priority":0},
        {"file":"missing.nxp","priority":100}
      ]})"),
                     vfs::PACK_BOOTSTRAP_PRIORITY));

  const vfs::PackDeviceOpener opener(
      [](const nx::string_view file) -> nx::fs::io_result<vfs::Device *> {
        if (file != "base.nxp")
          return missing_pack();
        vfs::MemoryDevice *pack = vfs::make_memory_device();
        pack->add("partial.txt", blob_of("MUST_NOT_MOUNT"));
        return pack;
      });

  auto mounted = vfs::mount_pack_manifest("/data/packs.json", opener);
  REQUIRE_FALSE(mounted.has_value());
  CHECK(mounted.error().code == vfs::PackManifestErrorCode::PackOpenFailed);
  CHECK(mounted.error().pack_index == 1);
  CHECK_FALSE(vfs::exists("/partial.txt"));
}

TEST_CASE("pack manifest reports a missing authoritative manifest") {
  scoped_vfs guard;
  REQUIRE(
      vfs::mount("/", vfs::make_memory_device(), vfs::PACK_BOOTSTRAP_PRIORITY));
  const vfs::PackDeviceOpener opener(
      [](nx::string_view) -> nx::fs::io_result<vfs::Device *> {
        return missing_pack();
      });

  auto mounted = vfs::mount_pack_manifest("/data/packs.json", opener);
  REQUIRE_FALSE(mounted.has_value());
  CHECK(mounted.error().code == vfs::PackManifestErrorCode::ManifestReadFailed);
  CHECK(mounted.error().io.kind == nx::fs::io_error::NotFound);
}
