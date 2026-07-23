#pragma once

#include <compare>
#include <type_traits>

#include "foundation/core/foundation.h"
#include "foundation/core/hash.h"

namespace nx {

// A packed handle: low IndexBits hold the slot index, the remaining high bits
// hold the version (generation). The all-ones value is the canonical null.
//
//   basic_handle<u32, 24>  ->  24 index bits / 8  version bits
//   basic_handle<u64, 32>  ->  32 index bits / 32 version bits
//
// Tag makes handles to different resource kinds distinct types so they cannot
// be assigned to one another by mistake.
template <typename Repr, usize IndexBits, typename Tag = void>
class basic_handle {
  static_assert(std::is_unsigned_v<Repr>, "Repr must be an unsigned integer");
  static_assert(IndexBits > 0 && IndexBits < sizeof(Repr) * 8,
                "IndexBits must leave room for at least one version bit");

public:
  using repr_type = Repr;
  using tag_type = Tag;

  static constexpr usize index_bits = IndexBits;
  static constexpr usize version_bits = sizeof(Repr) * 8 - IndexBits;
  static constexpr Repr index_mask = (Repr(1) << IndexBits) - 1;
  static constexpr Repr version_mask = (Repr(1) << version_bits) - 1;

  // index_mask is reserved to mean "no slot", so max_index is one below it.
  static constexpr Repr null_index = index_mask;
  static constexpr Repr max_index = index_mask - 1;
  static constexpr Repr max_version = version_mask;

  constexpr basic_handle() noexcept : value_(null_value) {}

  [[nodiscard]] static constexpr basic_handle
  from_parts(Repr index, Repr version) noexcept {
    return basic_handle(((version & version_mask) << IndexBits) |
                        (index & index_mask));
  }

  [[nodiscard]] static constexpr basic_handle from_raw(Repr raw) noexcept {
    return basic_handle(raw);
  }

  [[nodiscard]] static constexpr basic_handle null() noexcept {
    return basic_handle();
  }

  [[nodiscard]] constexpr Repr index() const noexcept {
    return value_ & index_mask;
  }
  [[nodiscard]] constexpr Repr version() const noexcept {
    return value_ >> IndexBits;
  }
  [[nodiscard]] constexpr Repr raw() const noexcept { return value_; }

  [[nodiscard]] constexpr bool valid() const noexcept {
    return index() != null_index;
  }
  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return valid();
  }
  void invalidate() noexcept {
    value_ = null_value;
  }

  [[nodiscard]] friend constexpr bool operator==(basic_handle a,
                                                 basic_handle b) noexcept {
    return a.value_ == b.value_;
  }
  [[nodiscard]] friend constexpr std::strong_ordering
  operator<=>(basic_handle a, basic_handle b) noexcept {
    return a.value_ <=> b.value_;
  }

private:
  static constexpr Repr null_value = static_cast<Repr>(~Repr(0));

  explicit constexpr basic_handle(Repr v) noexcept : value_(v) {}

  Repr value_;
};

using handle32 = basic_handle<u32, 24>;
using handle64 = basic_handle<u64, 32>;

template <typename Tag> using typed_handle32 = basic_handle<u32, 24, Tag>;
template <typename Tag> using typed_handle64 = basic_handle<u64, 32, Tag>;

} // namespace nx

namespace std {

template <typename Repr, std::size_t IndexBits, typename Tag>
struct hash<nx::basic_handle<Repr, IndexBits, Tag>> {
  [[nodiscard]] std::size_t
  operator()(const nx::basic_handle<Repr, IndexBits, Tag> &h) const noexcept {
    if constexpr (sizeof(Repr) <= 4)
      return nx::hash_u32(static_cast<u32>(h.raw()));
    else
      return nx::hash_u64(static_cast<u64>(h.raw()));
  }
};

} // namespace std