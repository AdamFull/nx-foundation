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
  case type_kind::integer: {
    const auto got = static_cast<u64>(v.get_i64(load_signed(p, t.size)));
    if (reflect::integer_fits(got, t.size, true))
      store_integer(p, t.size, got);
    break;
  }
  case type_kind::unsigned_integer: {
    const u64 got = v.get_u64(load_unsigned(p, t.size));
    if (reflect::integer_fits(got, t.size, false))
      store_integer(p, t.size, got);
    break;
  }
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
      const bool is_signed = t.enumeration->is_signed;
      const u64 current = static_cast<u64>(load_enum(t, p));
      const u64 got =
          is_signed ? static_cast<u64>(v.get_i64(static_cast<i64>(current)))
                    : v.get_u64(current);
      if (reflect::integer_fits(got, t.size, is_signed))
        store_integer(p, t.size, got);
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

namespace {

[[nodiscard]] string_view kind_name(const type_kind k) {
  switch (k) {
  case type_kind::opaque:
    return "opaque";
  case type_kind::boolean:
    return "boolean";
  case type_kind::integer:
    return "integer";
  case type_kind::unsigned_integer:
    return "unsigned";
  case type_kind::floating:
    return "float";
  case type_kind::string:
    return "string";
  case type_kind::id:
    return "id";
  case type_kind::floats:
    return "floats";
  case type_kind::enumeration:
    return "enum";
  case type_kind::aggregate:
    return "aggregate";
  case type_kind::sequence:
    return "sequence";
  }
  return "opaque";
}

/// Nested aggregates and sequences are described this deep, no deeper.
constexpr u32 MAX_SCHEMA_DEPTH = 8;

[[nodiscard]] value fields_schema(const type_info &type,
                                  const std::byte *defaults, u32 depth);

/// What @p t is, with what it holds: an aggregate's fields, a sequence's
/// element, an enum's spellings.
void describe_type(value &out, const type_info &t, const std::byte *defaults,
                   const u32 depth) {
  out.insert("kind", value(kind_name(t.kind)));
  if (t.kind == type_kind::floats)
    out.insert("count", value(nx::cast<i64>(t.components())));
  if (t.kind == type_kind::enumeration && t.enumeration != nullptr) {
    value values = value::make_array();
    for (const reflect::enum_spelling &e : t.enumeration->entries)
      values.push_back(value(e.name));
    out.insert("values", std::move(values));
  }
  if (depth >= MAX_SCHEMA_DEPTH)
    return;
  if (t.kind == type_kind::aggregate)
    out.insert("fields", fields_schema(t, defaults, depth + 1));
  if (t.kind == type_kind::sequence && t.sequence != nullptr &&
      t.sequence->element != nullptr) {
    const type_info &element = *t.sequence->element;
    value of = value::make_object();
    of.insert("name", value(element.name));
    describe_type(of, element, static_cast<const std::byte *>(element.defaults),
                  depth + 1);
    out.insert("element", std::move(of));
    if (t.sequence->fixed)
      out.insert("fixed", value(true));
  }
}

[[nodiscard]] value field_schema(const field_info &f, const std::byte *defaults,
                                 const u32 depth) {
  const type_info &t = *f.type;
  const std::byte *held = defaults != nullptr ? defaults + f.offset : nullptr;
  value out = value::make_object();
  out.insert("name", value(f.name));
  out.insert("key", value(f.key()));
  describe_type(out, t, held, depth);
  if (held != nullptr && t.kind != type_kind::opaque)
    out.insert("default", write_value(t, held));
  if (f.has(reflect::field_flag::transient))
    out.insert("transient", value(true));
  if (f.has(reflect::field_flag::read_only))
    out.insert("readOnly", value(true));
  if (f.has(reflect::field_flag::hidden))
    out.insert("hidden", value(true));
  if (f.attributes.min != 0.0 || f.attributes.max != 0.0) {
    out.insert("min", value(f.attributes.min));
    out.insert("max", value(f.attributes.max));
  }
#if NX_REFLECT_EDITOR
  if (!f.attributes.label.empty())
    out.insert("label", value(f.attributes.label));
#endif
  return out;
}

value fields_schema(const type_info &type, const std::byte *defaults,
                    const u32 depth) {
  value fields = value::make_array();
  for (const field_info &f : type.fields)
    fields.push_back(field_schema(f, defaults, depth));
  return fields;
}
}

value schema_of(const type_info &type) {
  value out = value::make_object();
  out.insert("name", value(type.name));
  describe_type(out, type, static_cast<const std::byte *>(type.defaults), 0);
  return out;
}

}
