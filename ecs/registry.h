#pragma once

/**
 * @file registry.h
 * @brief Entity-component-system registry (nx::ecs::registry /
 * nx::ecs::basic_registry).
 *
 * A low-level, allocator-aware ECS container for managing entities, their
 * components, and parent-child relationships. It is a foundational building
 * block: it knows nothing about scenes, rendering, editors, or GPU types, and
 * other systems are expected to build on top of it.
 *
 * Entities are versioned handles (nx::basic_handle). Components are stored in
 * dense, cache-friendly nx::sparse_set storages, one per registered type.
 * Component types must be registered before use. Typed views and cached
 * queries iterate entities that share a set of components, and a built-in
 * intrusive hierarchy provides parent-child relations.
 *
 * @section reg_threading Thread safety
 * The registry is not internally synchronized. Concurrent reads are safe only
 * when no thread is writing. Creating or destroying entities, adding or
 * removing components, and changing the hierarchy all mutate shared state and
 * require external synchronization. Mutating the set of components of a type
 * while a view or query over that type is iterating is undefined; finish the
 * iteration first.
 *
 * @section reg_invalidation Reference and iterator invalidation
 * Component storages are swap-and-pop sparse sets, so a T& or T* obtained from
 * get()/try_get() is invalidated by any add or remove on that same component
 * type (it may refer to a different element afterwards). Refer to components by
 * entity, not by stored pointer, across mutations. Entity handles stay valid
 * until the entity is destroyed; a destroyed-and-recycled slot yields a new
 * version, so old handles report as not alive.
 */

#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>

#include "foundation/containers/hash_map.h"
#include "foundation/containers/small_vector.h"
#include "foundation/containers/sparse_set.h"
#include "foundation/core/callable.h"
#include "foundation/core/foundation.h"
#include "foundation/core/handle.h"
#include "foundation/core/hash.h"
#include "foundation/strings/utf8_string_view.h"

#include <cassert>

namespace nx::ecs {

template <typename Entity, typename Alloc> class basic_registry;

/**
 * @brief Policy chosen when destroying an entity that has children.
 */
enum class destroy_policy : u8 {
  /// Destroy the entity and recursively destroy its whole subtree.
  destroy_descendants,
  /// Destroy the entity; detach its direct children so they become roots.
  detach_children,
  /// Destroy the entity; reattach its direct children to its former parent.
  reparent_to_grandparent,
};

/**
 * @brief Bit flags recorded in a component's metadata.
 */
enum class component_flags : u32 {
  none = 0,
  /// Type has no per-instance state (tag); recorded for tooling.
  empty = 1u << 0,
  /// Type cannot be copied; recorded for tooling and serialization layers.
  move_only = 1u << 1,
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

// Process-wide sequential component ids. The id of a type is stable for the
// lifetime of the process and is used as the dense storage slot for that type.
// Ids are shared across all registries; a registry that does not use a given
// type simply leaves a hole in its storage table.
using component_id = u32;

inline component_id next_component_id() noexcept {
  static component_id counter = 0;
  return counter++;
}

template <typename T> [[nodiscard]] component_id component_id_of() noexcept {
  static const component_id id = next_component_id();
  return id;
}

// Type-erased lifecycle operations derived from a component type. With typed
// storage these are not used to run the lifetime (the sparse_set already calls
// the real ctor/dtor), but they are recorded in metadata so reflection and
// serialization layers built on top of the registry can manipulate components
// generically. A null pointer means the operation is not available for T.
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

} // namespace detail

/**
 * @brief Immutable description supplied when registering a component type.
 *
 * Designated-initializer friendly: only fields that matter need to be set.
 *
 * @code
 * registry.register_component<Transform>({ .name = "Transform" });
 * @endcode
 */
struct component_desc {
  /// Human-readable type name for debugging and statistics (not owned).
  string_view name = {};
  /// Additional flags merged with the ones the registry derives from T.
  component_flags flags = component_flags::none;
};

/**
 * @brief Metadata stored by the registry for each registered component type.
 *
 * The registry owns this record for the registry's lifetime. The storage
 * pointer is owned by the registry; do not free it. Live counts and capacity
 * are not cached here; query them through statistics so they never go stale.
 */
struct component_metadata {
  detail::component_id id = 0;
  string_view name = {};
  u32 size = 0;
  u32 alignment = 0;
  component_flags flags = component_flags::none;
  detail::component_lifecycle lifecycle{};
  /// Opaque pointer to the type-erased storage base; owned by the registry.
  void *storage = nullptr;
};

namespace detail {

// Type-erased base for a component storage. It exposes only the operations the
// registry needs without knowing T, so heterogeneous storages can live in one
// table and components can be removed when an entity is destroyed. Hooks are
// fired here (on_destroy) so they run on the type-erased remove path too.
template <typename Entity, typename Alloc> class storage_base {
public:
  using registry_type = basic_registry<Entity, Alloc>;

  virtual ~storage_base() = default;

  [[nodiscard]] virtual bool contains(Entity e) const noexcept = 0;
  // Remove e if present, firing on_destroy first. Returns true if removed.
  virtual bool remove(registry_type &reg, Entity e) = 0;
  virtual void clear(registry_type &reg) = 0;
  virtual void reserve(usize n) = 0;

  [[nodiscard]] virtual usize size() const noexcept = 0;
  [[nodiscard]] virtual usize capacity() const noexcept = 0;
  [[nodiscard]] virtual usize element_size() const noexcept = 0;
  [[nodiscard]] virtual std::span<const Entity> entities() const noexcept = 0;

  // Destroy this object using the allocator it was constructed with.
  virtual void self_delete() noexcept = 0;
};

} // namespace detail

namespace detail {

// A "sink" is the list of user callbacks for one lifecycle event. Callbacks
// receive the registry and the affected entity, so they can look up component
// data themselves; this keeps the sink type independent of the component type.
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

// Typed component storage: wraps an nx::sparse_set<T, Entity> and owns the
// lifecycle hooks for T. Even empty (tag) types are stored through the set so
// that get<T>() can return a reference and views iterate uniformly.
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

