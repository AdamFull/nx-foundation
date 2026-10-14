#pragma once

#include "core/foundation/reflection/type.h"
#include "core/foundation/strings/utf8_string.h"

namespace nx::reflect {

[[nodiscard]] u64 layout_digest(const type_info &type) noexcept;

template <class T> [[nodiscard]] u64 layout_digest() {
  return layout_digest(type_of<std::remove_cv_t<T>>());
}

[[nodiscard]] string describe_layout(const type_info &type);

}
