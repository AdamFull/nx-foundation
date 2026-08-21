#include "core/foundation/serialization/reflect_binary.h"

#include "core/foundation/strings/utf8_string.h"

#include <cstring>

namespace nx::reflect {
namespace {

// A field is a byte offset into a struct this code knows nothing about, so it is
// reached through memcpy rather than a cast - it is not reliably aligned for its
// type. The same scalar helpers reflect_json uses; kept file-local here so the
// binary path has no dependency on the JSON one.

[[nodiscard]] const std::byte *at(const void *p, const usize offset) noexcept {
  return static_cast<const std::byte *>(p) + offset;
}
[[nodiscard]] std::byte *at(void *p, const usize offset) noexcept {
  return static_cast<std::byte *>(p) + offset;
}

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

void write_one(const type_info &t, const void *p, nx::nva::Writer &w);
[[nodiscard]] bool read_one(const type_info &t, nx::nva::Reader &r, void *p);

void write_aggregate(const type_info &t, const void *p, nx::nva::Writer &w) {
  for (const field_info &f : t.fields) {
    if (!f.serialised() || f.type->kind == type_kind::opaque)
      continue;
    write_one(*f.type, at(p, f.offset), w);
  }
}
[[nodiscard]] bool read_aggregate(const type_info &t, nx::nva::Reader &r,
                                  void *p) {
  for (const field_info &f : t.fields) {
    if (!f.serialised() || f.type->kind == type_kind::opaque)
      continue;
    if (!read_one(*f.type, r, at(p, f.offset)))
      return false;
  }
  return true;
}

void write_one(const type_info &t, const void *p, nx::nva::Writer &w) {
  switch (t.kind) {
  case type_kind::boolean:
    w.u8v(*static_cast<const bool *>(p) ? 1u : 0u);
    break;
  case type_kind::integer:
    w.u64v(static_cast<u64>(load_signed(p, t.size)));
    break;
  case type_kind::unsigned_integer:
    w.u64v(load_unsigned(p, t.size));
    break;
  case type_kind::enumeration:
    w.u64v(static_cast<u64>(load_enum(t, p)));
    break;
  case type_kind::floating:
    if (t.size == sizeof(f32)) {
      w.f32v(static_cast<f32>(load_real(p, t.size)));
    } else {
      const f64 v = load_real(p, t.size);
      u64 bits = 0;
      std::memcpy(&bits, &v, sizeof(bits));
      w.u64v(bits);
    }
    break;
  case type_kind::string:
    w.str(static_cast<const string *>(p)->view());
    break;
  case type_kind::id:
    w.str(static_cast<const id_string *>(p)->view());
    break;
  case type_kind::floats: {
    const auto *in = static_cast<const f32 *>(p);
    for (u32 i = 0; i < t.components(); ++i)
      w.f32v(in[i]);
    break;
  }
  case type_kind::aggregate:
    write_aggregate(t, p, w);
    break;
  case type_kind::sequence: {
    const auto &seq = *t.sequence;
    const usize n = seq.size(p);
    w.u32v(nx::cast<u32>(n));
    for (usize i = 0; i < n; ++i)
      write_one(*seq.element, seq.const_at(p, i), w);
    break;
  }
  case type_kind::opaque:
    break;
  }
}

[[nodiscard]] bool read_one(const type_info &t, nx::nva::Reader &r, void *p) {
  switch (t.kind) {
  case type_kind::boolean:
    *static_cast<bool *>(p) = r.u8v() != 0u;
    break;
  case type_kind::integer:
  case type_kind::unsigned_integer:
  case type_kind::enumeration:
    store_integer(p, t.size, r.u64v());
    break;
  case type_kind::floating:
    if (t.size == sizeof(f32)) {
      store_real(p, t.size, r.f32v());
    } else {
      const u64 bits = r.u64v();
      f64 v = 0.0;
      std::memcpy(&v, &bits, sizeof(v));
      store_real(p, t.size, v);
    }
    break;
  case type_kind::string:
    *static_cast<string *>(p) = string(r.str());
    break;
  case type_kind::id:
    *static_cast<id_string *>(p) = id_string(r.str());
    break;
  case type_kind::floats: {
    auto *out = static_cast<f32 *>(p);
    for (u32 i = 0; i < t.components(); ++i)
      out[i] = r.f32v();
    break;
  }
  case type_kind::aggregate:
    if (!read_aggregate(t, r, p))
      return false;
    break;
  case type_kind::sequence: {
    const auto &seq = *t.sequence;
    const u32 n = r.u32v();
    if (!r.ok())
      return false;
    seq.resize(p, n);
    // A dynamic sequence now holds exactly n; a fixed array that no longer does
    // is a struct that changed under a stale artifact - fail so the caller
    // recooks rather than reading the stream out of step.
    if (seq.size(p) != n)
      return false;
    for (u32 i = 0; i < n; ++i)
      if (!read_one(*seq.element, r, seq.at(p, i)))
        return false;
    break;
  }
  case type_kind::opaque:
    break;
  }
  return r.ok();
}

} // namespace

void write_binary(const type_info &type, const void *object,
                  nx::nva::Writer &w) {
  write_one(type, object, w);
}

bool read_binary(const type_info &type, nx::nva::Reader &r, void *object) {
  return read_one(type, r, object);
}

} // namespace nx::reflect
