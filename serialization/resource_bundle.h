#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

#include <optional>
#include <span>

namespace nx::resource_bundle {

/// Conservative defaults for small, related resource sets. Callers may lower
/// these limits for their asset family; decoding never allocates from an
/// unvalidated entry count or name length.
struct Limits {
  u32 max_entries = 4096;
  usize max_name_bytes = 4096;
  usize max_metadata_bytes = 1024 * 1024;
  usize max_data_bytes = 512u * 1024u * 1024u;
};

struct Source {
  string_view name;
  std::span<const u8> bytes;
};

/// Portable relative names only: well-formed UTF-8 with no root, drive,
/// Windows-invalid character or device name, control byte, or empty/dot path
/// segment. This prevents a bundle dependency from escaping its authored
/// descriptor directory or becoming unaddressable on a supported host.
[[nodiscard]] bool valid_name(string_view name,
                              usize max_bytes = 4096) noexcept;

/// Creates a deterministic NVA container. Entries are sorted by name and
/// packed without gaps; duplicate names, empty payloads, and limit violations
/// are rejected.
[[nodiscard]] std::optional<vector<u8>> encode(u32 format, u16 version,
                                               std::span<const u8> metadata,
                                               std::span<const Source> sources,
                                               Limits limits = {});

/// A validated non-owning view. The container bytes must outlive this object.
class View {
public:
  [[nodiscard]] static std::optional<View>
  open(std::span<const u8> bytes, u32 format, u16 version, Limits limits = {});

  [[nodiscard]] std::span<const u8> metadata() const noexcept {
    return m_metadata;
  }
  [[nodiscard]] usize size() const noexcept { return m_entries.size(); }
  [[nodiscard]] std::span<const u8> find(string_view name) const noexcept;

private:
  struct Entry {
    string_view name;
    usize offset = 0;
    usize size = 0;
  };

  std::span<const u8> m_metadata;
  std::span<const u8> m_data;
  vector<Entry> m_entries;
};

} // namespace nx::resource_bundle
