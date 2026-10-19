#pragma once

#include <algorithm>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>

#include "core/foundation/containers/hash_map.h"
#include "core/foundation/containers/small_vector.h"
#include "core/foundation/containers/sparse_set.h"
#include "core/foundation/core/callable.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/core/handle.h"
#include "core/foundation/core/hash.h"
#include "core/foundation/reflection/aggregate.h"
#include "core/foundation/strings/utf8_string_view.h"

#include <cassert>

namespace nx::ecs {

template <typename Entity, typename Alloc> class basic_registry;

enum class destroy_policy : u8 {
  destroy_descendants,
  detach_children,
  reparent_to_grandparent,
};

enum class component_flags : u32 {
  none = 0,
  empty = 1u << 0,
  /// Type cannot be copied; recorded for tooling and serialization layers.
  move_only = 1u << 1,
  /// Its bytes are its value. What lets a caller that only knows a name copy
  /// one, which a compile-time caller checks with a static_assert instead.
  trivially_copyable = 1u << 2,
};

[[nodiscard]] constexpr component_flags operator|(component_flags a,
                                                  component_flags b) noexcept {
  return static_cast<component_flags>(static_cast<u32>(a) |
                                      static_cast<u32>(b));
}
[[nodiscard]] constexpr bool has_flag(component_flags v,
                                      component_flags f) noexcept {
  return (static_cast<u32>(v) & static_cast<u32>(f)) != 0;
}

namespace detail {

// The id of a type is stable for the lifetime of the process and is used as the dense storage slot
// for that type.
using component_id = u32;

inline component_id next_component_id() noexcept {
  static component_id counter = 0;
  return counter++;
}

template <typename T> [[nodiscard]] component_id component_id_of() noexcept {
  static const component_id id = next_component_id();
  return id;
}

// Not used to run the lifetime - the sparse_set already calls the real ctor/dtor - but recorded so
// reflection and serialization layers can manipulate components generically.
struct component_lifecycle {
  void (*default_construct)(void *dst) = nullptr;
  void (*destroy)(void *obj) = nullptr;
  void (*move_construct)(void *dst, void *src) = nullptr;
  void (*copy_construct)(void *dst, const void *src) = nullptr;
};

template <typename T>
[[nodiscard]] component_lifecycle make_lifecycle() noexcept {
  component_lifecycle lc{};
  if constexpr (std::is_default_constructible_v<T>)
    lc.default_construct = [](void *dst) { ::new (dst) T(); };
  lc.destroy = [](void *obj) { static_cast<T *>(obj)->~T(); };
  if constexpr (std::is_move_constructible_v<T>)
    lc.move_construct = [](void *dst, void *src) {
      ::new (dst) T(std::move(*static_cast<T *>(src)));
    };
  if constexpr (std::is_copy_constructible_v<T>)
    lc.copy_construct = [](void *dst, const void *src) {
      ::new (dst) T(*static_cast<const T *>(src));
    };
  return lc;
}

}

struct component_desc {
  /// Human-readable type name for debugging and statistics (not owned).
  string_view name = {};
  component_flags flags = component_flags::none;
};

/// Metadata per registered component type, owned by the registry - the storage
/// pointer included, so do not free it. Live counts and capacity are not
/// cached here; ask statistics, so they never go stale.
struct component_metadata {
  detail::component_id id = 0;
  string_view name = {};
  /// nx::reflect::type_name of the component, for matching code that names it
  /// by type against code that names it as registered.
  string_view type_name = {};
  u32 size = 0;
  u32 alignment = 0;
  component_flags flags = component_flags::none;
  detail::component_lifecycle lifecycle{};
  /// Opaque pointer to the type-erased storage base; owned by the registry.
  void *storage = nullptr;
};

namespace detail {

inline constexpr u32 no_dense_index = ~u32{0};

template <typename Entity, typename Alloc> class storage_base {
public:
  using registry_type = basic_registry<Entity, Alloc>;

  virtual ~storage_base() = default;

  [[nodiscard]] virtual bool contains(Entity e) const noexcept = 0;
  virtual bool remove(registry_type &reg, Entity e) = 0;
  virtual void clear(registry_type &reg) = 0;
  virtual void reserve(usize n) = 0;

  [[nodiscard]] virtual usize size() const noexcept = 0;
  [[nodiscard]] virtual usize capacity() const noexcept = 0;
  [[nodiscard]] virtual usize element_size() const noexcept = 0;
  [[nodiscard]] virtual std::span<const Entity> entities() const noexcept = 0;
  [[nodiscard]] virtual void *packed() noexcept = 0;
  [[nodiscard]] virtual void *element(Entity e) noexcept = 0;

  [[nodiscard]] virtual u32 dense_index(Entity e) const noexcept = 0;
  [[nodiscard]] virtual void *emplace_default(registry_type &reg, Entity e) = 0;

  virtual void self_delete() noexcept = 0;
};

}

namespace detail {

template <typename Entity, typename Alloc> struct event_sink {
  using registry_type = basic_registry<Entity, Alloc>;
  using callback = nx::function<void(registry_type &, Entity)>;
  using cb_alloc =
      typename std::allocator_traits<Alloc>::template rebind_alloc<callback>;

  vector<callback, cb_alloc> callbacks;

  explicit event_sink(const Alloc &a) : callbacks(cb_alloc(a)) {}

  [[nodiscard]] bool empty() const noexcept { return callbacks.empty(); }

  void fire(registry_type &reg, Entity e) {
    for (auto &cb : callbacks)
      cb(reg, e);
  }
};

template <typename T, typename Entity, typename Alloc>
class component_storage final : public storage_base<Entity, Alloc> {
public:
  using base = storage_base<Entity, Alloc>;
  using registry_type = basic_registry<Entity, Alloc>;
  using value_alloc =
      typename std::allocator_traits<Alloc>::template rebind_alloc<T>;
  using set_type = sparse_set<T, Entity, 4096, value_alloc>;
  using self_alloc = typename std::allocator_traits<
      Alloc>::template rebind_alloc<component_storage>;

