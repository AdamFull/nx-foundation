#pragma once

#include "core/foundation/reflection/type.h"
#include "core/foundation/serialization/nva.h"

namespace nx::reflect {

void write_binary(const type_info &type, const void *object, nx::nva::Writer &w);
[[nodiscard]] bool read_binary(const type_info &type, nx::nva::Reader &r,
                               void *object);

template <class T> void write_binary(const T &object, nx::nva::Writer &w) {
  write_binary(type_of<T>(), &object, w);
}
template <class T>
[[nodiscard]] bool read_binary(nx::nva::Reader &r, T &object) {
  return read_binary(type_of<T>(), r, &object);
}

}
