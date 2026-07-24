#include "foundation/core/random.h"

#include <bit>

namespace nx {
namespace {

constexpr u64 PCG_MUL = 6364136223846793005ull;

void pcg_seed(Rng::PcgState &s, const u64 seed) {
  s.state = 0;
  s.inc = (seed << 1) | 1;
  s.state = s.state * PCG_MUL + s.inc;
  s.state += seed;
  s.state = s.state * PCG_MUL + s.inc;
}

u32 pcg_next(Rng::PcgState &s) {
  const u64 oldstate = s.state;
  s.state = oldstate * PCG_MUL + s.inc;
  const u32 xorshifted = nx::cast<u32>(((oldstate >> 18u) ^ oldstate) >> 27u);
  const u32 rot = nx::cast<u32>(oldstate >> 59u);
  return std::rotr(xorshifted, nx::cast<i32>(rot));
}

void splitmix64_seed(Rng::Splitmix64State &s, const u64 seed) {
  s.state = seed;
}

u64 splitmix64_next(Rng::Splitmix64State &s) {
  u64 z = (s.state += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

void xoshiro256_seed(Rng::Xoshiro256State &s, const u64 seed) {
  Rng::Splitmix64State sm;
  splitmix64_seed(sm, seed);
  for (u64 &v : s.s)
    v = splitmix64_next(sm);
}

u64 xoshiro256_next(Rng::Xoshiro256State &s) {
  const u64 result = std::rotl(s.s[1] * 5, 7) * 9;
  const u64 t = s.s[1] << 17;

  s.s[2] ^= s.s[0];
  s.s[3] ^= s.s[1];
  s.s[1] ^= s.s[2];
  s.s[0] ^= s.s[3];
  s.s[2] ^= t;
  s.s[3] = std::rotl(s.s[3], 45);

  return result;
}

} // namespace

void Rng::init(const Alg algorithm, const u64 seed_value) {
  alg = algorithm;
  seed(seed_value);
}

void Rng::seed(const u64 seed_value) {
  switch (alg) {
  case Alg::Pcg:
    pcg_seed(pcg, seed_value);
    break;
  case Alg::Xoshiro256:
    xoshiro256_seed(xoshiro256, seed_value);
    break;
  case Alg::Splitmix64:
    splitmix64_seed(splitmix64, seed_value);
    break;
  }
}

u32 Rng::next_u32() {
  switch (alg) {
  case Alg::Pcg:
    return pcg_next(pcg);
  case Alg::Xoshiro256:
    return nx::cast<u32>(xoshiro256_next(xoshiro256));
  case Alg::Splitmix64:
    return nx::cast<u32>(splitmix64_next(splitmix64));
  }
  return 0;
}

u64 Rng::next_u64() {
  switch (alg) {
  case Alg::Pcg: {
    const u64 hi = pcg_next(pcg);
    const u64 lo = pcg_next(pcg);
    return (hi << 32) | lo;
  }
  case Alg::Xoshiro256:
    return xoshiro256_next(xoshiro256);
  case Alg::Splitmix64:
    return splitmix64_next(splitmix64);
  }
  return 0;
}

u32 Rng::next_u32(const u32 bound) {
  if (bound == 0)
    return 0;

  // Lemire's method for unbiased bounded random numbers.
  const u32 threshold = (0u - bound) % bound;
  for (;;) {
    const u32 r = next_u32();
    const u64 m = nx::cast<u64>(r) * bound;
    const u32 low = nx::cast<u32>(m);
    if (low < threshold)
      continue;
    return nx::cast<u32>(m >> 32);
  }
}

u64 Rng::next_u64(const u64 bound) {
  if (bound == 0)
    return 0;
  const u64 threshold = (0ull - bound) % bound; // 2^64 mod bound
  for (;;) {
    const u64 r = next_u64();
    if (r >= threshold)
      return r % bound;
  }
}

i32 Rng::range(i32 min, i32 max) {
  if (min > max)
    std::swap(min, max);
  const u32 span = nx::cast<u32>(max) - nx::cast<u32>(min);
  if (span == ~u32{0}) // the full range: every value is in bounds
    return nx::cast<i32>(next_u32());
  return nx::cast<i32>(nx::cast<u32>(min) + next_u32(span + 1));
}

i64 Rng::range(i64 min, i64 max) {
  if (min > max)
    std::swap(min, max);
  const u64 span = nx::cast<u64>(max) - nx::cast<u64>(min);
  if (span == ~u64{0})
    return nx::cast<i64>(next_u64());
  return nx::cast<i64>(nx::cast<u64>(min) + next_u64(span + 1));
}

f32 Rng::next_f32() {
  const u32 r = next_u32() >> 8;
  return nx::cast<f32>(r) * (1.0f / 16777216.0f);
}

f64 Rng::next_f64() {
  const u64 r = next_u64() >> 11;
  return nx::cast<f64>(r) * (1.0 / 9007199254740992.0);
}

f32 Rng::range(const f32 min, const f32 max) {
  return min + next_f32() * (max - min);
}

f64 Rng::range(const f64 min, const f64 max) {
  return min + next_f64() * (max - min);
}

bool Rng::chance(const f32 probability) { return next_f32() < probability; }

void Rng::fill(const std::span<std::byte> buffer) {
  const usize count = buffer.size();
  usize i = 0;

  while (i + 8 <= count) {
    const u64 val = next_u64();
    std::memcpy(buffer.data() + i, &val, 8);
    i += 8;
  }

  if (i < count) {
    const u64 val = next_u64();
    std::memcpy(buffer.data() + i, &val, count - i);
  }
}

Guid Rng::guid() {
  Guid g;
  fill(std::as_writable_bytes(std::span(g.bytes)));
  g.bytes[6] = (g.bytes[6] & 0x0f) | 0x40;
  g.bytes[8] = (g.bytes[8] & 0x3f) | 0x80;
  return g;
}

} // namespace nx