  explicit component_storage(const Alloc &a)
      : m_on_construct(a), m_on_destroy(a), m_on_update(a),
        m_set(value_alloc(a)), m_alloc(a) {}

  [[nodiscard]] static component_storage *create(const Alloc &a) {
    self_alloc sa(a);
    component_storage *p = std::allocator_traits<self_alloc>::allocate(sa, 1);
    std::allocator_traits<self_alloc>::construct(sa, p, a);
    return p;
  }

  void self_delete() noexcept override {
    self_alloc sa = m_alloc;
    std::allocator_traits<self_alloc>::destroy(sa, this);
    std::allocator_traits<self_alloc>::deallocate(sa, this, 1);
  }

  [[nodiscard]] bool contains(Entity e) const noexcept override {
    return m_set.contains(e);
  }

  // A destroy callback may remove or destroy the entity it is told about; the
  // nested call finds it already being removed and leaves it to this one.
  bool remove(registry_type &reg, Entity e) override {
    if (!m_set.contains(e) ||
        std::find(m_removing.begin(), m_removing.end(), e) != m_removing.end())
      return false;
    if (!m_on_destroy.empty()) {
      m_removing.push_back(e);
      try {
        m_on_destroy.fire(reg, e);
      } catch (...) {
        m_removing.pop_back();
        throw;
      }
      m_removing.pop_back();
    }
    if (!m_set.erase(e))
      return false;
    reg.bump_generation(component_id_of<T>());
    return true;
  }

  void clear(registry_type &reg) override {
    if (!m_on_destroy.empty()) {
      for (usize i = m_set.size(); i-- > 0;)
        if (i < m_set.size())
          m_on_destroy.fire(reg, m_set.entities()[i]);
    }
    m_set.clear();
    reg.bump_generation(component_id_of<T>());
  }

  void reserve(usize n) override { m_set.reserve(n); }

  [[nodiscard]] usize size() const noexcept override { return m_set.size(); }
  [[nodiscard]] usize capacity() const noexcept override {
    return m_set.capacity();
  }
  [[nodiscard]] usize element_size() const noexcept override {
    return sizeof(T);
  }
  [[nodiscard]] std::span<const Entity> entities() const noexcept override {
    return m_set.entities();
  }

  [[nodiscard]] void *packed() noexcept override {
    if constexpr (std::is_empty_v<T>)
      return nullptr;
    else
      return m_set.values().data();
  }

  [[nodiscard]] void *element(Entity e) noexcept override {
    if constexpr (std::is_empty_v<T>)
      return nullptr;
    else
      return m_set.try_get(e);
  }

  [[nodiscard]] u32 dense_index(const Entity e) const noexcept override {
    const auto pos = m_set.try_index(e);
    return pos == decltype(m_set)::no_index ? no_dense_index : cast<u32>(pos);
  }

  [[nodiscard]] void *emplace_default(registry_type &reg,
                                      const Entity e) override {
    if constexpr (!std::is_default_constructible_v<T>) {
      (void)reg;
      (void)e;
      return nullptr;
    } else {
      if (m_set.contains(e))
        return element(e);
      m_set.emplace(e);
      reg.bump_generation(component_id_of<T>());
      if (!m_on_construct.empty())
        m_on_construct.fire(reg, e);
      T *const ref = m_set.try_get(e);
      // An empty type has a slot and no bytes; null is the honest answer and
      // means "there is nothing to write", not "it is not there".
      if constexpr (std::is_empty_v<T>) {
        (void)ref;
        return nullptr;
      } else {
        return ref;
      }
    }
  }

  [[nodiscard]] set_type &set() noexcept { return m_set; }
  [[nodiscard]] const set_type &set() const noexcept { return m_set; }

  [[nodiscard]] T &get(Entity e) noexcept { return m_set[e]; }
  [[nodiscard]] const T &get(Entity e) const noexcept { return m_set[e]; }
  [[nodiscard]] T *try_get(Entity e) noexcept { return m_set.try_get(e); }
  [[nodiscard]] const T *try_get(Entity e) const noexcept {
    return m_set.try_get(e);
  }