  bool remove(registry_type &reg, Entity e) override {
    if (!m_set.contains(e))
      return false;
    if (!m_on_destroy.empty())
      m_on_destroy.fire(reg, e);
    return m_set.erase(e);
  }

  void clear(registry_type &reg) override {
    if (!m_on_destroy.empty()) {
      // Fire on_destroy for each element before tearing the storage down.
      for (Entity e : m_set.entities())
        m_on_destroy.fire(reg, e);
    }
    m_set.clear();
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
  NX_NO_UNIQUE_ADDRESS self_alloc m_alloc;
};

} // namespace detail

namespace detail {

// Tuple element: the typed storage pointer for component Ti, const-qualified
// when the view was created over a const registry.
template <typename Reg, typename T>
using view_storage_ptr = std::conditional_t<
    std::is_const_v<Reg>,
    const component_storage<std::remove_const_t<T>, typename Reg::entity_type,
                            typename Reg::allocator_type> *,
    component_storage<T, typename Reg::entity_type,
                      typename Reg::allocator_type> *>;

} // namespace detail

/**
 * @brief Transient, non-owning view over entities sharing components T... .
 *
 * Created by registry::view<T...>(). It holds pointers into the registry's
 * storages and iterates the smallest matching storage, checking the others per
 * entity. A view is cheap to construct and is meant to be used immediately;
 * exclude<U...>() narrows it to entities that have none of U... . Do not add or
 * remove components of any T... (or excluded U...) on the matched entities
 * while iterating.
 *
 * @tparam Reg Registry type, possibly const-qualified.
 * @tparam T   Component types every matched entity must have.
 */
template <typename Reg, typename... T> class basic_view {
public:
  using entity_type = typename Reg::entity_type;
  using allocator_type = typename Reg::allocator_type;
  using base_ptr = const detail::storage_base<entity_type, allocator_type> *;

  basic_view(std::tuple<detail::view_storage_ptr<Reg, T>...> storages,
             const allocator_type &alloc)
      : m_storages(storages), m_excludes(exclude_alloc(alloc)) {}

  /// Restrict to entities that have none of the components U... .
  template <typename... U> basic_view &exclude(Reg &reg) {
    (m_excludes.push_back(reg.template raw_storage<U>()), ...);
    return *this;
  }

  /// Number of entities in the leading (smallest) storage. Upper bound only.
  [[nodiscard]] usize size_hint() const noexcept {
    base_ptr lead = leading();
    return lead ? lead->size() : 0;
  }

  [[nodiscard]] bool empty() const noexcept { return size_hint() == 0; }

  /**
   * @brief Invoke fn(entity, T&...) for every matched entity.
   *
   * Iterates the smallest storage and skips entities missing any T or present
   * in any excluded storage.
   */
  template <typename Fn> void each(Fn &&fn) const {
    if (!all_present())
      return;
    base_ptr lead = leading();
    if (!lead)
      return;
    for (entity_type e : lead->entities()) {
      if (!matches(e))
        continue;
      invoke(std::forward<Fn>(fn), e, std::index_sequence_for<T...>{});
    }
  }

private:
  using exclude_alloc = typename std::allocator_traits<
      allocator_type>::template rebind_alloc<base_ptr>;

  [[nodiscard]] bool all_present() const noexcept {
    return std::apply([](auto... p) { return ((p != nullptr) && ...); },
                      m_storages);
  }

  [[nodiscard]] base_ptr leading() const noexcept {
    base_ptr best = nullptr;
    usize best_size = ~usize(0);
    std::apply(
        [&](auto... p) {
          (([&] {
             const base_ptr b = p;
             if (b && b->size() < best_size) {
               best_size = b->size();
               best = b;
             }
           }()),
           ...);
        },
        m_storages);
    return best;
  }

  [[nodiscard]] bool matches(entity_type e) const noexcept {
    const bool has_all = std::apply(
        [&](auto... p) { return (p->contains(e) && ...); }, m_storages);
    if (!has_all)
      return false;
    for (base_ptr ex : m_excludes)
      if (ex && ex->contains(e))
        return false;
    return true;
  }

  template <typename Fn, usize... I>
  void invoke(Fn &&fn, entity_type e, std::index_sequence<I...>) const {
    fn(e, std::get<I>(m_storages)->get(e)...);
  }

  std::tuple<detail::view_storage_ptr<Reg, T>...> m_storages;
  small_vector<base_ptr, 4, exclude_alloc> m_excludes;
};

namespace detail {

// Per-cached-query state owned by the registry. The match list is rebuilt
// lazily when any tracked component type changes generation (see registry
// change counters). Hierarchy changes do not affect component generations and
// so do not invalidate cached queries.
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

} // namespace detail

template <typename Reg, typename... T> class cached_query;

/**
 * @brief Snapshot of registry-wide counts; cheap to produce.
 */
struct registry_statistics {
  usize entity_count = 0;            ///< Alive entities.
  usize entity_slot_count = 0;       ///< Allocated entity slots.
  usize free_slot_count = 0;         ///< Recyclable (dead) slots.
  usize component_type_count = 0;    ///< Registered component types.
  usize component_storage_count = 0; ///< Non-null component storages.
  usize relation_count = 0;          ///< Parent links currently set.
  usize cached_query_count = 0;      ///< Live cached queries.
  usize approx_memory_bytes = 0;     ///< Rough heap estimate; see docs.
};

/**
 * @brief Per-type statistics; cheap to produce.
 */
struct component_type_statistics {
  detail::component_id id = 0;
  string_view name = {};
  usize size = 0;
  usize alignment = 0;
  usize count = 0;    ///< Live components of this type.
  usize capacity = 0; ///< Storage capacity for this type.
  component_flags flags = component_flags::none;
};

/**
 * @brief Allocator-aware entity-component-system registry.
 *
 * Owns entities, their components, and a parent-child hierarchy. Component
 * types must be registered with register_component<T>() before they are used.
 *
 * The default entity handle (handle32) packs 24 index bits and 8 version bits;
 * handle64 packs 32 and 32. Choose the handle and allocator through the
 * template parameters:
 * @code
 * using registry_t = nx::ecs::basic_registry<nx::handle64,
 * nx::Allocator<std::byte>>;
 * @endcode
 *
 * @tparam Entity Versioned handle type used as the entity id.
 * @tparam Alloc  Allocator; rebound internally for every dynamic allocation.
 */
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

