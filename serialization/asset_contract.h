#pragma once

#include "core/foundation/serialization/nva.h"
#include "core/foundation/strings/utf8_string.h"

#include <optional>

namespace nx::asset_contract {

/// The production representations understood by the core cooker. A source
/// path that is Unsupported may remain available in development, but must not
/// pass a Shipping cook until its owning system supplies a real binary format.
enum class Kind : u8 {
  Physics,
  Atlas,
  Effect,
  Tilemap,
  Tileset,
  Graph,
  Bank,
  Ui,
  Scene,
  JsonDocument,
  Luau,
  Audio,
  Font,
  Texture,
  PreCooked,
  AuthoringOnly,
  Unsupported,
};

struct Format {
  Kind kind;
  string_view source_suffix;
  string_view published_suffix;
  u32 nva_format;
};

inline constexpr Format formats[] = {
    {Kind::Physics, ".nxphysics", ".nxphysics.nxb",
     nva::fourcc('P', 'H', 'Y', 'S')},
    {Kind::Atlas, ".nxatlas", ".nxatlas.nxb", nva::fourcc('A', 'T', 'L', 'S')},
    {Kind::Effect, ".nxfx", ".nxfx.nxb", nva::fourcc('N', 'X', 'F', 'X')},
    {Kind::Tilemap, ".nxtilemap", ".nxtilemap.nxb",
     nva::fourcc('N', 'X', 'T', 'M')},
    {Kind::Tileset, ".nxtileset", ".nxtileset.nxb",
     nva::fourcc('N', 'X', 'T', 'S')},
    {Kind::Graph, ".nxgraph", ".nxgraph.nxb", nva::fourcc('N', 'X', 'A', 'G')},
    {Kind::Bank, ".nxbank", ".nxbank.nxb", nva::fourcc('N', 'X', 'B', 'K')},
    {Kind::Ui, ".nxui", ".nxui.nxb", nva::fourcc('N', 'X', 'U', 'I')},
    {Kind::Scene, ".nxscene", ".nxscene.nxb", nva::fourcc('N', 'X', 'S', 'C')},
    {Kind::JsonDocument, ".nxframe", ".nxframe.nxb",
     nva::fourcc('N', 'X', 'J', 'D')},
    {Kind::JsonDocument, ".nxschedule", ".nxschedule.nxb",
     nva::fourcc('N', 'X', 'J', 'D')},
    {Kind::JsonDocument, ".nxactions", ".nxactions.nxb",
     nva::fourcc('N', 'X', 'J', 'D')},
    {Kind::JsonDocument, ".nxloc", ".nxloc.nxb",
     nva::fourcc('N', 'X', 'J', 'D')},
    {Kind::JsonDocument, ".nxstyle", ".nxstyle.nxb",
     nva::fourcc('N', 'X', 'J', 'D')},
    {Kind::JsonDocument, ".nxmat", ".nxmat.nxb",
     nva::fourcc('N', 'X', 'J', 'D')},
    {Kind::Luau, ".luau", ".luau.nxb", nva::fourcc('N', 'X', 'L', 'U')},
    {Kind::Audio, ".wav", ".wav.nxb", nva::fourcc('N', 'X', 'A', 'U')},
    {Kind::Audio, ".ogg", ".ogg.nxb", nva::fourcc('N', 'X', 'A', 'U')},
    {Kind::Audio, ".flac", ".flac.nxb", nva::fourcc('N', 'X', 'A', 'U')},
    {Kind::Audio, ".opus", ".opus.nxb", nva::fourcc('N', 'X', 'A', 'U')},
    {Kind::Font, ".ttf", ".ttf.nxb", nva::fourcc('N', 'X', 'F', 'T')},
    {Kind::Font, ".otf", ".otf.nxb", nva::fourcc('N', 'X', 'F', 'T')},
    {Kind::Font, ".ttc", ".ttc.nxb", nva::fourcc('N', 'X', 'F', 'T')},
    {Kind::Font, ".otc", ".otc.nxb", nva::fourcc('N', 'X', 'F', 'T')},
    {Kind::Texture, ".png", ".png.nxb", nva::fourcc('T', 'E', 'X', ' ')},
    {Kind::Texture, ".hdr", ".hdr.nxb", nva::fourcc('T', 'E', 'X', ' ')},
};

[[nodiscard]] constexpr bool ends_with(const string_view value,
                                       const string_view suffix) noexcept {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

[[nodiscard]] constexpr bool starts_with(const string_view value,
                                         const string_view prefix) noexcept {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] constexpr Kind classify_source(const string_view path) noexcept {
  for (const Format &format : formats)
    if (ends_with(path, format.source_suffix))
      return format.kind;

  if (ends_with(path, ".nxb"))
    return Kind::PreCooked;

  constexpr string_view authoring_only[] = {".meta", ".aseprite", ".slang",
                                            ".spv",  ".metallib", ".refl.json"};
  for (const string_view suffix : authoring_only)
    if (ends_with(path, suffix))
      return Kind::AuthoringOnly;
  return Kind::Unsupported;
}

/// Returns the exact container format allowed at a published relative path.
/// Shader outputs are the one core format whose source extension is replaced
/// by the shader compiler rather than retained before `.nxb`.
[[nodiscard]] constexpr std::optional<u32>
published_format(const string_view path) noexcept {
  const bool shader =
      starts_with(path, "shaders/") || starts_with(path, "shaders\\");
  if (shader && ends_with(path, ".nxb"))
    return nva::fourcc('S', 'H', 'D', 'R');
  for (const Format &format : formats)
    if (ends_with(path, format.published_suffix))
      return format.nva_format;
  return std::nullopt;
}

} // namespace nx::asset_contract
