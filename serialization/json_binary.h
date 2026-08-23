#pragma once

/**
 * @file json_binary.h
 * @brief A json::value tree to and from nva bytes (namespace nx::json).
 */

#include "core/foundation/serialization/json.h"
#include "core/foundation/serialization/nva.h"

namespace nx::json {

/// Writes @p v and everything under it. Object member order is preserved.
void write_binary(const value &v, nx::nva::Writer &w);

/// Reads one value written by write_binary. A truncated or malformed stream
/// leaves @p r not-ok and returns whatever was read so far; the caller checks
/// r.ok() to tell a clean read from a partial one.
[[nodiscard]] value read_binary(nx::nva::Reader &r);

} // namespace nx::json