  event_sink<Entity, Alloc> m_on_construct;
  event_sink<Entity, Alloc> m_on_destroy;
  event_sink<Entity, Alloc> m_on_update;

private:
  set_type m_set;
  small_vector<Entity, 2> m_removing;
  NX_NO_UNIQUE_ADDRESS self_alloc m_alloc;
};

} // namespace detail

namespace detail {

template <typename Reg, typename T>
using view_storage_ptr = std::conditional_t<
    std::is_const_v<Reg>,
    const component_storage<std::remove_const_t<T>, typename Reg::entity_type,
                            typename Reg::allocator_type> *,
    component_storage<T, typename Reg::entity_type,
                      typename Reg::allocator_type> *>;

}

/// Transient, non-owning view over entities sharing components T... . Do not add or remove
/// components of any T... (or excluded U...) on the matched entities while iterating.
template <typename Reg, typename... T> class basic_view {
public:
  using entity_type = typename Reg::entity_type;
  using allocator_type = typename Reg::allocator_type;
  using base_ptr = const detail::storage_base<entity_type, allocator_type> *;

  basic_view(std::tuple<detail::view_storage_ptr<Reg, T>...> storages,
             const allocator_type &alloc)
      : m_storages(storages), m_excludes(exclude_alloc(alloc)) {}

  template <typename... U> basic_view &exclude(Reg &reg) {
    (m_excludes.push_back(reg.template raw_storage<U>()), ...);
    return *this;
  }

  [[nodiscard]] usize size_hint() const noexcept {
    usize best = NO_LEAD;
    std::apply(
        [&](auto... p) {
          ((best = p != nullptr ? nx::min(best, p->set().size()) : best), ...);
        },
        m_storages);
    return best == NO_LEAD ? 0 : best;
  }

  [[nodiscard]] bool empty() const noexcept { return size_hint() == 0; }

  template <typename Fn> void each(Fn &&fn) const {
    if (!all_present())
      return;
    walk_from(leading_index(), fn, std::index_sequence_for<T...>{});
  }

private:
  using exclude_alloc = typename std::allocator_traits<
      allocator_type>::template rebind_alloc<base_ptr>;

  static constexpr usize NO_LEAD = ~usize(0);

  [[nodiscard]] bool all_present() const noexcept {
    return std::apply([](auto... p) { return ((p != nullptr) && ...); },
                      m_storages);
  }

  [[nodiscard]] usize leading_index() const noexcept {
    usize best = NO_LEAD;
    usize best_size = NO_LEAD;
    usize at = 0;
    std::apply(
        [&](auto... p) {
          (([&] {
             if (p != nullptr && p->set().size() < best_size) {
               best_size = p->set().size();
               best = at;
             }
             ++at;
           }()),
           ...);
        },
        m_storages);
    return best;
  }

  template <typename Fn, usize... I>
  void walk_from(const usize lead, Fn &fn, std::index_sequence<I...>) const {
    (void)((I == lead ? (walk<I>(fn), true) : false) || ...);
  }

  template <usize L, typename Fn> void walk(Fn &fn) const {
    const auto &lead = std::get<L>(m_storages)->set();
    for (usize i = 0, n = lead.size(); i < n; ++i) {
      const entity_type e = lead.handle_at(i);
      if (excluded(e))
        continue;
      call<L>(fn, e, i, std::index_sequence_for<T...>{});
    }
  }

  [[nodiscard]] bool excluded(const entity_type e) const noexcept {
    for (base_ptr ex : m_excludes)
      if (ex && ex->contains(e))
        return true;
    return false;
  }

  template <usize L, typename Fn, usize... I>
  void call(Fn &fn, const entity_type e, const usize i,
            std::index_sequence<I...>) const {
    std::tuple<decltype(pointer<L, I>(e, i))...> found{};
    if ((... && ((std::get<I>(found) = pointer<L, I>(e, i)) != nullptr)))
      fn(e, *std::get<I>(found)...);
  }

  template <usize L, usize I>
  [[nodiscard]] auto pointer(const entity_type e,
                             const usize i) const noexcept {
    if constexpr (I == L)
      return std::get<I>(m_storages)->set().values().data() + i;
    else
      return std::get<I>(m_storages)->try_get(e);
  }

  std::tuple<detail::view_storage_ptr<Reg, T>...> m_storages;
  small_vector<base_ptr, 4, exclude_alloc> m_excludes;
};

namespace detail {

// Per-cached-query state owned by the registry. Hierarchy changes do not affect component
// generations and so do not invalidate cached queries.
template <typename Entity, typename Alloc> struct query_cache {
  using id_alloc = typename std::allocator_traits<Alloc>::template rebind_alloc<
      component_id>;
  using gen_alloc =
      typename std::allocator_traits<Alloc>::template rebind_alloc<u64>;
  using ent_alloc =
      typename std::allocator_traits<Alloc>::template rebind_alloc<Entity>;

  small_vector<component_id, 8, id_alloc> ids;
  small_vector<u64, 8, gen_alloc> built_generations;
  vector<Entity, ent_alloc> entities;
  bool valid = false;

  explicit query_cache(const Alloc &a)
      : ids(id_alloc(a)), built_generations(gen_alloc(a)),
        entities(ent_alloc(a)) {}

  template <typename Reg> void record_generations(const Reg &reg) {
    built_generations.clear();
    for (component_id id : ids)
      built_generations.push_back(reg.component_generation(id));
  }

  template <typename Reg>
  [[nodiscard]] bool needs_rebuild(const Reg &reg) const {
    if (!valid || built_generations.size() != ids.size())
      return true;
    for (usize i = 0; i < ids.size(); ++i)
      if (built_generations[i] != reg.component_generation(ids[i]))
        return true;
    return false;
  }
};

}

template <typename Reg, typename... T> class cached_query;

struct registry_statistics {
  usize entity_count = 0;
  usize entity_slot_count = 0;
  usize free_slot_count = 0;
  usize component_type_count = 0;
  usize component_storage_count = 0;
  usize relation_count = 0;
  usize cached_query_count = 0;
  usize approx_memory_bytes = 0;
};

struct component_type_statistics {
  detail::component_id id = 0;
  string_view name = {};
  usize size = 0;
  usize alignment = 0;
  usize count = 0;
  usize capacity = 0;
  component_flags flags = component_flags::none;
};

/// Component types must be registered with register_component<T>() before use. The default handle
/// (handle32) packs 24 index bits and 8 version bits; handle64 packs 32 and 32.
template <typename Entity = handle32, typename Alloc = Allocator<std::byte>>
class basic_registry {
public:
  using entity_type = Entity;
  using allocator_type = Alloc;
  using size_type = usize;

  static constexpr Entity null_entity = Entity{};

private:
  using repr = typename Entity::repr_type;
  using at = std::allocator_traits<Alloc>;
  using base_storage = detail::storage_base<Entity, Alloc>;

  struct entity_slot {
    repr version = 0;
    bool live = false;
  };

  struct hierarchy_node {
    Entity parent{};
    Entity first_child{};
    Entity last_child{};
    Entity prev_sibling{};
    Entity next_sibling{};
    u32 child_count = 0;
  };

