#pragma once

/**
 * @file registry.h
 * @brief Finding a type by the name a document gave it (nx::reflect).
 */

#include "foundation/reflection/type.h"

namespace nx::reflect {

class type_registry {
public:
  [[nodiscard]] static type_registry &instance();

  void add(const type_info &type);
  template <class T> void add() { add(type_of<T>()); }

  [[nodiscard]] const type_info *find(type_id id) const;
  [[nodiscard]] const type_info *find(string_view name) const;

  [[nodiscard]] usize size() const;
  /// Every registered type, for a tool listing what a scene may contain.
  [[nodiscard]] nx::vector<const type_info *> all() const;

private:
  type_registry() = default;
};

} // namespace nx::reflect
