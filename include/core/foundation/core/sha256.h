#pragma once

#include "core/foundation/core/foundation.h"

#include <array>
#include <span>

namespace nx {

inline constexpr usize SHA256_BYTES = 32;

/// The SHA-256 digest (FIPS 180-4) of @p bytes.
[[nodiscard]] std::array<u8, SHA256_BYTES>
sha256(std::span<const u8> bytes) noexcept;

} // namespace nx
