#include "core/foundation/vfs/pack_manifest.h"

#include "core/foundation/core/scope_guard.h"
#include "core/foundation/serialization/json.h"
#include "core/foundation/strings/format.h"

namespace nx::vfs {
namespace {

[[nodiscard]] PackManifestError failure(const PackManifestErrorCode code,
                                        const nx::string_view detail,
                                        const usize pack_index = NO_PACK_INDEX,
                                        const nx::fs::file_error io = {}) {
  return PackManifestError{.code = code,
                           .pack_index = pack_index,
                           .io = io,
                           .detail = nx::string(detail)};
}

[[nodiscard]] constexpr bool ascii_alnum(const unsigned char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z');
}

[[nodiscard]] constexpr unsigned char
ascii_lower(const unsigned char c) noexcept {
  return c >= 'A' && c <= 'Z' ? nx::cast<unsigned char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] bool portable_pack_name(const nx::string_view file) noexcept {
  if (file.size() < 5 || file.size() > 128 || !file.ends_with(".nxp"))
    return false;
  if (!ascii_alnum(nx::cast<unsigned char>(file.front())))
    return false;
  for (const char c : file) {
    const auto ch = nx::cast<unsigned char>(c);
    if (!ascii_alnum(ch) && c != '.' && c != '-' && c != '_')
      return false;
  }
  return true;
}

[[nodiscard]] bool ascii_iequal(const nx::string_view a,
                                const nx::string_view b) noexcept {
  if (a.size() != b.size())
    return false;
  for (usize i = 0; i < a.size(); ++i) {
    const auto ac = nx::cast<unsigned char>(a[i]);
    const auto bc = nx::cast<unsigned char>(b[i]);
    if (ascii_lower(ac) != ascii_lower(bc))
      return false;
  }
  return true;
}

[[nodiscard]] nx::string_view error_name(const PackManifestErrorCode code) {
  switch (code) {
  case PackManifestErrorCode::ManifestReadFailed:
    return "could not read pack manifest";
  case PackManifestErrorCode::InvalidJson:
    return "invalid pack-manifest JSON";
  case PackManifestErrorCode::InvalidRoot:
    return "pack-manifest root must be an object";
  case PackManifestErrorCode::InvalidVersion:
    return "pack-manifest version must be an unsigned integer";
  case PackManifestErrorCode::UnsupportedVersion:
    return "unsupported pack-manifest version";
  case PackManifestErrorCode::MissingPacks:
    return "pack manifest is missing 'packs'";
  case PackManifestErrorCode::InvalidPacks:
    return "pack-manifest 'packs' must be an array";
  case PackManifestErrorCode::TooManyPacks:
    return "pack manifest contains too many packs";
  case PackManifestErrorCode::InvalidEntry:
    return "pack entry must be an object";
  case PackManifestErrorCode::MissingFile:
    return "pack entry is missing 'file'";
  case PackManifestErrorCode::InvalidFile:
    return "pack file must be a portable .nxp basename";
  case PackManifestErrorCode::MissingPriority:
    return "pack entry is missing 'priority'";
  case PackManifestErrorCode::InvalidPriority:
    return "pack priority must be a signed 32-bit integer above the reserved "
           "bootstrap priority";
  case PackManifestErrorCode::InvalidEnabled:
    return "pack 'enabled' must be a boolean";
  case PackManifestErrorCode::DuplicateFile:
    return "pack file is listed more than once";
  case PackManifestErrorCode::DuplicatePriority:
    return "enabled packs must have unique priorities";
  case PackManifestErrorCode::NoEnabledPacks:
    return "pack manifest has no enabled packs";
  case PackManifestErrorCode::InvalidOpener:
    return "pack device opener is not set";
  case PackManifestErrorCode::PackOpenFailed:
    return "could not open pack";
  case PackManifestErrorCode::MountFailed:
    return "could not mount pack";
  }
  return "pack-manifest error";
}

struct PendingPack {
  Device *device = nullptr;
  const PackSpec *spec = nullptr;
  usize index = NO_PACK_INDEX;
};

} // namespace

nx::string PackManifestError::message() const {
  nx::string out;
  if (pack_index != NO_PACK_INDEX)
    out = nx::format("pack[{}]: {}", pack_index, error_name(code));
  else
    out.assign(error_name(code));
  if (!detail.empty())
    out.append(nx::format(": {}", detail.view()));
  if (io.kind != nx::fs::io_error::Unknown)
    out.append(nx::format(": {} (native error {})", nx::fs::to_string(io.kind),
                          io.native));
  return out;
}

pack_result<PackManifest> parse_pack_manifest(const nx::string_view text) {
  nx::json::parse_options options;
  options.reject_duplicate_keys = true;
  options.validate_utf8 = true;
  auto parsed = nx::json::parse(text, options);
  if (!parsed)
    return nx::err(failure(PackManifestErrorCode::InvalidJson,
                           parsed.error().message().view()));

  const nx::json::value &root = *parsed;
  if (!root.is_object())
    return nx::err(failure(PackManifestErrorCode::InvalidRoot, {}));

  if (const nx::json::value *version = root.find("version")) {
    if (!version->fits<u32>())
      return nx::err(failure(PackManifestErrorCode::InvalidVersion, {}));
    if (version->get_u64() != PACK_MANIFEST_VERSION)
      return nx::err(
          failure(PackManifestErrorCode::UnsupportedVersion,
                  nx::format("expected {}, got {}", PACK_MANIFEST_VERSION,
                             version->get_u64())
                      .view()));
  }

  const nx::json::value *packs = root.find("packs");
  if (!packs)
    return nx::err(failure(PackManifestErrorCode::MissingPacks, {}));
  if (!packs->is_array())
    return nx::err(failure(PackManifestErrorCode::InvalidPacks, {}));
  if (packs->size() > MAX_PACKS_PER_MANIFEST)
    return nx::err(
        failure(PackManifestErrorCode::TooManyPacks,
                nx::format("maximum is {}", MAX_PACKS_PER_MANIFEST).view()));

  PackManifest manifest;
  manifest.packs.reserve(packs->size());
  for (usize i = 0; i < packs->size(); ++i) {
    const nx::json::value &entry = (*packs)[i];
    if (!entry.is_object())
      return nx::err(failure(PackManifestErrorCode::InvalidEntry, {}, i));

    const nx::json::value *file = entry.find("file");
    if (!file)
      return nx::err(failure(PackManifestErrorCode::MissingFile, {}, i));
    if (!file->is_string() || !portable_pack_name(file->get_string()))
      return nx::err(failure(
          PackManifestErrorCode::InvalidFile,
          file->is_string() ? file->get_string() : nx::string_view{}, i));

    const nx::json::value *priority = entry.find("priority");
    if (!priority)
      return nx::err(failure(PackManifestErrorCode::MissingPriority, {}, i));
    if (!priority->fits<i32>() || priority->get_i64() < MIN_PACK_PRIORITY)
      return nx::err(failure(PackManifestErrorCode::InvalidPriority, {}, i));

    bool enabled = true;
    if (const nx::json::value *value = entry.find("enabled")) {
      if (!value->is_bool())
        return nx::err(failure(PackManifestErrorCode::InvalidEnabled, {}, i));
      enabled = value->as_bool();
    }

    PackSpec spec{.file = nx::string(file->get_string()),
                  .priority = nx::cast<i32>(priority->get_i64()),
                  .enabled = enabled};
    for (usize other = 0; other < manifest.packs.size(); ++other) {
      const PackSpec &existing = manifest.packs[other];
      if (ascii_iequal(existing.file.view(), spec.file.view()))
        return nx::err(
            failure(PackManifestErrorCode::DuplicateFile,
                    nx::format("also listed by pack[{}]", other).view(), i));
      if (existing.enabled && spec.enabled &&
          existing.priority == spec.priority)
        return nx::err(
            failure(PackManifestErrorCode::DuplicatePriority,
                    nx::format("priority {} is also used by pack[{}]",
                               spec.priority, other)
                        .view(),
                    i));
    }
    manifest.packs.push_back(std::move(spec));
  }

  for (const PackSpec &pack : manifest.packs)
    if (pack.enabled)
      return manifest;
  return nx::err(failure(PackManifestErrorCode::NoEnabledPacks, {}));
}

pack_result<PackMountReport>
mount_pack_manifest(const nx::fs::path_view manifest_path,
                    const PackDeviceOpener &open_pack) {
  if (!open_pack)
    return nx::err(failure(PackManifestErrorCode::InvalidOpener, {}));

  auto text = nx::vfs::read_text(manifest_path);
  if (!text)
    return nx::err(failure(PackManifestErrorCode::ManifestReadFailed,
                           manifest_path.native(), NO_PACK_INDEX,
                           text.error()));

  auto manifest = parse_pack_manifest(text->view());
  if (!manifest)
    return nx::err(std::move(manifest).error());

  PackMountReport report;
  report.declared = nx::cast<u32>(manifest->packs.size());

  nx::vector<PendingPack> pending;
  pending.reserve(manifest->packs.size());
  defer {
    for (const PendingPack &pack : pending)
      if (pack.device)
        nx::release(pack.device);
  };

  for (usize i = 0; i < manifest->packs.size(); ++i) {
    const PackSpec &spec = manifest->packs[i];
    if (!spec.enabled)
      continue;
    ++report.enabled;
    report.highest_priority = nx::max(report.highest_priority, spec.priority);
    auto device = open_pack(spec.file.view());
    if (!device || *device == nullptr)
      return nx::err(
          failure(PackManifestErrorCode::PackOpenFailed, spec.file.view(), i,
                  device ? nx::fs::file_error{nx::fs::io_error::OpenFailed, 0}
                         : device.error()));
    pending.push_back(
        PendingPack{.device = *device, .spec = &spec, .index = i});
  }

  nx::vector<MountId> mounted;
  mounted.reserve(pending.size());
  for (PendingPack &pack : pending) {
    Device *device = pack.device;
    pack.device = nullptr;
    const MountId id = nx::vfs::mount("/", device, pack.spec->priority);
    if (!id) {
      for (const MountId mounted_id : mounted)
        nx::vfs::unmount(mounted_id);
      return nx::err(failure(PackManifestErrorCode::MountFailed,
                             pack.spec->file.view(), pack.index));
    }
    mounted.push_back(id);
    ++report.mounted;
  }
  return report;
}

} // namespace nx::vfs