  template <typename X> using rebind = typename at::template rebind_alloc<X>;

  using hier_set =
      sparse_set<hierarchy_node, Entity, 4096, rebind<hierarchy_node>>;
  using qcache = detail::query_cache<Entity, Alloc>;
  using qcache_index = hash_map<u64, usize, std::hash<u64>, std::equal_to<u64>,
                                rebind<std::pair<const u64, usize>>>;

public:
  basic_registry() = default;

  explicit basic_registry(const Alloc &alloc)
      : m_alloc(alloc), m_slots(rebind<entity_slot>(alloc)),
        m_free(rebind<repr>(alloc)), m_storages(rebind<base_storage *>(alloc)),
        m_metadata(rebind<component_metadata>(alloc)),
        m_generations(rebind<u64>(alloc)), m_hierarchy(alloc),
        m_query_caches(rebind<qcache *>(alloc)), m_query_index(alloc) {}

  basic_registry(const basic_registry &) = delete;
  basic_registry &operator=(const basic_registry &) = delete;

  basic_registry(basic_registry &&o) noexcept { move_from(std::move(o)); }
  basic_registry &operator=(basic_registry &&o) noexcept {
    if (this != &o) {
      destroy_everything();
      move_from(std::move(o));
    }
    return *this;
  }

  ~basic_registry() { destroy_everything(); }

  [[nodiscard]] allocator_type get_allocator() const noexcept {
    return m_alloc;
  }

  [[nodiscard]] Entity create() {
    repr idx;
    if (!m_free.empty()) {
      idx = m_free.back();
      m_free.pop_back();
    } else {
      assert(m_slots.size() <= Entity::max_index &&
             "registry: entity index space exhausted");
      idx = nx::cast<repr>(m_slots.size());
      m_slots.push_back(entity_slot{});
    }
    m_slots[idx].live = true;
    ++m_alive_count;
    return Entity::from_parts(idx, m_slots[idx].version);
  }

  [[nodiscard]] bool alive(Entity e) const noexcept {
    if (!e.valid())
      return false;
    const repr idx = e.index();
    return idx < m_slots.size() && m_slots[idx].live &&
           m_slots[idx].version == e.version();
  }

  void destroy(Entity e,
               destroy_policy policy = destroy_policy::detach_children) {
    if (!alive(e))
      return;
    resolve_children_on_destroy(e, policy);
    detach_from_parent(e);
    remove_all_components(e);
    if (!alive(e))
      return;
    if (m_hierarchy.contains(e))
      m_hierarchy.erase(e);

    const repr idx = e.index();
    entity_slot &s = m_slots[idx];
    s.live = false;
    if (s.version < Entity::max_version) {
      ++s.version;
      m_free.push_back(idx);
    }
    --m_alive_count;
  }

  void clear() {
    for (base_storage *s : m_storages)
      if (s)
        s->clear(*this);
    for (u64 &g : m_generations)
      ++g;
    m_hierarchy.clear();
    m_relation_count = 0;
    for (usize i = 0; i < m_slots.size(); ++i) {
      entity_slot &slot = m_slots[i];
      if (!slot.live)
        continue;
      slot.live = false;
      if (slot.version < Entity::max_version) {
        ++slot.version;
        m_free.push_back(nx::cast<repr>(i));
      }
    }
    m_alive_count = 0;
    for (qcache *q : m_query_caches)
      if (q)
        q->valid = false;
  }

  void release_components() noexcept {
    clear();
    destroy_everything();
    m_metadata.clear();
    m_generations.clear();
    m_query_index.clear();
    m_registered_count = 0;
  }

  void reserve(size_type n) {
    m_slots.reserve(n);
    m_free.reserve(n);
  }

  [[nodiscard]] size_type size() const noexcept { return m_alive_count; }
  [[nodiscard]] bool empty() const noexcept { return m_alive_count == 0; }

  /// Register a component type. Once per type, before any emplace/get/view/ query for it.
  /// Registering twice is a usage error (asserts in debug) and leaves the first registration in
  /// place. @return Reference to the stored metadata for T.
  template <typename C>
  const component_metadata &
  register_component(const component_desc &desc = {}) {
    using T = std::remove_cv_t<C>;
    const detail::component_id id = detail::component_id_of<T>();
    ensure_tables(id);
    if (m_storages[id] != nullptr) {
      assert(false && "registry: component type registered more than once");
      return m_metadata[id];
    }

    auto *store = detail::component_storage<T, Entity, Alloc>::create(m_alloc);
    m_storages[id] = store;

    component_flags flags = desc.flags;
    if constexpr (std::is_empty_v<T>)
      flags = flags | component_flags::empty;
    if constexpr (!std::is_copy_constructible_v<T>)
      flags = flags | component_flags::move_only;
    if constexpr (std::is_trivially_copyable_v<T>)
      flags = flags | component_flags::trivially_copyable;

    component_metadata md{};
    md.id = id;
    md.name = desc.name;
    md.type_name = nx::reflect::type_name<T>();
    md.size = nx::cast<u32>(sizeof(T));
    md.alignment = nx::cast<u32>(alignof(T));
    md.flags = flags;
    md.lifecycle = detail::make_lifecycle<T>();
    md.storage = store;
    m_metadata[id] = md;
    ++m_registered_count;
    return m_metadata[id];
  }

  template <typename C>
  [[nodiscard]] bool is_component_registered() const noexcept {
    const detail::component_id id =
        detail::component_id_of<std::remove_cv_t<C>>();
    return id < m_storages.size() && m_storages[id] != nullptr;
  }

  [[nodiscard]] const component_metadata *
  component_metadata_named(const string_view name) const noexcept {
    if (name.empty())
      return nullptr;
    for (usize id = 0; id < m_storages.size(); ++id)
      if (m_storages[id] != nullptr && m_metadata[id].name == name)
        return &m_metadata[id];
    return nullptr;
  }

