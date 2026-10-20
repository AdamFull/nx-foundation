#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/strings/utf8_string_view.h"

#include <optional>
#include <span>

namespace nx::asset_contract {

using Handler = u32;
inline constexpr Handler INVALID_HANDLER = ~Handler{0};

enum class SourceKind : u8 {
  Cookable,
  PreCooked,
  AuthoringOnly,
  Unsupported,
};

/// The cooked file of the authored @p path, a source cooked by its kind: the
/// suffix its file name ends in, from its last dot, gives way to
/// @p kind_suffix (`/ui/hero.png` and `.tex.nxb` make `/ui/hero.tex.nxb`). A
/// name without a dot has @p kind_suffix appended.
[[nodiscard]] string cooked_path(string_view path, string_view kind_suffix);

/// One source suffix and the exact versioned container it may publish. Formats
/// may publish one suffix only when they publish one container, so a cooked
/// name tells its container though not which of them made it. The descriptor
/// and its strings must outlive the Registry they are added to.
struct Format {
  string_view source_suffix;
  string_view published_suffix;
  u32 nva_format = 0;
  bool identity_bearing = false;
};

struct Family {
  string_view name;
  std::span<const Format> formats;
};

struct SourceMatch {
  SourceKind kind = SourceKind::Unsupported;
  Handler handler = INVALID_HANDLER;
  const Format *format = nullptr;

  [[nodiscard]] constexpr bool cookable() const noexcept {
    return kind == SourceKind::Cookable && handler != INVALID_HANDLER &&
           format != nullptr;
  }
};

struct PublishedMatch {
  Handler handler = INVALID_HANDLER;
  const Format *format = nullptr;
};

enum class RegistryError : u8 {
  None,
  Frozen,
  EmptyName,
  EmptyFormats,
  EmptySuffix,
  InvalidSourceSuffix,
  InvalidPublishedSuffix,
  InvalidFormat,
  DuplicateName,
  DuplicateSourceSuffix,
  DuplicatePublishedSuffix,
};

[[nodiscard]] string_view to_string(RegistryError error) noexcept;

/// Composes publication contracts before asset processing starts. Which
/// formats exist is the caller's: the registry knows only `.nxb` containers.
/// Validation failures do not change the registry; freeze() makes returned
/// Format pointers stable and switches lookup to deterministic longest-suffix
/// order.
class Registry {
public:
  Registry() = default;
  /// @p authoring_only names files that never publish, such as an importer's
  /// `.meta`; no family may claim those suffixes. It must outlive the
  /// registry.
  explicit Registry(std::span<const string_view> authoring_only) noexcept
      : m_authoring_only(authoring_only) {}

  [[nodiscard]] RegistryError
  add(string_view name, std::span<const Format> formats,
      Handler *handler = nullptr,
      std::span<const string_view> embedded_sources = {});
  /// Every container published under @p directory (`shaders`) is @p format,
  /// whatever its name: for outputs named for what made them, not a kind.
  [[nodiscard]] RegistryError add_directory(string_view directory,
                                            const Format &format);
  void freeze();

  [[nodiscard]] bool frozen() const noexcept { return m_frozen; }
  [[nodiscard]] usize handler_count() const noexcept {
    return m_handlers.size();
  }
  [[nodiscard]] usize format_count() const noexcept { return m_formats.size(); }
  [[nodiscard]] usize embedded_source_count() const noexcept {
    return m_embedded_sources.size();
  }
  [[nodiscard]] string_view handler_name(Handler handler) const noexcept;

  [[nodiscard]] SourceMatch classify_source(string_view path) const noexcept;
  [[nodiscard]] std::optional<PublishedMatch>
  find_published(string_view path) const noexcept;
  [[nodiscard]] std::optional<u32>
  published_format(string_view path) const noexcept;

private:
  struct HandlerRecord {
    string_view name;
  };
  struct RegisteredFormat {
    Format format;
    Handler handler = INVALID_HANDLER;
  };
  struct DirectoryFormat {
    string_view directory;
    Format format;
  };

  [[nodiscard]] bool reserved_source_suffix(string_view suffix) const noexcept;

  std::span<const string_view> m_authoring_only;
  nx::vector<DirectoryFormat> m_directories;
  nx::vector<HandlerRecord> m_handlers;
  nx::vector<RegisteredFormat> m_formats;
  nx::vector<string_view> m_embedded_sources;
  nx::vector<usize> m_source_order;
  nx::vector<usize> m_published_order;
  nx::vector<usize> m_embedded_order;
  bool m_frozen = false;
};

} // namespace nx::asset_contract
