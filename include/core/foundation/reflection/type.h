#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/core/hash.h"
#include "core/foundation/reflection/aggregate.h"
#include "core/foundation/reflection/attributes.h"
#include "core/foundation/strings/id_string.h"
#include "core/foundation/strings/utf8_string_view.h"

#include <array>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace nx::reflect {

/// Whether @p bits, a 64-bit integer as reflection writes one (sign-extended
/// when @p is_signed), fits a field @p size bytes wide.
[[nodiscard]] constexpr bool integer_fits(const u64 bits, const u32 size,
                                          const bool is_signed) noexcept {
  if (size >= sizeof(u64))
    return true;
  const u32 width = size * 8;
  if (!is_signed)
    return (bits >> width) == 0;
  const i64 v = static_cast<i64>(bits);
  const i64 low = -(i64{1} << (width - 1));
  return v >= low && v < -low;
}

using type_id = u64;

enum class type_kind : u8 {
  /// Described only by name and size. A handle, an opaque id, anything a
  /// generic walker must leave to the code that owns it.
  opaque,
  boolean,
  integer,
  unsigned_integer,
  floating,
  string,
  id,
  floats,
  enumeration,
  aggregate,
  sequence,
};

struct type_info;

struct field_info {
  string_view name;
  u32 name_hash = 0;
  u32 offset = 0;
  const type_info *type = nullptr;
  field_attributes attributes{};

  /// The name this field has in a document, which is its own unless something
  /// said otherwise.
  [[nodiscard]] constexpr string_view key() const noexcept {
    return attributes.key.empty() ? name : attributes.key;
  }

  [[nodiscard]] constexpr bool has(const field_flag f) const noexcept {
    return attributes.flags.has(f);
  }
  [[nodiscard]] constexpr bool serialised() const noexcept {
    return !has(field_flag::transient);
  }
};

struct enum_data {
  bool is_signed = false;
  std::span<const enum_spelling> entries;

  [[nodiscard]] constexpr string_view
  text(const i64 value, const string_view fallback = {}) const noexcept {
    for (const enum_spelling &e : entries)
      if (e.value == value)
        return e.name;
    return fallback;
  }

  [[nodiscard]] constexpr bool value(const string_view name,
                                     i64 &out) const noexcept {
    for (const enum_spelling &e : entries)
      if (e.name == name) {
        out = e.value;
        return true;
      }
    return false;
  }
};

struct sequence_data {
  const type_info *element = nullptr;
  /// A C array cannot grow: resize does nothing and size is its length.
  bool fixed = false;
  usize (*size)(const void *) = nullptr;
  void (*resize)(void *, usize) = nullptr;
  void *(*at)(void *, usize) = nullptr;
  const void *(*const_at)(const void *, usize) = nullptr;
};

struct type_ops {
  void (*default_construct)(void *) = nullptr;
  void (*destroy)(void *) = nullptr;
  void (*copy_assign)(void *dst, const void *src) = nullptr;
};

struct type_info {
  string_view name;
  type_id id = 0;
  u32 size = 0;
  u32 align = 0;
  type_kind kind = type_kind::opaque;
  std::span<const field_info> fields;
  const enum_data *enumeration = nullptr;
  const sequence_data *sequence = nullptr;
  const type_ops *ops = nullptr;
  const void *defaults = nullptr;

  [[nodiscard]] constexpr u32 components() const noexcept {
    return size / cast<u32>(sizeof(f32));
  }

  [[nodiscard]] constexpr const field_info *
  field(const string_view field_name) const noexcept {
    for (const field_info &f : fields)
      if (f.name == field_name)
        return &f;
    return nullptr;
  }
};

template <class T>
concept float_component = std::is_same_v<std::remove_cvref_t<T>, f32>;

template <class T>
concept vec2_like =
    std::is_class_v<T> && sizeof(T) == 2 * sizeof(f32) && requires(T v) {
      requires float_component<decltype(v.x)>;
      requires float_component<decltype(v.y)>;
    };

template <class T>
concept vec3_like =
    std::is_class_v<T> && sizeof(T) == 3 * sizeof(f32) && requires(T v) {
      requires float_component<decltype(v.x)>;
      requires float_component<decltype(v.y)>;
      requires float_component<decltype(v.z)>;
    };

template <class T>
concept vec4_like =
    std::is_class_v<T> && sizeof(T) == 4 * sizeof(f32) && requires(T v) {
      requires float_component<decltype(v.x)>;
      requires float_component<decltype(v.y)>;
      requires float_component<decltype(v.z)>;
      requires float_component<decltype(v.w)>;
    };

template <class T>
concept float_vector = vec2_like<T> || vec3_like<T> || vec4_like<T>;

template <class T>
concept string_like = std::is_convertible_v<const T &, string_view> &&
                      requires(T s, usize n) { s.resize(n); };

template <class T>
concept resizable_sequence =
    !string_like<T> && requires(T c, const T cc, usize n) {
      typename T::value_type;
      { cc.size() } -> std::convertible_to<usize>;
      c.resize(n);
      { c[n] } -> std::same_as<typename T::value_type &>;
    };

template <class T>
concept sequence_like = resizable_sequence<T> || std::is_array_v<T>;