  [[nodiscard]] const component_metadata *
  component_metadata_typed(const string_view type_name) const noexcept {
    if (type_name.empty())
      return nullptr;
    for (usize id = 0; id < m_storages.size(); ++id)
      if (m_storages[id] != nullptr && m_metadata[id].type_name == type_name)
        return &m_metadata[id];
    return nullptr;
  }

  [[nodiscard]] usize registered_component_count() const noexcept {
    return m_registered_count;
  }

  template <typename C>
  [[nodiscard]] const component_metadata &
  component_metadata_of() const noexcept {
    const detail::component_id id =
        detail::component_id_of<std::remove_cv_t<C>>();
    assert(id < m_storages.size() && m_storages[id] != nullptr &&
           "registry: component type not registered");
    return m_metadata[id];
  }

  template <typename C, typename... Args> C &emplace(Entity e, Args &&...args) {
    using T = std::remove_cv_t<C>;
    auto &store = assure<T>();
    assert(alive(e) && "registry::emplace: entity not alive");
    T &ref = store.set().emplace(e, std::forward<Args>(args)...);
    bump_generation(detail::component_id_of<T>());
    if (!store.m_on_construct.empty())
      store.m_on_construct.fire(*this, e);
    return ref;
  }

  /// Add T if missing, otherwise overwrite it. Requires T move-assignable when
  /// it already exists. Fires on_construct or on_update accordingly.
  template <typename C, typename... Args>
  C &emplace_or_replace(Entity e, Args &&...args) {
    using T = std::remove_cv_t<C>;
    auto &store = assure<T>();
    assert(alive(e) && "registry::emplace_or_replace: entity not alive");
    if (T *p = store.try_get(e)) {
      *p = T(std::forward<Args>(args)...);
      if (!store.m_on_update.empty())
        store.m_on_update.fire(*this, e);
      return *p;
    }
    T &ref = store.set().emplace(e, std::forward<Args>(args)...);
    bump_generation(detail::component_id_of<T>());
    if (!store.m_on_construct.empty())
      store.m_on_construct.fire(*this, e);
    return ref;
  }

  template <typename C, typename... Args>
  C &insert_or_assign(Entity e, Args &&...args) {
    return emplace_or_replace<C>(e, std::forward<Args>(args)...);
  }

  template <typename C, typename... Args>
  C *try_emplace_or_replace(Entity e, Args &&...args) {
    if (!alive(e))
      return nullptr;
    return &emplace_or_replace<C>(e, std::forward<Args>(args)...);
  }

  template <typename C, typename... Args> C &replace(Entity e, Args &&...args) {
    using T = std::remove_cv_t<C>;
    auto &store = assure<T>();
    T *p = store.try_get(e);
    assert(p && "registry::replace: entity does not have component");
    *p = T(std::forward<Args>(args)...);
    if (!store.m_on_update.empty())
      store.m_on_update.fire(*this, e);
    return *p;
  }

  template <typename C, typename Fn> C &patch(Entity e, Fn &&fn) {
    using T = std::remove_cv_t<C>;
    auto &store = assure<T>();
    T *p = store.try_get(e);
    assert(p && "registry::patch: entity does not have component");
    fn(*p);
    if (!store.m_on_update.empty())
      store.m_on_update.fire(*this, e);
    return *p;
  }

  template <typename... C> [[nodiscard]] decltype(auto) get(Entity e) {
    static_assert(sizeof...(C) >= 1, "registry::get requires a type");
    if constexpr (sizeof...(C) == 1)
      return (get_single<std::remove_cv_t<C>...>(e));
    else
      return std::forward_as_tuple(get_single<std::remove_cv_t<C>>(e)...);
  }
  template <typename... C> [[nodiscard]] decltype(auto) get(Entity e) const {
    static_assert(sizeof...(C) >= 1, "registry::get requires a type");
    if constexpr (sizeof...(C) == 1)
      return (get_single<std::remove_cv_t<C>...>(e));
    else
      return std::forward_as_tuple(get_single<std::remove_cv_t<C>>(e)...);
  }

  template <typename C> [[nodiscard]] auto *try_get(Entity e) noexcept {
    using T = std::remove_cv_t<C>;
    auto *store = raw_storage<T>();
    return store ? store->try_get(e) : static_cast<T *>(nullptr);
  }
  template <typename C>
  [[nodiscard]] const auto *try_get(Entity e) const noexcept {
    using T = std::remove_cv_t<C>;
    const auto *store = raw_storage<T>();
    return store ? store->try_get(e) : static_cast<const T *>(nullptr);
  }

  template <typename C> [[nodiscard]] bool has(Entity e) const noexcept {
    const auto *store = raw_storage<std::remove_cv_t<C>>();
    return store && store->contains(e);
  }
  template <typename... C> [[nodiscard]] bool all_of(Entity e) const noexcept {
    return (has<C>(e) && ...);
  }
  template <typename... C> [[nodiscard]] bool any_of(Entity e) const noexcept {
    return (has<C>(e) || ...);
  }

  template <typename C> bool remove(Entity e) {
    using T = std::remove_cv_t<C>;
    auto *store = raw_storage<T>();
    if (!store)
      return false;
    return store->remove(*this, e);
  }

  template <typename C> void erase(Entity e) {
    const bool removed = remove<C>(e);
    assert(removed && "registry::erase: entity does not have component");
    (void)removed;
  }

  void remove_all(Entity e) { remove_all_components(e); }

  template <typename C> void clear() {
    using T = std::remove_cv_t<C>;
    auto *store = raw_storage<T>();
    if (!store)
      return;
    store->clear(*this);
  }

  template <typename C> [[nodiscard]] size_type count() const noexcept {
    const auto *store = raw_storage<std::remove_cv_t<C>>();
    return store != nullptr ? store->size() : 0;
  }

