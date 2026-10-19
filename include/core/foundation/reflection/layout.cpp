#include "core/foundation/reflection/layout.h"

#include "core/foundation/containers/small_vector.h"
#include "core/foundation/core/hash.h"
#include "core/foundation/strings/format.h"

#include <algorithm>

namespace nx::reflect {
namespace {

void fold(fnv1a64 &hash, const string_view text) noexcept {
  hash.combine_bytes(text.data(), text.size());
  // A separator, so "ab" + "c" and "a" + "bc" do not fold alike.
  hash.combine(u8{0});
}

[[nodiscard]] u32 width_of(const field_info &field) noexcept {
  return field.type != nullptr ? field.type->size : 0u;
}

using open_types = small_vector<const type_info *, 8>;

/// A type and, all the way down, the fields and elements it holds. One
/// already being folded further up, as a type that holds itself through a
/// sequence is, adds only its name and size.
void fold_type(fnv1a64 &hash, const type_info *type, open_types &open) {
  if (type == nullptr) {
    hash.combine(u8{0});
    return;
  }
  fold(hash, type->name);
  hash.combine(type->size);
  hash.combine(type->align);
  hash.combine(cast<u32>(type->kind));
  if (std::find(open.begin(), open.end(), type) != open.end())
    return;
  open.push_back(type);
  for (const field_info &field : type->fields) {
    fold(hash, field.name);
    hash.combine(field.offset);
    fold_type(hash, field.type, open);
  }
  if (type->kind == type_kind::sequence && type->sequence != nullptr)
    fold_type(hash, type->sequence->element, open);
  open.pop_back();
}

} // namespace

u64 layout_digest(const type_info &type) noexcept {
  fnv1a64 hash;
  open_types open;
  fold_type(hash, &type, open);
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

}
