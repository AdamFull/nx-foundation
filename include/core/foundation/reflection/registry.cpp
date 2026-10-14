#include "core/foundation/reflection/registry.h"

#include "core/foundation/containers/hash_map.h"
#include "core/foundation/diagnostics/log.h"
#include "core/foundation/threading/sync.h"

namespace nx::reflect {
namespace {

const nx::log::Category log_reflect = nx::log::category("reflect");

struct table {
  mutable nx::mutex guard;
  nx::hash_map<type_id, const type_info *> by_id;
};

[[nodiscard]] table &storage() {
  static table t;
  return t;
}

}

type_registry &type_registry::instance() {
  static type_registry registry;
  return registry;
}

void type_registry::add(const type_info &type) {
  table &t = storage();
  const nx::scoped_lock lock(t.guard);
  const auto it = t.by_id.find(type.id);
  if (it != t.by_id.end()) {
    if (it->second != &type)
      nx::loge(log_reflect, "'{}' and '{}' share an id; the second is ignored",
               it->second->name, type.name);
    return;
  }
  t.by_id.insert_or_assign(type.id, &type);
}

const type_info *type_registry::find(const type_id id) const {
  const table &t = storage();
  const nx::scoped_lock lock(t.guard);
  const auto it = t.by_id.find(id);
  return it == t.by_id.end() ? nullptr : it->second;
}

const type_info *type_registry::find(const string_view name) const {
  return find(hash_fnv1a64(name.data(), name.size()));
}

usize type_registry::size() const {
  const table &t = storage();
  const nx::scoped_lock lock(t.guard);
  return t.by_id.size();
}

nx::vector<const type_info *> type_registry::all() const {
  const table &t = storage();
  const nx::scoped_lock lock(t.guard);
  nx::vector<const type_info *> out;
  out.reserve(t.by_id.size());
  for (const auto &[id, type] : t.by_id)
    out.push_back(type);
  return out;
}

}
