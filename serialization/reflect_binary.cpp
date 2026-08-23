#include "core/foundation/serialization/reflect_binary.h"

#include "core/foundation/strings/utf8_string.h"

#include <cstring>

namespace nx::reflect {
namespace {

inline constexpr u32 MAX_READ_DEPTH = 128;
inline constexpr u32 MAX_ZERO_WIDTH_SEQUENCE_ITEMS = 4096;

// A field is a byte offset into a struct this code knows nothing about, so it
// is reached through memcpy rather than a cast - it is not reliably aligned for
// its type.

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
[[nodiscard]] bool read_one(const type_info &t, nx::nva::Reader &r, void *p,
                            u32 depth);

/// The least number of bytes one value of @p t can occupy on the wire. A
/// sequence contributes its count prefix only: its payload may legitimately be
/// empty. Saturation is conservative and makes impossible layouts fail before
/// allocation.
[[nodiscard]] u64 minimum_encoded_size(const type_info &t) noexcept {
  switch (t.kind) {
  case type_kind::boolean:
    return 1;
  case type_kind::integer:
  case type_kind::unsigned_integer:
  case type_kind::enumeration:
    return 8;
  case type_kind::floating:
    return t.size == sizeof(f32) ? 4 : 8;
  case type_kind::string:
  case type_kind::id:
  case type_kind::sequence:
    return 4;
  case type_kind::floats:
    return nx::cast<u64>(t.components()) * sizeof(f32);
  case type_kind::aggregate: {
    u64 total = 0;
    for (const field_info &f : t.fields) {
      if (!f.serialised() || f.type->kind == type_kind::opaque)
        continue;
      const u64 size = minimum_encoded_size(*f.type);
      if (size > ~u64{0} - total)
        return ~u64{0};
      total += size;
    }
    return total;
  }
  case type_kind::opaque:
    return 0;
  }
  return 0;
}

void write_aggregate(const type_info &t, const void *p, nx::nva::Writer &w) {
  for (const field_info &f : t.fields) {
    if (!f.serialised() || f.type->kind == type_kind::opaque)
      continue;
    write_one(*f.type, at(p, f.offset), w);
  }
}
[[nodiscard]] bool read_aggregate(const type_info &t, nx::nva::Reader &r,
                                  void *p, const u32 depth) {
  for (const field_info &f : t.fields) {
    if (!f.serialised() || f.type->kind == type_kind::opaque)
      continue;
    if (!read_one(*f.type, r, at(p, f.offset), depth + 1))
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

[[nodiscard]] bool read_one(const type_info &t, nx::nva::Reader &r, void *p,
                            const u32 depth) {
  if (depth > MAX_READ_DEPTH) {
    r.fail();
    return false;
  }
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
    if (!read_aggregate(t, r, p, depth))
      return false;
    break;
  case type_kind::sequence: {
    const auto &seq = *t.sequence;
    const u32 n = r.u32v();
    if (!r.ok())
      return false;
    const u64 min_size = minimum_encoded_size(*seq.element);
    if ((min_size != 0 && !r.require_items(n, min_size)) ||
        (min_size == 0 && n > MAX_ZERO_WIDTH_SEQUENCE_ITEMS)) {
      r.fail();
      return false;
    }
    seq.resize(p, n);
    if (seq.size(p) != n)
      return false;
    for (u32 i = 0; i < n; ++i)
      if (!read_one(*seq.element, r, seq.at(p, i), depth + 1))
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
  return read_one(type, r, object, 0);
}

} // namespace nx::reflect
