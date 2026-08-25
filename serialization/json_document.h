#pragma once

#include "core/foundation/serialization/json.h"

#include <span>

namespace nx::json {

/// Parses an authored JSON asset. JSONC line/block comments and trailing
/// commas are accepted for authoring convenience, then object members whose
/// names begin with '_' (or equal "$comment") are removed recursively. Those
/// names are reserved for editor notes and must never enter a cooked payload.
[[nodiscard]] result<value> parse_asset_document(nx::string_view text);

/// Removes the reserved authoring-only members from an already parsed tree.
/// Array elements are retained, but objects nested inside them are cleaned.
void strip_asset_comments(value &root);

/// Parses and rewrites an authored JSON asset as compact strict JSON. Useful
/// for typed cookers and resource bundles whose existing parsers own the
/// runtime schema but whose embedded source must not retain comments.
[[nodiscard]] result<nx::string>
normalize_asset_document(nx::string_view text);

/// Encodes an arbitrary JSON value tree in the engine's versioned NVA
/// container. This is the common cooked representation for JSON-backed
/// configuration assets whose runtime schema remains owned by their consumer.
[[nodiscard]] nx::vector<u8> encode_document(const value &root);

/// Decodes a document produced by encode_document. False means the bytes are
/// not a supported JSON document container; @p out is left unchanged.
[[nodiscard]] bool decode_document(std::span<const u8> bytes, value &out);

/// The cooked sibling of an authored JSON asset. The source extension remains
/// part of the name so different authored formats cannot collide.
[[nodiscard]] nx::string document_cooked_path(nx::string_view source);

/// Loads the cooked sibling first. Editor/development builds may use authored
/// JSON only when the cooked sibling is absent. Shipping builds require the
/// cooked asset, and a present but unreadable or malformed cook always fails.
[[nodiscard]] bool load_document_file(nx::string_view source, value &out);

} // namespace nx::json
