#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/serialization/nva.h"
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

/// What a cooked file is named by when its source is media or a scene: what
/// it is, not the format it came from. `hero.png` and `hero.webp` both cook to
/// `hero.tex.nxb`, so one folder may hold only one of them.
inline constexpr string_view TEXTURE_SUFFIX = ".tex.nxb";
inline constexpr string_view AUDIO_SUFFIX = ".audio.nxb";
inline constexpr string_view FONT_SUFFIX = ".font.nxb";
/// A 3D scene source's sub-asset index; its sub-assets cook into a folder
/// beside it named for it, `hero.scene3d.d`.
inline constexpr string_view SCENE3D_SUFFIX = ".scene3d.nxb";
inline constexpr string_view MESH_SUFFIX = ".mesh.nxb";
inline constexpr string_view GEOMETRY_SUFFIX = ".geom.nxb";
inline constexpr string_view MATERIAL3D_SUFFIX = ".mat3d.nxb";
inline constexpr string_view SKELETON_SUFFIX = ".skel.nxb";
inline constexpr string_view SKIN_SUFFIX = ".skin.nxb";
inline constexpr string_view CLIP_SUFFIX = ".clip.nxb";
inline constexpr string_view PREFAB_SUFFIX = ".prefab.nxb";
/// A 3D world source's sub-asset index, its index and cells beside it in
/// `forest.world3d.d`.
inline constexpr string_view WORLD3D_SUFFIX = ".world3d.nxb";
inline constexpr string_view WORLD_SUFFIX = ".world.nxb";
inline constexpr string_view CELL_SUFFIX = ".cell.nxb";

/// The cooked file of the authored @p path, a source cooked by its kind: the
/// suffix its file name ends in, from its last dot, gives way to
/// @p kind_suffix (`/ui/hero.png` and TEXTURE_SUFFIX make
/// `/ui/hero.tex.nxb`). A name without a dot has @p kind_suffix appended.
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

/// Composes the core and optional-module publication contracts before asset
/// processing starts. Validation failures do not change the registry;
/// freeze() makes returned Format pointers stable and switches lookup to
/// deterministic longest-suffix order.
class Registry {
public:
  [[nodiscard]] RegistryError
  add(string_view name, std::span<const Format> formats,
      Handler *handler = nullptr,
      std::span<const string_view> embedded_sources = {});
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

  nx::vector<HandlerRecord> m_handlers;
  nx::vector<RegisteredFormat> m_formats;
  nx::vector<string_view> m_embedded_sources;
  nx::vector<usize> m_source_order;
  nx::vector<usize> m_published_order;
  nx::vector<usize> m_embedded_order;
  bool m_frozen = false;
};

