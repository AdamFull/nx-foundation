#pragma once

#include "core/foundation/core/enum.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/reflection/aggregate.h"
#include "core/foundation/strings/utf8_string_view.h"

#include <array>
#include <span>

/// Editor metadata is carried in dev builds and compiled out of a shipping one.
/// It has to be the same for the whole build: field_attributes would otherwise
/// have two layouts in one program.
#ifndef NX_REFLECT_EDITOR
#define NX_REFLECT_EDITOR 1
#endif

#if NX_REFLECT_EDITOR
#define NX_EDITOR_ONLY(...) __VA_ARGS__
#else
#define NX_EDITOR_ONLY(...)
#endif

namespace nx::reflect {

enum class field_flag : u16 {
  none = 0,
  transient = 1u << 0,
  read_only = 1u << 1,
  hidden = 1u << 2,
};
NX_ENUM_FLAGS_DEFINE(field_flag, field_flags)

struct field_attributes {
  field_flags flags{};
  /// The name this field has in a document, when that differs from the name it
  /// has in the type. `SamplerSettings::max_anisotropy` is written
  /// "anisotropy"; renaming the field must not rename the key.
  string_view key{};
  f64 min = 0.0;
  f64 max = 0.0;
#if NX_REFLECT_EDITOR
  string_view label{};
#endif
};

struct enum_spelling {
  i64 value;
  string_view name;
};

template <class E> struct enum_spellings {
  static constexpr std::span<const enum_spelling> entries{};
};

struct field_attribute_entry {
  string_view field;
  field_attributes attributes;
};

template <class T> struct attributes_of {
  static constexpr std::span<const field_attribute_entry> entries{};
};

namespace detail {

template <plain_aggregate T>
[[nodiscard]] consteval bool has_field_named(const string_view name) {
  return [&]<usize... I>(std::index_sequence<I...>) {
    return ((field_name<T, I>() == name) || ...);
  }(std::make_index_sequence<field_count<T>>{});
}

/// Every attributed name has to be a field. A rename would otherwise drop its
/// attributes without a word.
template <plain_aggregate T> [[nodiscard]] consteval bool attributes_match() {
  for (const field_attribute_entry &e : attributes_of<T>::entries)
    if (!has_field_named<T>(e.field))
      return false;
  return true;
}

template <class T>
[[nodiscard]] constexpr field_attributes
attributes_for(const string_view name) {
  for (const field_attribute_entry &e : attributes_of<T>::entries)
    if (e.field == name)
      return e.attributes;
  return {};
}

}

}

#define NX_RF_UNWRAP(...) __VA_ARGS__

#define NX_RF_FE_1(M, x) M(x)
#define NX_RF_FE_2(M, x, ...) M(x), NX_RF_FE_1(M, __VA_ARGS__)
#define NX_RF_FE_3(M, x, ...) M(x), NX_RF_FE_2(M, __VA_ARGS__)
#define NX_RF_FE_4(M, x, ...) M(x), NX_RF_FE_3(M, __VA_ARGS__)
#define NX_RF_FE_5(M, x, ...) M(x), NX_RF_FE_4(M, __VA_ARGS__)
#define NX_RF_FE_6(M, x, ...) M(x), NX_RF_FE_5(M, __VA_ARGS__)
#define NX_RF_FE_7(M, x, ...) M(x), NX_RF_FE_6(M, __VA_ARGS__)
#define NX_RF_FE_8(M, x, ...) M(x), NX_RF_FE_7(M, __VA_ARGS__)
#define NX_RF_FE_9(M, x, ...) M(x), NX_RF_FE_8(M, __VA_ARGS__)
#define NX_RF_FE_10(M, x, ...) M(x), NX_RF_FE_9(M, __VA_ARGS__)
#define NX_RF_FE_11(M, x, ...) M(x), NX_RF_FE_10(M, __VA_ARGS__)
#define NX_RF_FE_12(M, x, ...) M(x), NX_RF_FE_11(M, __VA_ARGS__)
#define NX_RF_FE_13(M, x, ...) M(x), NX_RF_FE_12(M, __VA_ARGS__)
#define NX_RF_FE_14(M, x, ...) M(x), NX_RF_FE_13(M, __VA_ARGS__)
#define NX_RF_FE_15(M, x, ...) M(x), NX_RF_FE_14(M, __VA_ARGS__)
#define NX_RF_FE_16(M, x, ...) M(x), NX_RF_FE_15(M, __VA_ARGS__)
#define NX_RF_FE_17(M, x, ...) M(x), NX_RF_FE_16(M, __VA_ARGS__)
#define NX_RF_FE_18(M, x, ...) M(x), NX_RF_FE_17(M, __VA_ARGS__)
#define NX_RF_FE_19(M, x, ...) M(x), NX_RF_FE_18(M, __VA_ARGS__)
#define NX_RF_FE_20(M, x, ...) M(x), NX_RF_FE_19(M, __VA_ARGS__)
#define NX_RF_FE_21(M, x, ...) M(x), NX_RF_FE_20(M, __VA_ARGS__)
#define NX_RF_FE_22(M, x, ...) M(x), NX_RF_FE_21(M, __VA_ARGS__)
#define NX_RF_FE_23(M, x, ...) M(x), NX_RF_FE_22(M, __VA_ARGS__)
#define NX_RF_FE_24(M, x, ...) M(x), NX_RF_FE_23(M, __VA_ARGS__)
#define NX_RF_FE_25(M, x, ...) M(x), NX_RF_FE_24(M, __VA_ARGS__)
#define NX_RF_FE_26(M, x, ...) M(x), NX_RF_FE_25(M, __VA_ARGS__)
#define NX_RF_FE_27(M, x, ...) M(x), NX_RF_FE_26(M, __VA_ARGS__)
#define NX_RF_FE_28(M, x, ...) M(x), NX_RF_FE_27(M, __VA_ARGS__)
#define NX_RF_FE_29(M, x, ...) M(x), NX_RF_FE_28(M, __VA_ARGS__)
#define NX_RF_FE_30(M, x, ...) M(x), NX_RF_FE_29(M, __VA_ARGS__)
#define NX_RF_FE_31(M, x, ...) M(x), NX_RF_FE_30(M, __VA_ARGS__)
#define NX_RF_FE_32(M, x, ...) M(x), NX_RF_FE_31(M, __VA_ARGS__)

