#include "core/foundation/serialization/reflect_json.h"

#include "core/foundation/serialization/json_util.h"
#include "core/foundation/strings/utf8_string.h"

#include <cstring>

namespace nx::json {
namespace {

using reflect::field_info;
using reflect::type_info;
using reflect::type_kind;

[[nodiscard]] const std::byte *at(const void *p, const usize offset) noexcept {
  return static_cast<const std::byte *>(p) + offset;
}
[[nodiscard]] std::byte *at(void *p, const usize offset) noexcept {
  return static_cast<std::byte *>(p) + offset;
}

// Through memcpy rather than a cast: a field is a byte offset into a struct
// this code knows nothing about, so it is not reliably aligned for its type.

[[nodiscard]] i64 load_signed(const void *p, const u32 size) noexcept {
  u64 raw = 0;
  std::memcpy(&raw, p, size);
  if (size < sizeof(u64) && ((raw >> (size * 8 - 1)) & 1) != 0)
    return static_cast<i64>(raw | (~u64{0} << (size * 8)));
  return static_cast<i64>(raw);
}

[[nodiscard]] u64 load_unsigned(const void *p, const u32 size) noexcept {
  u64 raw = 0;
  std::memcpy(&raw, p, size);
  return raw;
}

void store_integer(void *p, const u32 size, const u64 value) noexcept {
  std::memcpy(p, &value, size);
}

[[nodiscard]] f64 load_real(const void *p, const u32 size) noexcept {
  if (size == sizeof(f32)) {
    f32 v;
    std::memcpy(&v, p, sizeof(f32));
    return v;
  }
  f64 v;
  std::memcpy(&v, p, sizeof(f64));
  return v;
}

void store_real(void *p, const u32 size, const f64 value) noexcept {
  if (size == sizeof(f32)) {
    const auto v = static_cast<f32>(value);
    std::memcpy(p, &v, sizeof(f32));
    return;
  }
  std::memcpy(p, &value, sizeof(f64));
}

[[nodiscard]] i64 load_enum(const type_info &t, const void *p) noexcept {
  return t.enumeration->is_signed ? load_signed(p, t.size)
                                  : static_cast<i64>(load_unsigned(p, t.size));
}

} // namespace

[[nodiscard]] static bool same_as(const type_info &t, const void *a,
                                  const void *b) {
  switch (t.kind) {
  case type_kind::string:
    return *static_cast<const string *>(a) == *static_cast<const string *>(b);
  case type_kind::id:
    return static_cast<const id_string *>(a)->id() ==
           static_cast<const id_string *>(b)->id();
  case type_kind::aggregate:
    for (const field_info &f : t.fields)
      if (!same_as(*f.type, at(a, f.offset), at(b, f.offset)))
        return false;
    return true;
  case type_kind::sequence: {
    const auto &seq = *t.sequence;
    const usize n = seq.size(a);
    if (n != seq.size(b))
      return false;
    for (usize i = 0; i < n; ++i)
      if (!same_as(*seq.element, seq.const_at(a, i), seq.const_at(b, i)))
        return false;
    return true;
  }
  case type_kind::opaque:
    return true;
  default:
    return std::memcmp(a, b, t.size) == 0;
  }
}

value write_value(const type_info &t, const void *p, const write_mode mode) {
  switch (t.kind) {
  case type_kind::boolean:
    return value(*static_cast<const bool *>(p));
  case type_kind::integer:
    return value(load_signed(p, t.size));
  case type_kind::unsigned_integer:
    return value(load_unsigned(p, t.size));
  case type_kind::floating:
    return value(load_real(p, t.size));
  case type_kind::string:
    return value(*static_cast<const string *>(p));
  case type_kind::id:
    return value(static_cast<const id_string *>(p)->view());
  case type_kind::floats: {
    const auto *in = static_cast<const f32 *>(p);
    value a = value::make_array();
    for (u32 i = 0; i < t.components(); ++i)
      a.push_back(value(static_cast<f64>(in[i])));
    return a;
  }
  case type_kind::enumeration: {
    const i64 v = load_enum(t, p);
    // A value the table does not name goes out as its number, which at least
    // round-trips rather than writing an empty string nothing reads back.
    const string_view text = t.enumeration->text(v);
    return text.empty() ? value(v) : value(text);
  }
  case type_kind::aggregate:
    return write_fields(t, p, mode);
  case type_kind::sequence: {
    const auto &seq = *t.sequence;
    value a = value::make_array();
    for (usize i = 0; i < seq.size(p); ++i)
      a.push_back(write_value(*seq.element, seq.const_at(p, i), mode));
    return a;
  }
  case type_kind::opaque:
    break;
  }
  return value();
}

void read_value(const type_info &t, const value &v, void *p) {
  switch (t.kind) {
  case type_kind::boolean:
    *static_cast<bool *>(p) = v.get_bool(*static_cast<const bool *>(p));
    break;
  case type_kind::integer:
    store_integer(p, t.size,
                  static_cast<u64>(v.get_i64(load_signed(p, t.size))));
    break;
  case type_kind::unsigned_integer:
    store_integer(p, t.size, v.get_u64(load_unsigned(p, t.size)));
    break;
  case type_kind::floating:
    store_real(p, t.size, v.get_double(load_real(p, t.size)));
    break;
  case type_kind::string:
    if (v.is_string())
      *static_cast<string *>(p) = string(v.get_string({}));
    break;
  case type_kind::id:
    if (v.is_string())
      *static_cast<id_string *>(p) = id_string(v.get_string({}));
    break;
  case type_kind::floats: {
    auto *out = static_cast<f32 *>(p);
    for (u32 i = 0; i < t.components(); ++i)
      out[i] = cast<f32>(at_or(v, i, cast<f64>(out[i])));
    break;
  }
  case type_kind::enumeration:
    if (v.is_string()) {
      i64 out = 0;
      if (t.enumeration->value(v.get_string({}), out))
        store_integer(p, t.size, static_cast<u64>(out));
    } else if (v.is_number()) {
      store_integer(p, t.size, static_cast<u64>(v.get_i64(load_enum(t, p))));
    }
    break;
  case type_kind::aggregate:
    read_fields(t, v, p);
    break;
  case type_kind::sequence: {
    if (!v.is_array())
      break;
    const auto &seq = *t.sequence;
    seq.resize(p, v.size());
    const usize n = nx::min(v.size(), seq.size(p));
    for (usize i = 0; i < n; ++i)
      read_value(*seq.element, v[i], seq.at(p, i));
    break;
  }
  case type_kind::opaque:
    break;
  }
}

value write_fields(const type_info &type, const void *object,
                   const write_mode mode) {
  value out = value::make_object();
  for (const field_info &f : type.fields) {
    if (!f.serialised() || f.type->kind == type_kind::opaque)
      continue;
    const void *p = at(object, f.offset);
    if (mode == write_mode::elide_defaults && type.defaults != nullptr &&
        same_as(*f.type, p, at(type.defaults, f.offset)))
      continue;
    out[f.key()] = write_value(*f.type, p, mode);
  }
  return out;
}

void read_fields(const type_info &type, const value &in, void *object) {
  for (const field_info &f : type.fields) {
    if (!f.serialised() || f.type->kind == type_kind::opaque)
      continue;
    read_value(*f.type, in[f.key()], at(object, f.offset));
  }
}

}
