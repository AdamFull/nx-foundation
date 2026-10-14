#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/reflection/type.h"
#include "core/foundation/serialization/json.h"

namespace nx::json {

[[nodiscard]] inline f64 at_or(const value &array, const usize i,
                               const f64 fallback) noexcept {
  if (!array.is_array() || i >= array.size())
    return fallback;
  return array[i].get_double(fallback);
}

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
[[nodiscard]] V read_vec3(const value &v, const V &fallback) noexcept {
  return V{cast<f32>(at_or(v, 0, cast<f64>(fallback.x))),
           cast<f32>(at_or(v, 1, cast<f64>(fallback.y))),
           cast<f32>(at_or(v, 2, cast<f64>(fallback.z)))};
}

template <class V>
[[nodiscard]] V read_vec3(const value &owner, const string_view key,
                          const V &fallback) noexcept {
  return read_vec3(owner[key], fallback);
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

template <class V> [[nodiscard]] value write_vec3(const V &v) {
  value a = value::make_array();
  a.push_back(value(cast<f64>(v.x)));
  a.push_back(value(cast<f64>(v.y)));
  a.push_back(value(cast<f64>(v.z)));
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

[[nodiscard]] inline bool version_ok(const value &root,
                                     const u64 current) noexcept {
  const value &v = root["version"];
  return !v.is_number() || v.get_u64(current) <= current;
}

template <class E>
concept spelled_enum = !reflect::enum_spellings<E>::entries.empty();

template <spelled_enum E>
bool parse_enum(const string_view text, E &out) noexcept {
  const auto &spellings = *reflect::type_of<E>().enumeration;
  i64 value = 0;
  if (!spellings.value(text, value))
    return false;
  out = static_cast<E>(value);
  return true;
}

template <spelled_enum E>
[[nodiscard]] string_view enum_text(const E value) noexcept {
  return reflect::type_of<E>().enumeration->text(static_cast<i64>(value));
}

}