inline constexpr Format physics_formats[] = {
    {".nxphysics", ".nxphysics.nxb", nva::fourcc('P', 'H', 'Y', 'S'), true},
};
inline constexpr Format atlas_formats[] = {
    {".nxatlas", ".nxatlas.nxb", nva::fourcc('A', 'T', 'L', 'S'), true},
};
inline constexpr Format generated_atlas_formats[] = {
    {".nxatlaspack", ".nxatlaspack.nxb",
     nva::fourcc('A', 'T', 'L', 'S'), true},
};
inline constexpr Format effect_formats[] = {
    {".nxfx", ".nxfx.nxb", nva::fourcc('N', 'X', 'F', 'X'), true},
};
inline constexpr Format tilemap_formats[] = {
    {".nxtilemap", ".nxtilemap.nxb", nva::fourcc('N', 'X', 'T', 'M'), true},
};
inline constexpr Format tileset_formats[] = {
    {".nxtileset", ".nxtileset.nxb", nva::fourcc('N', 'X', 'T', 'S'), true},
};
inline constexpr Format animation_formats[] = {
    {".nxanim", ".nxanim.nxb", nva::fourcc('N', 'X', 'A', 'N'), true},
};
inline constexpr Format graph_formats[] = {
    {".nxgraph", ".nxgraph.nxb", nva::fourcc('N', 'X', 'A', 'G'), true},
};
inline constexpr Format bank_formats[] = {
    {".nxbank", ".nxbank.nxb", nva::fourcc('N', 'X', 'B', 'K'), true},
};
inline constexpr Format ui_formats[] = {
    {".nxui", ".nxui.nxb", nva::fourcc('N', 'X', 'U', 'I'), true},
};
inline constexpr Format scene_formats[] = {
    {".nxscene", ".nxscene.nxb", nva::fourcc('N', 'X', 'S', 'C'), true},
};
inline constexpr Format paper_doll_formats[] = {
    {".nxpdoll", ".nxpdoll.nxb", nva::fourcc('N', 'X', 'J', 'D'), true},
};
inline constexpr Format json_document_formats[] = {
    {".nxdoc", ".nxdoc.nxb", nva::fourcc('N', 'X', 'J', 'D')},
    {".nxframe", ".nxframe.nxb", nva::fourcc('N', 'X', 'J', 'D')},
    {".nxschedule", ".nxschedule.nxb", nva::fourcc('N', 'X', 'J', 'D')},
    {".nxactions", ".nxactions.nxb", nva::fourcc('N', 'X', 'J', 'D')},
    {".nxloc", ".nxloc.nxb", nva::fourcc('N', 'X', 'J', 'D')},
    {".nxstyle", ".nxstyle.nxb", nva::fourcc('N', 'X', 'J', 'D')},
    {".nxicons", ".nxicons.nxb", nva::fourcc('N', 'X', 'J', 'D')},
    {".nxdata", ".nxdata.nxb", nva::fourcc('N', 'X', 'J', 'D')},
    {".nxmat", ".nxmat.nxb", nva::fourcc('N', 'X', 'J', 'D'), true},
};
inline constexpr Format luau_formats[] = {
    {".luau", ".luau.nxb", nva::fourcc('N', 'X', 'L', 'U')},
};
/// INI settings, as modules read their project configuration (nx::ini::load).
inline constexpr Format ini_formats[] = {
    {".ini", ".ini.nxb", nva::fourcc('N', 'X', 'I', 'N')},
};
inline constexpr Format audio_formats[] = {
    {".wav", AUDIO_SUFFIX, nva::fourcc('N', 'X', 'A', 'U'), true},
    {".ogg", AUDIO_SUFFIX, nva::fourcc('N', 'X', 'A', 'U'), true},
    {".flac", AUDIO_SUFFIX, nva::fourcc('N', 'X', 'A', 'U'), true},
    {".opus", AUDIO_SUFFIX, nva::fourcc('N', 'X', 'A', 'U'), true},
};
inline constexpr Format font_formats[] = {
    {".ttf", FONT_SUFFIX, nva::fourcc('N', 'X', 'F', 'T'), true},
    {".otf", FONT_SUFFIX, nva::fourcc('N', 'X', 'F', 'T'), true},
    {".ttc", FONT_SUFFIX, nva::fourcc('N', 'X', 'F', 'T'), true},
    {".otc", FONT_SUFFIX, nva::fourcc('N', 'X', 'F', 'T'), true},
};
/// A texture made of channels of others (assetc's composite_texture_cook.h).
inline constexpr Format composite_texture_formats[] = {
    {".nxcomposite", TEXTURE_SUFFIX, nva::fourcc('T', 'E', 'X', ' '), true},
};
inline constexpr Format texture_formats[] = {
    {".png", TEXTURE_SUFFIX, nva::fourcc('T', 'E', 'X', ' '), true},
    {".jpg", TEXTURE_SUFFIX, nva::fourcc('T', 'E', 'X', ' '), true},
    {".jpeg", TEXTURE_SUFFIX, nva::fourcc('T', 'E', 'X', ' '), true},
    {".webp", TEXTURE_SUFFIX, nva::fourcc('T', 'E', 'X', ' '), true},
    {".hdr", TEXTURE_SUFFIX, nva::fourcc('T', 'E', 'X', ' '), true},
    {".ktx2", TEXTURE_SUFFIX, nva::fourcc('T', 'E', 'X', ' '), true},
};

inline constexpr Family core_families[] = {
    {"physics", physics_formats},
    {"atlas", atlas_formats},
    {"generated-atlas", generated_atlas_formats},
    {"effect", effect_formats},
    {"tilemap", tilemap_formats},
    {"tileset", tileset_formats},
    {"animation", animation_formats},
    {"graph", graph_formats},
    {"bank", bank_formats},
    {"ui", ui_formats},
    {"scene", scene_formats},
    {"paper-doll", paper_doll_formats},
    {"json-document", json_document_formats},
    {"luau", luau_formats},
    {"ini", ini_formats},
    {"audio", audio_formats},
    {"font", font_formats},
    {"texture", texture_formats},
    {"composite-texture", composite_texture_formats},
};

} // namespace nx::asset_contract
