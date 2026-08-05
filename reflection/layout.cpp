#include "core/foundation/reflection/layout.h"

#include "core/foundation/core/hash.h"
#include "core/foundation/strings/format.h"

namespace nx::reflect {
namespace {

void fold(fnv1a64 &hash, const string_view text) noexcept {
  hash.combine_bytes(text.data(), text.size());
  // A separator, so "ab" + "c" and "a" + "bc" do not fold alike.
  hash.combine(u8{0});
}

/// A field's own size, which the digest needs and field_info does not carry.
[[nodiscard]] u32 width_of(const field_info &field) noexcept {
  return field.type != nullptr ? field.type->size : 0u;
}

} // namespace

u64 layout_digest(const type_info &type) noexcept {
  fnv1a64 hash;
  fold(hash, type.name);
  hash.combine(type.size);
  hash.combine(type.align);
  hash.combine(cast<u32>(type.kind));

  // Declaration order, not sorted: two fields swapped is a different layout
  // even when the set of them is the same.
  for (const field_info &field : type.fields) {
    fold(hash, field.name);
    hash.combine(field.offset);
    hash.combine(width_of(field));
    hash.combine(field.type != nullptr ? cast<u32>(field.type->kind) : 0u);
  }
  return hash.value();
}

string describe_layout(const type_info &type) {
  string out =
      nx::format("{} ({} bytes, align {})\n", type.name, type.size, type.align);
  for (const field_info &field : type.fields)
    out += nx::format("  +{:<4} {:<6} {}\n", field.offset, width_of(field),
                      field.name);
  return out;
}

} // namespace nx::reflect