#define NX_RF_PICK(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13,     \
                   _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, \
                   _26, _27, _28, _29, _30, _31, _32, NAME, ...)               \
  NAME

#define NX_RF_LIST(M, ...)                                                     \
  NX_RF_PICK(__VA_ARGS__, NX_RF_FE_32, NX_RF_FE_31, NX_RF_FE_30, NX_RF_FE_29,  \
             NX_RF_FE_28, NX_RF_FE_27, NX_RF_FE_26, NX_RF_FE_25, NX_RF_FE_24,  \
             NX_RF_FE_23, NX_RF_FE_22, NX_RF_FE_21, NX_RF_FE_20, NX_RF_FE_19,  \
             NX_RF_FE_18, NX_RF_FE_17, NX_RF_FE_16, NX_RF_FE_15, NX_RF_FE_14,  \
             NX_RF_FE_13, NX_RF_FE_12, NX_RF_FE_11, NX_RF_FE_10, NX_RF_FE_9,   \
             NX_RF_FE_8, NX_RF_FE_7, NX_RF_FE_6, NX_RF_FE_5, NX_RF_FE_4,       \
             NX_RF_FE_3, NX_RF_FE_2, NX_RF_FE_1, 0)                            \
  (M, __VA_ARGS__)

#define NX_ATTR_ONE(item) NX_ATTR_ONE_(NX_RF_UNWRAP item)
#define NX_ATTR_ONE_(...) NX_ATTR_MAKE(__VA_ARGS__)
#define NX_ATTR_MAKE(field, ...)                                               \
  ::nx::reflect::field_attribute_entry {                                       \
    #field, { __VA_ARGS__ }                                                    \
  }

#define NX_ATTRIBUTES(Type, ...)                                               \
  template <> struct nx::reflect::attributes_of<Type> {                        \
    static constexpr ::std::array table = {                                    \
        NX_RF_LIST(NX_ATTR_ONE, __VA_ARGS__)};                                 \
    static constexpr ::std::span<const ::nx::reflect::field_attribute_entry>   \
        entries{table};                                                        \
  };                                                                           \
  static_assert(::nx::reflect::detail::attributes_match<Type>(),               \
                "nx::reflect: an attribute names a field this type does not "  \
                "have - was it renamed?");

#define NX_ENUM_ONE(item) NX_ENUM_ONE_(NX_RF_UNWRAP item)
#define NX_ENUM_ONE_(...) NX_ENUM_MAKE(__VA_ARGS__)
#define NX_ENUM_MAKE(enumerator, text)                                         \
  ::nx::reflect::enum_spelling { static_cast<i64>(Enum_::enumerator), text }

#define NX_REFLECT_ENUM(Enum, ...)                                             \
  template <> struct nx::reflect::enum_spellings<Enum> {                       \
    using Enum_ = Enum;                                                        \
    static constexpr ::std::array table = {                                    \
        NX_RF_LIST(NX_ENUM_ONE, __VA_ARGS__)};                                 \
    static constexpr ::std::span<const ::nx::reflect::enum_spelling> entries{  \
        table};                                                                \
  };
