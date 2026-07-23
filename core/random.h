#pragma once

#include "foundation/strings/utf8_string.h"

#include <array>
#include <optional>
#include <span>

namespace nx {

/**
 * @brief 128-bit globally unique identifier (UUID).
 *
 * Holds 16 raw bytes. Parses from and formats to the canonical
 * 8-4-4-4-12 hex string, is comparable and hashable. Generate one with
 * nx::Rng::guid().
 */
struct Guid {
  std::array<u8, 16> bytes{};

  [[nodiscard]] static constexpr std::optional<Guid>
  parse(string_view str) noexcept {
    constexpr auto hex_val = [](const char c) noexcept -> i32 {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      return -1;
    };

    constexpr int layout[] = {8, 4, 4, 4, 12};
    Guid result{};
    int byte_idx = 0;
    usize pos = 0;

    for (int seg = 0; seg < 5; ++seg) {
      if (seg > 0) {
        if (pos >= str.size() || str[pos] != '-')
          return std::nullopt;
        ++pos;
      }
      for (int i = 0; i < layout[seg]; i += 2) {
        if (pos + 1 >= str.size())
          return std::nullopt;
        const i32 hi = hex_val(str[pos]);
        const i32 lo = hex_val(str[pos + 1]);
        if (hi < 0 || lo < 0)
          return std::nullopt;
        result.bytes[byte_idx++] = nx::cast<u8>((hi << 4) | lo);
        pos += 2;
      }
    }

    if (pos != str.size())
      return std::nullopt;
    if ((result.bytes[6] >> 4) != 4)
      return std::nullopt;
    if ((result.bytes[8] >> 6) != 2)
      return std::nullopt;

    return result;
  }

  [[nodiscard]] constexpr std::array<char, 37> to_chars() const noexcept {
    constexpr char hex[] = "0123456789abcdef";
    constexpr int group_sizes[] = {4, 2, 2, 2, 6};
    std::array<char, 37> out{};
    int bi = 0, ci = 0;

    for (int g = 0; g < 5; ++g) {
      if (g > 0)
        out[ci++] = '-';
      for (int n = 0; n < group_sizes[g]; ++n) {
        out[ci++] = hex[bytes[bi] >> 4];
        out[ci++] = hex[bytes[bi] & 0x0f];
        ++bi;
      }
    }
    out[ci] = '\0';
    return out;
  }

  [[nodiscard]] string to_string() const { return to_chars().data(); }

  bool operator==(const Guid &) const = default;
  auto operator<=>(const Guid &) const = default;
};

struct Rng {
  enum class Alg { Pcg, Xoshiro256, Splitmix64 };

  struct PcgState {
    u64 state;
    u64 inc;
  };
  struct Xoshiro256State {
    u64 s[4];
  };
  struct Splitmix64State {
    u64 state;
  };

  Alg alg;
  union {
    PcgState pcg;
    Xoshiro256State xoshiro256;
    Splitmix64State splitmix64;
  };

  Rng() = default;
  Rng(const Alg alg, const u64 seed) { init(alg, seed); }

  void init(Alg alg, u64 seed);
  void seed(u64 seed);

  [[nodiscard]] u32 next_u32();
  [[nodiscard]] u64 next_u64();

  // Unbiased value in [0, bound).
  [[nodiscard]] u32 next_u32(u32 bound);
  [[nodiscard]] u64 next_u64(u64 bound);

  // Inclusive integer range [min, max].
  [[nodiscard]] i32 range(i32 min, i32 max);
  [[nodiscard]] i64 range(i64 min, i64 max);

  // Value in [0, 1).
  [[nodiscard]] f32 next_f32();
  [[nodiscard]] f64 next_f64();

  [[nodiscard]] f32 range(f32 min, f32 max);
  [[nodiscard]] f64 range(f64 min, f64 max);

  [[nodiscard]] bool chance(f32 probability);

  void fill(std::span<std::byte> buffer);

  [[nodiscard]] Guid guid();
};

} // namespace nx

template <> struct std::hash<nx::Guid> {
  std::size_t operator()(const nx::Guid &g) const noexcept {
    const auto words = std::bit_cast<std::array<u64, 2>>(g.bytes);
    const auto lo = words[0];
    const auto hi = words[1];

    u64 h = lo ^ hi;
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h;
  }
};