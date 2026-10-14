#pragma once

namespace nx::asset_policy {

/// Authored files are an editor/development convenience. A Shipping runtime
/// accepts only cooked assets, including from overlays and downloadable packs.
#if defined(NX_BUILD_SHIPPING)
inline constexpr bool authored_source_fallback_enabled = false;
#else
inline constexpr bool authored_source_fallback_enabled = true;
#endif

/// A malformed or unreadable cooked sibling must never be hidden by its source.
/// Source fallback is considered only when the cooked asset is genuinely
/// absent.
[[nodiscard]] constexpr bool
can_fallback_to_authored_source(const bool cooked_exists) noexcept {
  return authored_source_fallback_enabled && !cooked_exists;
}

} // namespace nx::asset_policy
