#pragma once

#include "foundation/core/foundation.h"

#include <span>

namespace nx {
[[nodiscard]] constexpr u32 hash_fnv1a32(const char *data, usize len) noexcept {
  u32 hash = 0x811c9dc5u;
  for (usize i = 0; i < len; ++i) {
    hash ^= nx::cast<u8>(data[i]);
    hash *= 0x01000193u;
  }
  return hash;
}

[[nodiscard]] constexpr u64 hash_fnv1a64(const char *data, usize len) noexcept {
  u64 hash = 0xcbf29ce484222325ull;
  for (usize i = 0; i < len; ++i) {
    hash ^= nx::cast<u8>(data[i]);
    hash *= 0x100000001b3ull;
  }
  return hash;
}

[[nodiscard]] inline u32
hash_fnv1a32(const std::span<const std::byte> data) noexcept {
  return hash_fnv1a32(reinterpret_cast<const char *>(data.data()), data.size());
}

[[nodiscard]] inline u64
hash_fnv1a64(const std::span<const std::byte> data) noexcept {
  return hash_fnv1a64(reinterpret_cast<const char *>(data.data()), data.size());
}

// https://gist.github.com/badboy/6267743
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

// https://stackoverflow.com/questions/2590677/how-do-i-combine-hash-values-in-c0x
[[nodiscard]] constexpr usize hash_comb(usize h1, const usize h2) noexcept {
  if constexpr (sizeof(usize) == 8)
    h1 ^= h2 + 0x9e3779b97f4a7c15ull + (h1 << 6) + (h1 >> 2);
  else
    h1 ^= h2 + 0x9e3779b9u + (h1 << 6) + (h1 >> 2);
  return h1;
}
} // namespace nx