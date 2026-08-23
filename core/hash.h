#pragma once

#include "core/foundation/core/foundation.h"

#include <span>
#include <string_view>

namespace nx {

inline constexpr u32 FNV1A32_BASIS = 0x811c9dc5u;
inline constexpr u32 FNV1A32_PRIME = 0x01000193u;
inline constexpr u64 FNV1A64_BASIS = 0xcbf29ce484222325ull;
inline constexpr u64 FNV1A64_PRIME = 0x100000001b3ull;

/// Fold values for in-memory keys, bytes for anything whose byte sequence is the identity.
/// nx::fnv1a64 hash; hash.combine_all(desc.width, desc.height, desc.format); return hash.value();
template <typename T, T Basis, T Prime> class basic_fnv1a {
public:
  using value_type = T;

  constexpr basic_fnv1a() noexcept = default;
  explicit constexpr basic_fnv1a(const T seed) noexcept : m_hash(seed) {}

  constexpr basic_fnv1a &combine(const T value) noexcept {
    m_hash = nx::cast<T>((m_hash ^ value) * Prime);
    return *this;
  }

  /// Folds several values left to right. Enums and narrower integers are
  /// converted; a value wider than T is truncated, so do not feed a u64 to the
  /// 32-bit form and expect all of it to count.
  template <typename... Args>
    requires(sizeof...(Args) > 0)
  constexpr basic_fnv1a &combine_all(const Args &...values) noexcept {
    (combine(nx::cast<T>(values)), ...);
    return *this;
  }

  /// Folds a buffer one octet at a time: canonical FNV-1a over bytes.
  constexpr basic_fnv1a &combine_bytes(const char *data,
                                       const usize len) noexcept {
    for (usize i = 0; i < len; ++i)
      combine(nx::cast<T>(nx::cast<u8>(data[i])));
    return *this;
  }

  [[nodiscard]] constexpr T value() const noexcept { return m_hash; }

private:
  T m_hash = Basis;
};

using fnv1a32 = basic_fnv1a<u32, FNV1A32_BASIS, FNV1A32_PRIME>;
using fnv1a64 = basic_fnv1a<u64, FNV1A64_BASIS, FNV1A64_PRIME>;

[[nodiscard]] constexpr u32 hash_fnv1a32(const char *data, usize len) noexcept {
  return fnv1a32().combine_bytes(data, len).value();
}

[[nodiscard]] constexpr u64 hash_fnv1a64(const char *data, usize len) noexcept {
  return fnv1a64().combine_bytes(data, len).value();
}

[[nodiscard]] inline u32
hash_fnv1a32(const std::span<const std::byte> data) noexcept {
  return hash_fnv1a32(reinterpret_cast<const char *>(data.data()), data.size());
}

[[nodiscard]] inline u64
hash_fnv1a64(const std::span<const std::byte> data) noexcept {
  return hash_fnv1a64(reinterpret_cast<const char *>(data.data()), data.size());
}

[[nodiscard]] constexpr u32 hash_fnv1a32(const std::string_view s) noexcept {
  return hash_fnv1a32(s.data(), s.size());
}

[[nodiscard]] constexpr u64 hash_fnv1a64(const std::string_view s) noexcept {
  return hash_fnv1a64(s.data(), s.size());
}

[[nodiscard]] constexpr u32 hash_u32(u32 val) noexcept {
  val = (val ^ 61u) ^ (val >> 16);
  val = val + (val << 3);
  val = val ^ (val >> 4);
  val = val * 0x27d4eb2du;
  val = val ^ (val >> 15);
  return val;
}

[[nodiscard]] constexpr u64 hash_u64(u64 val) noexcept {
  val = (~val) + (val << 21);
  val = val ^ (val >> 24);
  val = (val + (val << 3)) + (val << 8);
  val = val ^ (val >> 14);
  val = (val + (val << 2)) + (val << 4);
  val = val ^ (val >> 28);
  val = val + (val << 31);
  return val;
}

[[nodiscard]] inline usize hash_ptr(const void *ptr) noexcept {
  if constexpr (sizeof(uintptr_t) == 8)
    return hash_u64(reinterpret_cast<uintptr_t>(ptr));
  else
    return hash_u32(nx::cast<u32>(reinterpret_cast<uintptr_t>(ptr)));
}

[[nodiscard]] constexpr usize hash_comb(usize h1, const usize h2) noexcept {
  if constexpr (sizeof(usize) == 8)
    h1 ^= h2 + 0x9e3779b97f4a7c15ull + (h1 << 6) + (h1 >> 2);
  else
    h1 ^= h2 + 0x9e3779b9u + (h1 << 6) + (h1 >> 2);
  return h1;
}
}
