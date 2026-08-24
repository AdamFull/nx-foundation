#pragma once

#include "core/foundation/serialization/json.h"

#include <span>

namespace nx::json {

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
