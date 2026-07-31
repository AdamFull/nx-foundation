#pragma once

/**
 * @file json_util.h
 * @brief Lenient readers for hand-authored documents (namespace nx::json).
 *
 * json.h's serializer<T> is strict: a missing field or a wrong type is an
 * error, which is what a wire format wants. An asset a person edits wants the
 * opposite - state the two fields that differ from the defaults and leave the
 * rest out - so everything here takes a fallback and never fails.
 *
 * The vector helpers deduce their type from that fallback, so this stays
 * usable from any layer without foundation having to know a maths library.
 */

#include "foundation/core/foundation.h"
#include "foundation/serialization/json.h"

namespace nx::json {

/// Element @p i of @p array, or @p fallback when it is not an array or is too
/// short. value::operator[](usize) has a bounds precondition; this does not.
[[nodiscard]] inline f64 at_or(const value &array, const usize i,
                               const f64 fallback) noexcept {
  if (!array.is_array() || i >= array.size())
    return fallback;
  return array[i].get_double(fallback);
}

// owner[key] on a missing key is a shared null, so these are safe on any
// document - including one where owner is not an object at all.

[[nodiscard]] inline f32 f32_or(const value &owner, const string_view key,
                                const f32 fallback) noexcept {
  return cast<f32>(owner[key].get_double(cast<f64>(fallback)));
}

[[nodiscard]] inline u32 u32_or(const value &owner, const string_view key,
                                const u32 fallback) noexcept {
  return cast<u32>(owner[key].get_u64(fallback));
}

[[nodiscard]] inline i32 i32_or(const value &owner, const string_view key,
                                const i32 fallback) noexcept {
  return cast<i32>(owner[key].get_i64(fallback));
}

[[nodiscard]] inline string_view string_or(const value &owner,
                                           const string_view key,
                                           const string_view fallback) {
  return owner[key].get_string(fallback);
}

/// A two-component vector written as `[x, y]`. Each component falls back on its
/// own, so `[3]` reads as x = 3 with y left at the default.
///
/// The array itself, for a vector that is an element rather than a field - a
/// point in a polygon, say, where there is no key to name it by.
template <class V>
[[nodiscard]] V read_vec2(const value &v, const V &fallback) noexcept {
  return V{cast<f32>(at_or(v, 0, cast<f64>(fallback.x))),
           cast<f32>(at_or(v, 1, cast<f64>(fallback.y)))};
}

template <class V>
[[nodiscard]] V read_vec2(const value &owner, const string_view key,
                          const V &fallback) noexcept {
  return read_vec2(owner[key], fallback);
}

template <class V>
[[nodiscard]] V read_vec4(const value &v, const V &fallback) noexcept {
  return V{cast<f32>(at_or(v, 0, cast<f64>(fallback.x))),
           cast<f32>(at_or(v, 1, cast<f64>(fallback.y))),
           cast<f32>(at_or(v, 2, cast<f64>(fallback.z))),
           cast<f32>(at_or(v, 3, cast<f64>(fallback.w)))};
}

template <class V>
[[nodiscard]] V read_vec4(const value &owner, const string_view key,
                          const V &fallback) noexcept {
  return read_vec4(owner[key], fallback);
}

template <class V> [[nodiscard]] value write_vec2(const V &v) {
  value a = value::make_array();
  a.push_back(value(cast<f64>(v.x)));
  a.push_back(value(cast<f64>(v.y)));
  return a;
}

template <class V> [[nodiscard]] value write_vec4(const V &v) {
  value a = value::make_array();
  a.push_back(value(cast<f64>(v.x)));
  a.push_back(value(cast<f64>(v.y)));
  a.push_back(value(cast<f64>(v.z)));
  a.push_back(value(cast<f64>(v.w)));
  return a;
}

/// True when @p root declares no version, or one at or below @p current.
///
/// Absent is accepted on purpose: a hand-authored document should not need
/// boilerplate to load, and every writer stamps a version anyway. A *newer* one
/// is refused, which is the only thing the field is for - a reader that ignores
/// it loads half a document it does not understand and says nothing.
[[nodiscard]] inline bool version_ok(const value &root,
                                     const u64 current) noexcept {
  const value &v = root["version"];
  return !v.is_number() || v.get_u64(current) <= current;
}

/// One enum value and a spelling that names it in a document.
///
/// A table may name one value more than once; the first entry is the canonical
/// spelling, so aliases are something a format adds without changing what it
/// writes back out.
template <class E> struct enum_entry {
  E value;
  string_view name;
};

/// Looks @p text up in @p table. Leaves @p out alone and returns false when it
/// is not there, so the caller keeps its default and warns in its own words.
template <class E, usize N>
[[nodiscard]] bool parse_enum(const enum_entry<E> (&table)[N],
                              const string_view text, E &out) noexcept {
  for (const enum_entry<E> &entry : table) {
    if (entry.name == text) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

/// The spelling @p value is written as: the first entry naming it, or empty for
/// a value the table does not cover.
template <class E, usize N>
[[nodiscard]] string_view enum_text(const enum_entry<E> (&table)[N],
                                    const E value) noexcept {
  for (const enum_entry<E> &entry : table)
    if (entry.value == value)
      return entry.name;
  return {};
}

} // namespace nx::json
