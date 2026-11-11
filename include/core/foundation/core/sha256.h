#pragma once

#include "core/foundation/core/foundation.h"

#include <array>
#include <span>

namespace nx {

inline constexpr usize SHA256_BYTES = 32;

/// The SHA-256 digest (FIPS 180-4) of @p bytes.
[[nodiscard]] std::array<u8, SHA256_BYTES>
sha256(std::span<const u8> bytes) noexcept;

[[nodiscard]] inline std::array<u8, SHA256_BYTES>
sha256(const std::span<const std::byte> bytes) noexcept {
  return sha256(std::span<const u8>(reinterpret_cast<const u8 *>(bytes.data()),
                                    bytes.size()));
}

namespace detail {

/// sha256 without the processor's SHA instructions, for comparing the two.
[[nodiscard]] std::array<u8, SHA256_BYTES>
sha256_portable(std::span<const u8> bytes) noexcept;
/// Whether sha256 uses the processor's SHA instructions.
[[nodiscard]] bool sha256_accelerated() noexcept;

} // namespace detail

} // namespace nx