  // --- Entities -----------------------------------------------------------

  /// Create a fresh entity, reusing a dead slot when one is available.
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

  /// True if the handle refers to a currently live entity (version-checked).
  [[nodiscard]] bool alive(Entity e) const noexcept {
    if (!e.valid())
      return false;
    const repr idx = e.index();
    return idx < m_slots.size() && m_slots[idx].live &&
           m_slots[idx].version == e.version();
  }

  /**
   * @brief Destroy an entity, all its components, and resolve its children.
   *
   * Stale or null handles are ignored. The hierarchy is resolved using
   * @p policy (default: detach children so they become roots).
   */
  void destroy(Entity e,
               destroy_policy policy = destroy_policy::detach_children) {
    if (!alive(e))
      return;
    resolve_children_on_destroy(e, policy);
    detach_from_parent(e);
    remove_all_components(e);
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

  /// Remove every entity, component, and relation; storages are kept
  /// registered.
  void clear() {
    for (base_storage *s : m_storages)
      if (s)
        s->clear(*this);
    for (u64 &g : m_generations)
      ++g;
    m_hierarchy.clear();
    m_relation_count = 0;
    m_slots.clear();
    m_free.clear();
    m_alive_count = 0;
    for (qcache *q : m_query_caches)
      if (q)
        q->valid = false;
  }

  /// Reserve capacity for an expected number of entity slots.
  void reserve(size_type n) {
    m_slots.reserve(n);
    m_free.reserve(n);
  }

  [[nodiscard]] size_type size() const noexcept { return m_alive_count; }
  [[nodiscard]] bool empty() const noexcept { return m_alive_count == 0; }

  // --- Component registration --------------------------------------------

  /**
   * @brief Register a component type so it can be added to entities.
   *
   * Must be called once per type before any emplace/get/view/query for it.
   * Registering the same type twice is a usage error (asserts in debug) and
   * leaves the first registration in place. The registry owns the resulting
   * storage and metadata for its lifetime.
   *
   * @return Reference to the stored metadata for T.
   */
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

    component_metadata md{};
    md.id = id;
    md.name = desc.name;
    md.size = nx::cast<u32>(sizeof(T));
    md.alignment = nx::cast<u32>(alignof(T));
    md.flags = flags;
    md.lifecycle = detail::make_lifecycle<T>();
    md.storage = store;
    m_metadata[id] = md;
    ++m_registered_count;
    return m_metadata[id];
  }

