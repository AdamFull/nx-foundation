#pragma once

/**
 * @file layout.h
 * @brief A fingerprint of how a type is laid out (nx::reflect).
 */

#include "core/foundation/reflection/type.h"
#include "core/foundation/strings/utf8_string.h"

namespace nx::reflect {

[[nodiscard]] u64 layout_digest(const type_info &type) noexcept;

template <class T> [[nodiscard]] u64 layout_digest() {
  return layout_digest(type_of<std::remove_cv_t<T>>());
}

/// @p type as it is now, one field per line. For the message beside a digest
/// that did not match: the digest says something moved, this says to what.
[[nodiscard]] string describe_layout(const type_info &type);

} // namespace nx::reflect
