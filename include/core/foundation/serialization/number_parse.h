#pragma once

#include "core/foundation/core/foundation.h"

#include <charconv>

namespace nx::number {

/// Parses a floating-point value without consulting the process numeric locale.
///
/// The accepted syntax and result pointer follow the general-format
/// std::from_chars overload. A subnormal value parses without error; on
/// result_out_of_range @p out is +-infinity for a value too large and +-0 for
/// one too small, on every platform. The fallback can additionally report
/// not_enough_memory or operation_not_supported when its temporary buffer or
/// platform C locale cannot be created.
[[nodiscard]] std::from_chars_result
parse_f64(const char *first, const char *last, f64 &out) noexcept;

} // namespace nx::number