  template <typename C, typename Fn> void each_run(Fn &&fn) {
    auto *store = raw_storage<std::remove_cv_t<C>>();
    if (store == nullptr)
      return;
    const std::span<const Entity> ents = store->entities();
    if (!ents.empty())
      fn(ents, store->set().values().data());
  }

  template <typename C, typename Fn> void each_run(Fn &&fn) const {
    const auto *store = raw_storage<std::remove_cv_t<C>>();
    if (store == nullptr)
      return;
    const std::span<const Entity> ents = store->entities();
    if (!ents.empty())
      fn(ents, store->set().values().data());
  }

  template <typename C, typename Fn> void on_construct(Fn &&fn) {
    assure<std::remove_cv_t<C>>().m_on_construct.callbacks.emplace_back(
        std::forward<Fn>(fn));
  }
  template <typename C, typename Fn> void on_destroy(Fn &&fn) {
    assure<std::remove_cv_t<C>>().m_on_destroy.callbacks.emplace_back(
        std::forward<Fn>(fn));
  }
  template <typename C, typename Fn> void on_update(Fn &&fn) {
    assure<std::remove_cv_t<C>>().m_on_update.callbacks.emplace_back(
        std::forward<Fn>(fn));
  }

  template <typename... C> [[nodiscard]] auto view() {
    (assert_registered<std::remove_cv_t<C>>(), ...);
    return basic_view<basic_registry, std::remove_cv_t<C>...>(
        std::make_tuple(raw_storage<std::remove_cv_t<C>>()...), m_alloc);
  }
  template <typename... C> [[nodiscard]] auto view() const {
    (assert_registered<std::remove_cv_t<C>>(), ...);
    return basic_view<const basic_registry, std::remove_cv_t<C>...>(
        std::make_tuple(raw_storage<std::remove_cv_t<C>>()...), m_alloc);
  }

  template <typename... C> [[nodiscard]] auto query() {
    (assert_registered<std::remove_cv_t<C>>(), ...);
    qcache &c = assure_query<std::remove_cv_t<C>...>();
    return cached_query<basic_registry, std::remove_cv_t<C>...>(this, &c);
  }

  bool set_parent(Entity child, Entity parent) {
    if (!alive(child) || !alive(parent))
      return false;
    if (child == parent)
      return false;
    if (is_descendant_of(parent, child))
      return false;

    detach_from_parent(child);

    ensure_node(child);
    ensure_node(parent);
    hierarchy_node &cn = m_hierarchy.at(child);
    hierarchy_node &pn = m_hierarchy.at(parent);
    cn.parent = parent;
    cn.next_sibling = Entity{};
    cn.prev_sibling = pn.last_child;
    if (pn.last_child.valid())
      m_hierarchy.at(pn.last_child).next_sibling = child;
    else
      pn.first_child = child;
    pn.last_child = child;
    ++pn.child_count;
    ++m_relation_count;
    ++m_hierarchy_version;
    return true;
  }

  void remove_parent(Entity child) {
    if (!m_hierarchy.contains(child))
      return;
    detach_from_parent(child);
    maybe_erase_node(child);
  }

  [[nodiscard]] Entity parent(Entity e) const noexcept {
    const hierarchy_node *n = m_hierarchy.try_get(e);
    return n ? n->parent : Entity{};
  }

  [[nodiscard]] bool has_parent(Entity e) const noexcept {
    return parent(e).valid();
  }

  [[nodiscard]] usize child_count(Entity e) const noexcept {
    const hierarchy_node *n = m_hierarchy.try_get(e);
    return n ? n->child_count : 0;
  }

  [[nodiscard]] Entity next_sibling(Entity e) const noexcept {
    const hierarchy_node *n = m_hierarchy.try_get(e);
    return n ? n->next_sibling : Entity{};
  }

  template <typename Fn> void each_child(Entity e, Fn &&fn) const {
    const hierarchy_node *n = m_hierarchy.try_get(e);
    if (!n)
      return;
    Entity c = n->first_child;
    while (c.valid()) {
      const Entity next = next_sibling(c);
      fn(c);
      c = next;
    }
  }

  template <typename Fn> void each_descendant(Entity e, Fn &&fn) const {
    small_vector<Entity, 32, rebind<Entity>> stack{rebind<Entity>(m_alloc)};
    push_children(e, stack);
    while (!stack.empty()) {
      const Entity c = stack.back();
      stack.pop_back();
      fn(c);
      push_children(c, stack);
    }
  }

  [[nodiscard]] bool is_descendant_of(Entity maybe_descendant,
                                      Entity ancestor) const noexcept {
    Entity cur = parent(maybe_descendant);
    while (cur.valid()) {
      if (cur == ancestor)
        return true;
      cur = parent(cur);
    }
    return false;
  }

  [[nodiscard]] registry_statistics statistics() const noexcept {
    registry_statistics st{};
    st.entity_count = m_alive_count;
    st.entity_slot_count = m_slots.size();
    st.free_slot_count = m_free.size();
    st.component_type_count = m_registered_count;
    for (const base_storage *s : m_storages)
      if (s)
        ++st.component_storage_count;
    st.relation_count = m_relation_count;
    usize live_queries = 0;
    for (const qcache *q : m_query_caches)
      if (q)
        ++live_queries;
    st.cached_query_count = live_queries;

    usize mem = m_slots.capacity() * sizeof(entity_slot) +
                m_free.capacity() * sizeof(repr) +
                m_hierarchy.capacity() * sizeof(hierarchy_node);
    for (const base_storage *s : m_storages)
      if (s)
        mem += s->capacity() * s->element_size();
    for (const qcache *q : m_query_caches)
      if (q)
        mem += q->entities.capacity() * sizeof(Entity);
    st.approx_memory_bytes = mem;
    return st;
  }