  /// True if T has been registered.
  template <typename C>
  [[nodiscard]] bool is_component_registered() const noexcept {
    const detail::component_id id =
        detail::component_id_of<std::remove_cv_t<C>>();
    return id < m_storages.size() && m_storages[id] != nullptr;
  }

  /// Metadata for a registered T. Precondition: is_component_registered<T>().
  template <typename C>
  [[nodiscard]] const component_metadata &
  component_metadata_of() const noexcept {
    const detail::component_id id =
        detail::component_id_of<std::remove_cv_t<C>>();
    assert(id < m_storages.size() && m_storages[id] != nullptr &&
           "registry: component type not registered");
    return m_metadata[id];
  }

  // --- Components ---------------------------------------------------------

  /// Construct a T in place on @p e. Precondition: e alive, T registered, e
  /// has no T yet. Fires on_construct hooks. Returns the new component.
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

  /// Alias for emplace_or_replace.
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

  /// Overwrite an existing T. Precondition: e has T. Fires on_update hooks.
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

  /// Mutate an existing T through fn(T&), then fire on_update hooks.
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

  /// Get T (or a tuple of components for multiple types). Precondition: each
  /// requested type is registered and present on @p e.
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

  /// Pointer to T on @p e, or nullptr if T is unregistered or absent. Safe for
  /// stale handles.
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

  /// True if @p e has component T (false if T unregistered or e stale).
  template <typename C> [[nodiscard]] bool has(Entity e) const noexcept {
    const auto *store = raw_storage<std::remove_cv_t<C>>();
    return store && store->contains(e);
  }
  /// True if @p e has all of C... .
  template <typename... C> [[nodiscard]] bool all_of(Entity e) const noexcept {
    return (has<C>(e) && ...);
  }
  /// True if @p e has at least one of C... .
  template <typename... C> [[nodiscard]] bool any_of(Entity e) const noexcept {
    return (has<C>(e) || ...);
  }

  /// Remove T from @p e if present. Fires on_destroy hooks. Returns true if a
  /// component was removed. Safe for stale handles and unregistered types.
  template <typename C> bool remove(Entity e) {
    using T = std::remove_cv_t<C>;
    auto *store = raw_storage<T>();
    if (!store)
      return false;
    if (store->remove(*this, e)) {
      bump_generation(detail::component_id_of<T>());
      return true;
    }
    return false;
  }

  /// Remove T from @p e. Precondition: e has T. Fires on_destroy hooks.
  template <typename C> void erase(Entity e) {
    const bool removed = remove<C>(e);
    assert(removed && "registry::erase: entity does not have component");
    (void)removed;
  }

