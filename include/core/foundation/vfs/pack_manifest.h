#pragma once

#include "core/foundation/core/result.h"
#include "core/foundation/vfs/vfs.h"

#include <limits>

namespace nx::vfs {

inline constexpr u32 PACK_MANIFEST_VERSION = 1;
inline constexpr usize MAX_PACKS_PER_MANIFEST = 256;
inline constexpr usize NO_PACK_INDEX = ~usize{0};
inline constexpr i32 PACK_BOOTSTRAP_PRIORITY = std::numeric_limits<i32>::min();
inline constexpr i32 MIN_PACK_PRIORITY = PACK_BOOTSTRAP_PRIORITY + 1;

struct PackSpec {
  nx::string file;
  i32 priority = 0;
  bool enabled = true;
};

struct PackManifest {
  nx::vector<PackSpec> packs;
};

enum class PackManifestErrorCode : u8 {
  ManifestReadFailed,
  InvalidJson,
  InvalidRoot,
  InvalidVersion,
  UnsupportedVersion,
  MissingPacks,
  InvalidPacks,
  TooManyPacks,
  InvalidEntry,
  MissingFile,
  InvalidFile,
  MissingPriority,
  InvalidPriority,
  InvalidEnabled,
  DuplicateFile,
  DuplicatePriority,
  NoEnabledPacks,
  InvalidOpener,
  PackOpenFailed,
  MountFailed,
};

struct PackManifestError {
  PackManifestErrorCode code = PackManifestErrorCode::InvalidJson;
  usize pack_index = NO_PACK_INDEX;
  nx::fs::file_error io{};
  nx::string detail;

  [[nodiscard]] nx::string message() const;
};

template <typename T> using pack_result = nx::result<T, PackManifestError>;

/// Parses and validates a version-1 pack manifest. Pack file names are portable
/// ASCII basenames in the manifest's data directory; paths and traversal are
/// rejected. The lowest i32 is reserved for the bootstrap source. Enabled
/// packs must have unique priorities, and higher priorities shadow lower
/// priorities in the VFS.
[[nodiscard]] pack_result<PackManifest>
parse_pack_manifest(nx::string_view text);

using PackDeviceOpener =
    nx::function<nx::fs::io_result<Device *>(nx::string_view file)>;

struct PackMountReport {
  u32 declared = 0;
  u32 enabled = 0;
  u32 mounted = 0;
  i32 highest_priority = 0;
};

/// Reads a manifest through the VFS, validates it completely, opens every
/// enabled pack, and then mounts the set at `/`. On any failure, no package
/// from this call remains mounted. The opener receives only validated file
/// basenames and retains platform-specific control over file-backed versus
/// memory-backed ZIP devices.
[[nodiscard]] pack_result<PackMountReport>
mount_pack_manifest(nx::fs::path_view manifest_path,
                    const PackDeviceOpener &open_pack);

} // namespace nx::vfs