template <class T> const type_info &type_of();

namespace detail {

/// A string reaching this before a sequence is *not* what keeps it a string - `resizable_sequence`
/// excludes one - so do not rely on the position of that one.
template <class T> constexpr type_kind kind_of() {
  if constexpr (std::is_same_v<T, bool>)
    return type_kind::boolean;
  else if constexpr (std::is_enum_v<T>)
    return type_kind::enumeration;
  else if constexpr (std::is_same_v<T, id_string>)
    return type_kind::id;
  else if constexpr (float_vector<T>)
    return type_kind::floats;
  else if constexpr (std::is_floating_point_v<T>)
    return type_kind::floating;
  else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>)
    return type_kind::unsigned_integer;
  else if constexpr (std::is_integral_v<T>)
    return type_kind::integer;
  else if constexpr (string_like<T>)
    return type_kind::string;
  else if constexpr (sequence_like<T>)
    return type_kind::sequence;
  else if constexpr (plain_aggregate<T>)
    return type_kind::aggregate;
  else
    return type_kind::opaque;
}

template <class T> const T &defaults_of() {
  static const T value{};
  return value;
}

template <class T> const type_ops &ops_of() {
  static const type_ops ops = [] {
    type_ops o{};
    if constexpr (std::is_default_constructible_v<T>) {
      o.default_construct = [](void *p) { ::new (p) T(); };
      o.destroy = [](void *p) { std::destroy_at(static_cast<T *>(p)); };
    }
    if constexpr (std::is_copy_assignable_v<T>)
      o.copy_assign = [](void *dst, const void *src) {
        *static_cast<T *>(dst) = *static_cast<const T *>(src);
      };
    return o;
  }();
  return ops;
}

template <plain_aggregate T> const auto &fields_of() {
  static const auto built = [] {
    constexpr usize n = field_count<T>;
    std::array<field_info, n> out{};
    if constexpr (n > 0) {
      const T &probe = defaults_of<T>();
      const auto *base = reinterpret_cast<const std::byte *>(&probe);
      tie(probe, [&](const auto &...fields) {
        const std::byte *address[] = {
            reinterpret_cast<const std::byte *>(&fields)...};
        const type_info *types[] = {
            &type_of<std::remove_cvref_t<decltype(fields)>>()...};
        for (usize i = 0; i < n; ++i) {
          out[i].offset = cast<u32>(static_cast<usize>(address[i] - base));
          out[i].type = types[i];
        }
      });
      [&]<usize... I>(std::index_sequence<I...>) {
        ((out[I].name = field_name<T, I>(),
          out[I].name_hash =
              hash_fnv1a32(out[I].name.data(), out[I].name.size()),
          out[I].attributes = attributes_for<T>(out[I].name)),
         ...);
      }(std::make_index_sequence<n>{});
    }
    return out;
  }();
  return built;
}

template <class T> const sequence_data &sequence_of() {
  static const sequence_data data = [] {
    if constexpr (std::is_array_v<T>) {
      using E = std::remove_extent_t<T>;
      return sequence_data{
          &type_of<E>(),
          true,
          [](const void *) -> usize { return std::extent_v<T>; },
          [](void *, usize) {},
          [](void *p, const usize i) -> void * {
            return static_cast<E *>(p) + i;
          },
          [](const void *p, const usize i) -> const void * {
            return static_cast<const E *>(p) + i;
          }};
    } else {
      using E = typename T::value_type;
      return sequence_data{
          &type_of<E>(),
          false,
          [](const void *p) -> usize {
            return static_cast<const T *>(p)->size();
          },
          [](void *p, const usize n) { static_cast<T *>(p)->resize(n); },
          [](void *p, const usize i) -> void * {
            return &(*static_cast<T *>(p))[i];
          },
          [](const void *p, const usize i) -> const void * {
            return &(*static_cast<const T *>(p))[i];
          }};
    }
  }();
  return data;
}

template <class E> const enum_data &enum_of();

}

namespace detail {

template <class E> const enum_data &enum_of() {
  static const enum_data data{std::is_signed_v<std::underlying_type_t<E>>,
                              enum_spellings<E>::entries};
  return data;
}

template <class T> const type_info &build() {
  static const type_info info = [] {
    using U = std::remove_cv_t<T>;
    constexpr type_kind kind = kind_of<U>();
    type_info t{};
    t.name = type_name<U>();
    t.id = hash_fnv1a64(t.name.data(), t.name.size());
    t.size = cast<u32>(sizeof(U));
    t.align = cast<u32>(alignof(U));
    t.kind = kind;
    t.ops = &ops_of<U>();
    if constexpr (std::is_default_constructible_v<U>)
      t.defaults = &defaults_of<U>();
    if constexpr (kind == type_kind::aggregate) {
      const auto &fields = fields_of<U>();
      t.fields = {fields.data(), fields.size()};
    } else if constexpr (kind == type_kind::enumeration) {
      t.enumeration = &enum_of<U>();
    } else if constexpr (kind == type_kind::sequence) {
      t.sequence = &sequence_of<U>();
    }
    return t;
  }();
  return info;
}

}

template <class T> const type_info &type_of() {
  return detail::build<std::remove_cv_t<T>>();
}

}