  /// Remove every component from @p e. Fires on_destroy hooks.
  void remove_all(Entity e) { remove_all_components(e); }

  /// Remove all components of type T. Fires on_destroy hooks per element.
  template <typename C> void clear() {
    using T = std::remove_cv_t<C>;
    auto *store = raw_storage<T>();
    if (!store)
      return;
    store->clear(*this);
    bump_generation(detail::component_id_of<T>());
  }

  /// Reorder T's dense storage by comp(const T&, const T&). Views and queries
  /// take their iteration order from the smallest storage where possible.
  template <typename C, typename Compare> void sort(Compare comp) {
    using T = std::remove_cv_t<C>;
    auto &store = assure<T>();
    store.set().sort(std::move(comp));
  }

  /// Pointer to the typed storage for T, or nullptr if T is unregistered.
  /// Low-level access for systems that iterate a single component densely.
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

  // --- Hooks --------------------------------------------------------------

  /// Append a callback fired after a T is constructed on an entity.
  template <typename C, typename Fn> void on_construct(Fn &&fn) {
    assure<std::remove_cv_t<C>>().m_on_construct.callbacks.emplace_back(
        std::forward<Fn>(fn));
  }
  /// Append a callback fired before a T is removed from an entity.
  template <typename C, typename Fn> void on_destroy(Fn &&fn) {
    assure<std::remove_cv_t<C>>().m_on_destroy.callbacks.emplace_back(
        std::forward<Fn>(fn));
  }
  /// Append a callback fired after a T is replaced or patched.
  template <typename C, typename Fn> void on_update(Fn &&fn) {
    assure<std::remove_cv_t<C>>().m_on_update.callbacks.emplace_back(
        std::forward<Fn>(fn));
  }

  // --- Views and queries --------------------------------------------------

  /// Transient view over entities having all of T... . See nx::ecs::basic_view.
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

  /// Cached query over entities having all of T...; reused across calls and
  /// rebuilt lazily when a tracked component type changes. See
  /// nx::ecs::cached_query.
  template <typename... C> [[nodiscard]] auto query() {
    (assert_registered<std::remove_cv_t<C>>(), ...);
    qcache &c = assure_query<std::remove_cv_t<C>...>();
    return cached_query<basic_registry, std::remove_cv_t<C>...>(this, &c);
  }

  // --- Hierarchy ----------------------------------------------------------

  /**
   * @brief Make @p parent the parent of @p child.
   *
   * Rejects (returns false) null/stale handles, self-parenting, and any link
   * that would create a cycle. An existing parent of @p child is replaced.
   */
  bool set_parent(Entity child, Entity parent) {
    if (!alive(child) || !alive(parent))
      return false;
    if (child == parent)
      return false;
    if (is_descendant_of(parent, child))
      return false;

    detach_from_parent(child);

    // Materialize both nodes first; emplace may reallocate the dense array,
    // so references are taken only after no further insertion can happen.
    ensure_node(child);
    ensure_node(parent);
    hierarchy_node &cn = m_hierarchy.at(child);
    hierarchy_node &pn = m_hierarchy.at(parent);
    cn.parent = parent;
    cn.prev_sibling = Entity{};
    cn.next_sibling = pn.first_child;
    if (pn.first_child.valid())
      m_hierarchy.at(pn.first_child).prev_sibling = child;
    pn.first_child = child;
    ++pn.child_count;
    ++m_relation_count;
    ++m_hierarchy_version;
    return true;
  }

  /// Detach @p child from its parent (becomes a root). No-op if it had none.
  void remove_parent(Entity child) {
    if (!m_hierarchy.contains(child))
      return;
    detach_from_parent(child);
    maybe_erase_node(child);
  }

  /// Parent of @p e, or null_entity if it has none.
  [[nodiscard]] Entity parent(Entity e) const noexcept {
    const hierarchy_node *n = m_hierarchy.try_get(e);
    return n ? n->parent : Entity{};
  }

  /// True if @p e has a parent.
  [[nodiscard]] bool has_parent(Entity e) const noexcept {
    return parent(e).valid();
  }

  /// Number of direct children of @p e.
  [[nodiscard]] usize child_count(Entity e) const noexcept {
    const hierarchy_node *n = m_hierarchy.try_get(e);
    return n ? n->child_count : 0;
  }