  template <typename C>
  [[nodiscard]] component_type_statistics
  component_statistics() const noexcept {
    using T = std::remove_cv_t<C>;
    const auto *store = raw_storage<T>();
    assert(store && "registry::component_statistics: type not registered");
    const component_metadata &md = component_metadata_of<T>();
    component_type_statistics st{};
    st.id = md.id;
    st.name = md.name;
    st.size = md.size;
    st.alignment = md.alignment;
    st.count = store->size();
    st.capacity = store->capacity();
    st.flags = md.flags;
    return st;
  }

  [[nodiscard]] u64
  component_generation(detail::component_id id) const noexcept {
    return id < m_generations.size() ? m_generations[id] : 0;
  }

  void rebuild_query(qcache &c) {
    c.entities.clear();
    base_storage *lead = nullptr;
    usize lead_size = ~usize(0);
    for (detail::component_id id : c.ids) {
      base_storage *s = id < m_storages.size() ? m_storages[id] : nullptr;
      if (!s) {
        c.record_generations(*this);
        c.valid = true;
        return;
      }
      if (s->size() < lead_size) {
        lead_size = s->size();
        lead = s;
      }
    }
    if (lead) {
      for (Entity e : lead->entities()) {
        bool all = true;
        for (detail::component_id id : c.ids) {
          if (!m_storages[id]->contains(e)) {
            all = false;
            break;
          }
        }
        if (all)
          c.entities.push_back(e);
      }
    }
    c.record_generations(*this);
    c.valid = true;
  }

private:
  template <typename Reg, typename... T> friend class basic_view;
  template <typename, typename, typename>
  friend class detail::component_storage;

  template <typename C>
  [[nodiscard]] detail::component_storage<std::remove_cv_t<C>, Entity, Alloc> *
  raw_storage() noexcept {
    using T = std::remove_cv_t<C>;
    const detail::component_id id = detail::component_id_of<T>();
    if (id >= m_storages.size() || m_storages[id] == nullptr)
      return nullptr;
    return static_cast<detail::component_storage<T, Entity, Alloc> *>(
        m_storages[id]);
  }
  template <typename C>
  [[nodiscard]] const detail::component_storage<std::remove_cv_t<C>, Entity,
                                                Alloc> *
  raw_storage() const noexcept {
    using T = std::remove_cv_t<C>;
    const detail::component_id id = detail::component_id_of<T>();
    if (id >= m_storages.size() || m_storages[id] == nullptr)
      return nullptr;
    return static_cast<const detail::component_storage<T, Entity, Alloc> *>(
        m_storages[id]);
  }

  template <typename T> void assert_registered() const noexcept {
    assert(is_component_registered<T>() &&
           "registry: component type not registered");
  }

  template <typename T> detail::component_storage<T, Entity, Alloc> &assure() {
    auto *store = raw_storage<T>();
    assert(store && "registry: component type not registered");
    return *store;
  }

  template <typename T> T &get_single(Entity e) {
    auto &store = assure<T>();
    assert(store.contains(e) && "registry::get: entity lacks component");
    return store.get(e);
  }
  template <typename T> const T &get_single(Entity e) const {
    const auto *store = raw_storage<T>();
    assert(store && "registry::get: component type not registered");
    assert(store->contains(e) && "registry::get: entity lacks component");
    return store->get(e);
  }

  void ensure_tables(detail::component_id id) {
    if (id >= m_storages.size()) {
      m_storages.resize(id + 1, nullptr);
      m_metadata.resize(id + 1);
      m_generations.resize(id + 1, 0);
    }
  }

  void bump_generation(detail::component_id id) noexcept {
    if (id < m_generations.size())
      ++m_generations[id];
  }

  void remove_all_components(Entity e) {
    for (usize id = 0; id < m_storages.size(); ++id) {
      base_storage *s = m_storages[id];
      if (s && s->contains(e))
        s->remove(*this, e);
    }
  }

  hierarchy_node &ensure_node(Entity e) {
    if (hierarchy_node *n = m_hierarchy.try_get(e))
      return *n;
    return m_hierarchy.emplace(e, hierarchy_node{});
  }

  void maybe_erase_node(Entity e) {
    hierarchy_node *n = m_hierarchy.try_get(e);
    if (n && !n->parent.valid() && !n->first_child.valid())
      m_hierarchy.erase(e);
  }

  void detach_from_parent(Entity child) {
    hierarchy_node *cn = m_hierarchy.try_get(child);
    if (!cn || !cn->parent.valid())
      return;
    const Entity p = cn->parent;
    hierarchy_node &pn = m_hierarchy.at(p);

    const Entity prev = cn->prev_sibling;
    const Entity next = cn->next_sibling;
    if (prev.valid())
      m_hierarchy.at(prev).next_sibling = next;
    else
      pn.first_child = next;
    if (next.valid())
      m_hierarchy.at(next).prev_sibling = prev;
    else
      pn.last_child = prev;

    cn->parent = Entity{};
    cn->prev_sibling = Entity{};
    cn->next_sibling = Entity{};
    if (pn.child_count > 0)
      --pn.child_count;
    --m_relation_count;
    ++m_hierarchy_version;
    maybe_erase_node(p);
  }

  template <usize N, typename A>
  void push_children(Entity e, small_vector<Entity, N, A> &stack) const {
    const hierarchy_node *n = m_hierarchy.try_get(e);
    if (!n)
      return;
    Entity c = n->first_child;
    while (c.valid()) {
      stack.push_back(c);
      c = next_sibling(c);
    }
  }

