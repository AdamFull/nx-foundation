#pragma once

/**
 * @file reflect_json.h
 * @brief Walking a described type into a document and back (nx::json).
 */

#include "foundation/reflection/type.h"
#include "foundation/serialization/json.h"

namespace nx::json {

enum class write_mode : u8 {
  elide_defaults,
  every_field,
};

[[nodiscard]] value write_fields(const reflect::type_info &type,
                                 const void *object,
                                 write_mode mode = write_mode::elide_defaults);

void read_fields(const reflect::type_info &type, const value &in, void *object);

template <class T>
[[nodiscard]] value
write_fields(const T &object,
             const write_mode mode = write_mode::elide_defaults) {
  return write_fields(reflect::type_of<T>(), &object, mode);
}

template <class T> void read_fields(const value &in, T &object) {
  read_fields(reflect::type_of<T>(), in, &object);
}

} // namespace nx::json