  /// Invoke fn(child) for each direct child of @p e. Safe if fn destroys the
  /// child currently being visited.
  template <typename Fn> void each_child(Entity e, Fn &&fn) const {
    const hierarchy_node *n = m_hierarchy.try_get(e);
    if (!n)
      return;
    Entity c = n->first_child;
    while (c.valid()) {
      const Entity next = sibling_next(c);
      fn(c);
      c = next;
    }
  }

  /// Depth-first pre-order traversal invoking fn(descendant) for the whole
  /// subtree below @p e (excluding @p e itself).
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

  /// True if @p maybe_descendant lies anywhere below @p ancestor.
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

  // --- Statistics ---------------------------------------------------------

  /// Registry-wide counts. Cheap to compute.
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

  /// Per-type statistics for registered T. Precondition: T registered.
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

  // Current change generation of a component type; bumped on add/remove. Used
  // by cached queries to decide when to rebuild.
  [[nodiscard]] u64
  component_generation(detail::component_id id) const noexcept {
    return id < m_generations.size() ? m_generations[id] : 0;
  }

  // Rebuild a cached query's entity list from scratch. Public so cached_query
  // can drive it; not intended for direct user calls.
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
      if (s && s->contains(e)) {
        s->remove(*this, e);
        ++m_generations[id];
      }
    }
  }

  // --- hierarchy helpers ---

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

    cn->parent = Entity{};
    cn->prev_sibling = Entity{};
    cn->next_sibling = Entity{};
    if (pn.child_count > 0)
      --pn.child_count;
    --m_relation_count;
    ++m_hierarchy_version;
    maybe_erase_node(p);
  }

  [[nodiscard]] Entity sibling_next(Entity e) const noexcept {
    const hierarchy_node *n = m_hierarchy.try_get(e);
    return n ? n->next_sibling : Entity{};
  }

  template <usize N, typename A>
  void push_children(Entity e, small_vector<Entity, N, A> &stack) const {
    const hierarchy_node *n = m_hierarchy.try_get(e);
    if (!n)
      return;
    Entity c = n->first_child;
    while (c.valid()) {
      stack.push_back(c);
      c = sibling_next(c);
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
      c = sibling_next(c);
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

  // --- query cache helpers ---

  template <typename... T> qcache &assure_query() {
    detail::component_id ids[] = {detail::component_id_of<T>()...};
    const usize n = sizeof...(T);
    // Insertion sort the small id list so the key is order-independent.
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

/// Default registry: 32-bit entity handles, default nx allocator.
using registry = basic_registry<handle32, Allocator<std::byte>>;
/// Convenience registry over 64-bit entity handles.
using registry64 = basic_registry<handle64, Allocator<std::byte>>;

/**
 * @brief Cached query over entities sharing components T... .
 *
 * Obtained from registry::query<T...>(). Unlike a transient view, the registry
 * keeps the match list between calls and rebuilds it lazily: the first each()
 * after a tracked component type was added to or removed from any entity (or
 * after clear()) recomputes the set, otherwise the cached list is reused. This
 * makes it the right choice for a component set iterated every frame.
 *
 * @section cq_invalidation Invalidation rules
 * A rebuild is triggered when, for any T in the query, a component of that type
 * is added or removed, when an entity carrying such a component is destroyed,
 * or when the registry is cleared. Parent-child changes do not affect cached
 * queries. As with views, do not add or remove components of any T while a
 * query over T is iterating.
 *
 * @tparam Reg Registry type, possibly const-qualified.
 * @tparam T   Component types every matched entity has.
 */
template <typename Reg, typename... T> class cached_query {
public:
  using entity_type = typename Reg::entity_type;
  using allocator_type = typename Reg::allocator_type;
  using cache_type = detail::query_cache<entity_type, allocator_type>;

  cached_query(Reg *reg, cache_type *cache) noexcept
      : m_reg(reg), m_cache(cache) {}

  /// Number of matched entities (rebuilds the cache if stale).
  [[nodiscard]] usize size() {
    ensure_built();
    return m_cache->entities.size();
  }
  [[nodiscard]] bool empty() { return size() == 0; }

  /// Matched entities as a contiguous span (rebuilds the cache if stale).
  [[nodiscard]] std::span<const entity_type> entities() {
    ensure_built();
    return {m_cache->entities.data(), m_cache->entities.size()};
  }

  /// Invoke fn(entity, T&...) for each matched entity.
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

} // namespace nx::ecs