  void resolve_children_on_destroy(Entity e, destroy_policy policy) {
    const hierarchy_node *n = m_hierarchy.try_get(e);
    if (!n || !n->first_child.valid())
      return;

    small_vector<Entity, 16, rebind<Entity>> kids{rebind<Entity>(m_alloc)};
    Entity c = n->first_child;
    while (c.valid()) {
      kids.push_back(c);
      c = next_sibling(c);
    }

    switch (policy) {
    case destroy_policy::destroy_descendants:
      for (Entity k : kids)
        destroy(k, policy);
      break;
    case destroy_policy::detach_children:
      for (Entity k : kids)
        detach_from_parent(k);
      break;
    case destroy_policy::reparent_to_grandparent: {
      const Entity gp = n->parent;
      for (Entity k : kids) {
        detach_from_parent(k);
        if (gp.valid())
          set_parent(k, gp);
      }
      break;
    }
    }
  }

  template <typename... T> qcache &assure_query() {
    detail::component_id ids[] = {detail::component_id_of<T>()...};
    const usize n = sizeof...(T);
    for (usize i = 1; i < n; ++i) {
      detail::component_id v = ids[i];
      usize j = i;
      while (j > 0 && ids[j - 1] > v) {
        ids[j] = ids[j - 1];
        --j;
      }
      ids[j] = v;
    }
    nx::fnv1a64 hash;
    for (usize i = 0; i < n; ++i)
      hash.combine(ids[i]);
    const u64 key = hash.value();

    if (auto it = m_query_index.find(key); it != m_query_index.end())
      return *m_query_caches[it->second];

    qcache *c = make_query_cache();
    for (usize i = 0; i < n; ++i)
      c->ids.push_back(ids[i]);
    const usize idx = m_query_caches.size();
    m_query_caches.push_back(c);
    m_query_index.insert_or_assign(key, idx);
    return *c;
  }

  qcache *make_query_cache() {
    using qalloc = rebind<qcache>;
    qalloc a(m_alloc);
    qcache *p = std::allocator_traits<qalloc>::allocate(a, 1);
    std::allocator_traits<qalloc>::construct(a, p, m_alloc);
    return p;
  }

  void destroy_query_cache(qcache *p) noexcept {
    using qalloc = rebind<qcache>;
    qalloc a(m_alloc);
    std::allocator_traits<qalloc>::destroy(a, p);
    std::allocator_traits<qalloc>::deallocate(a, p, 1);
  }

  void destroy_everything() noexcept {
    for (base_storage *s : m_storages)
      if (s)
        s->self_delete();
    m_storages.clear();
    for (qcache *q : m_query_caches)
      if (q)
        destroy_query_cache(q);
    m_query_caches.clear();
  }

  void move_from(basic_registry &&o) noexcept {
    m_alloc = o.m_alloc;
    m_slots = std::move(o.m_slots);
    m_free = std::move(o.m_free);
    m_storages = std::move(o.m_storages);
    m_metadata = std::move(o.m_metadata);
    m_generations = std::move(o.m_generations);
    m_hierarchy = std::move(o.m_hierarchy);
    m_query_caches = std::move(o.m_query_caches);
    m_query_index = std::move(o.m_query_index);
    m_alive_count = o.m_alive_count;
    m_relation_count = o.m_relation_count;
    m_registered_count = o.m_registered_count;
    m_hierarchy_version = o.m_hierarchy_version;
    o.m_storages.clear();
    o.m_query_caches.clear();
    o.m_alive_count = 0;
    o.m_relation_count = 0;
    o.m_registered_count = 0;
  }

  NX_NO_UNIQUE_ADDRESS Alloc m_alloc{};
  vector<entity_slot, rebind<entity_slot>> m_slots{
      rebind<entity_slot>(m_alloc)};
  vector<repr, rebind<repr>> m_free{rebind<repr>(m_alloc)};
  vector<base_storage *, rebind<base_storage *>> m_storages{
      rebind<base_storage *>(m_alloc)};
  vector<component_metadata, rebind<component_metadata>> m_metadata{
      rebind<component_metadata>(m_alloc)};
  vector<u64, rebind<u64>> m_generations{rebind<u64>(m_alloc)};
  hier_set m_hierarchy{m_alloc};
  vector<qcache *, rebind<qcache *>> m_query_caches{rebind<qcache *>(m_alloc)};
  qcache_index m_query_index{};
  usize m_alive_count = 0;
  usize m_relation_count = 0;
  usize m_registered_count = 0;
  u64 m_hierarchy_version = 0;
};

using registry = basic_registry<handle32, Allocator<std::byte>>;
using registry64 = basic_registry<handle64, Allocator<std::byte>>;

template <typename Reg, typename... T> class cached_query {
public:
  using entity_type = typename Reg::entity_type;
  using allocator_type = typename Reg::allocator_type;
  using cache_type = detail::query_cache<entity_type, allocator_type>;

  cached_query(Reg *reg, cache_type *cache) noexcept
      : m_reg(reg), m_cache(cache) {}

  [[nodiscard]] usize size() {
    ensure_built();
    return m_cache->entities.size();
  }
  [[nodiscard]] bool empty() { return size() == 0; }

  [[nodiscard]] std::span<const entity_type> entities() {
    ensure_built();
    return {m_cache->entities.data(), m_cache->entities.size()};
  }

  template <typename Fn> void each(Fn &&fn) {
    ensure_built();
    for (entity_type e : m_cache->entities)
      invoke(std::forward<Fn>(fn), e, std::index_sequence_for<T...>{});
  }

private:
  void ensure_built() {
    if (m_cache->needs_rebuild(*m_reg))
      m_reg->rebuild_query(*m_cache);
  }

  template <typename Fn, usize... I>
  void invoke(Fn &&fn, entity_type e, std::index_sequence<I...>) {
    fn(e, m_reg->template get<T>(e)...);
  }

  Reg *m_reg;
  cache_type *m_cache;
};

}
