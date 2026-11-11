#include "core/foundation/core/sha256.h"

#if NX_ARCH_X64
#include <immintrin.h>
#if defined(NX_COMPILER_MSVC) && !defined(NX_COMPILER_CLANG)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#elif NX_ARCH_AARCH64
#if defined(NX_COMPILER_MSVC) && !defined(NX_COMPILER_CLANG)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#if defined(NX_PLATFORM_WINDOWS)
#include <windows.h>
#elif defined(NX_PLATFORM_LINUX) || defined(NX_PLATFORM_ANDROID)
#include <sys/auxv.h>
#endif
#endif

namespace nx {
namespace {

constexpr u32 ROUNDS[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

[[nodiscard]] constexpr u32 rotr(const u32 x, const u32 n) noexcept {
  return (x >> n) | (x << (32u - n));
}

void compress(u32 (&state)[8], const u8 *const block) noexcept {
  u32 w[64];
  for (usize i = 0; i < 16; ++i)
    w[i] = (u32{block[i * 4]} << 24) | (u32{block[i * 4 + 1]} << 16) |
           (u32{block[i * 4 + 2]} << 8) | u32{block[i * 4 + 3]};
  for (usize i = 16; i < 64; ++i) {
    const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  u32 a = state[0], b = state[1], c = state[2], d = state[3];
  u32 e = state[4], f = state[5], g = state[6], h = state[7];
  for (usize i = 0; i < 64; ++i) {
    const u32 t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) +
                   ((e & f) ^ (~e & g)) + ROUNDS[i] + w[i];
    const u32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) +
                   ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

using compress_fn = void (*)(u32 (&)[8], const u8 *, usize) noexcept;

void compress_portable(u32 (&state)[8], const u8 *const data,
                       const usize blocks) noexcept {
  for (usize i = 0; i < blocks; ++i)
    compress(state, data + i * 64);
}

#if NX_ARCH_X64
#if defined(NX_COMPILER_MSVC) && !defined(NX_COMPILER_CLANG)
#define NX_SHA_TARGET
#else
#define NX_SHA_TARGET __attribute__((target("sha,ssse3,sse4.1")))
#endif

/// The SHA extensions' rounds hold the state as ABEF and CDGH and the
/// message as 4-word groups, each scheduled from the four before it.
NX_SHA_TARGET void compress_shani(u32 (&state)[8], const u8 *data,
                                  usize blocks) noexcept {
  const __m128i swap =
      _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
  __m128i cdab = _mm_shuffle_epi32(
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(state)), 0xb1);
  __m128i efgh = _mm_shuffle_epi32(
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(state + 4)), 0x1b);
  __m128i abef = _mm_alignr_epi8(cdab, efgh, 8);
  __m128i cdgh = _mm_blend_epi16(efgh, cdab, 0xf0);

  for (; blocks != 0; --blocks, data += 64) {
    const __m128i abef_before = abef;
    const __m128i cdgh_before = cdgh;
    __m128i w[4];
    for (int group = 0; group < 16; ++group) {
      __m128i &words = w[group & 3];
      if (group < 4) {
        words =
            _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i *>(
                                 data + group * 16)),
                             swap);
      } else {
        const __m128i before = w[(group - 1) & 3];
        __m128i next = _mm_sha256msg1_epu32(words, w[(group - 3) & 3]);
        next =
            _mm_add_epi32(next, _mm_alignr_epi8(before, w[(group - 2) & 3], 4));
        words = _mm_sha256msg2_epu32(next, before);
      }
      __m128i message = _mm_add_epi32(
          words, _mm_loadu_si128(
                     reinterpret_cast<const __m128i *>(ROUNDS + group * 4)));
      cdgh = _mm_sha256rnds2_epu32(cdgh, abef, message);
      message = _mm_shuffle_epi32(message, 0x0e);
      abef = _mm_sha256rnds2_epu32(abef, cdgh, message);
    }
    abef = _mm_add_epi32(abef, abef_before);
    cdgh = _mm_add_epi32(cdgh, cdgh_before);
  }

  const __m128i feba = _mm_shuffle_epi32(abef, 0x1b);
  const __m128i dchg = _mm_shuffle_epi32(cdgh, 0xb1);
  _mm_storeu_si128(reinterpret_cast<__m128i *>(state),
                   _mm_blend_epi16(feba, dchg, 0xf0));
  _mm_storeu_si128(reinterpret_cast<__m128i *>(state + 4),
                   _mm_alignr_epi8(dchg, feba, 8));
}

[[nodiscard]] bool cpu_has_sha() noexcept {
  unsigned r[4] = {};
#if defined(NX_COMPILER_MSVC) && !defined(NX_COMPILER_CLANG)
  __cpuid(reinterpret_cast<int *>(r), 0);
  if (r[0] < 7)
    return false;
  __cpuid(reinterpret_cast<int *>(r), 1);
  const unsigned ecx = r[2];
  __cpuidex(reinterpret_cast<int *>(r), 7, 0);
  const unsigned ebx7 = r[1];
#else
  if (__get_cpuid_max(0, nullptr) < 7)
    return false;
  __get_cpuid(1, &r[0], &r[1], &r[2], &r[3]);
  const unsigned ecx = r[2];
  __cpuid_count(7, 0, r[0], r[1], r[2], r[3]);
  const unsigned ebx7 = r[1];
#endif
  const bool ssse3 = (ecx >> 9) & 1, sse41 = (ecx >> 19) & 1;
  const bool sha = (ebx7 >> 29) & 1;
  return ssse3 && sse41 && sha;
}
#endif

#if NX_ARCH_AARCH64
#if defined(NX_COMPILER_MSVC) && !defined(NX_COMPILER_CLANG)
#define NX_SHA2_TARGET
#elif defined(NX_COMPILER_CLANG)
#define NX_SHA2_TARGET __attribute__((target("sha2")))
#else
#define NX_SHA2_TARGET __attribute__((target("+sha2")))
#endif

/// ARMv8's SHA-256 instructions keep the state as ABCD and EFGH and schedule
/// each 4-word group from the four before it.
NX_SHA2_TARGET void compress_armv8(u32 (&state)[8], const u8 *data,
                                   usize blocks) noexcept {
  uint32x4_t abcd = vld1q_u32(state);
  uint32x4_t efgh = vld1q_u32(state + 4);
  for (; blocks != 0; --blocks, data += 64) {
    const uint32x4_t abcd_before = abcd;
    const uint32x4_t efgh_before = efgh;
    uint32x4_t w[4];
    for (int group = 0; group < 16; ++group) {
      uint32x4_t &words = w[group & 3];
      if (group < 4)
        words = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + group * 16)));
      else
        words = vsha256su1q_u32(vsha256su0q_u32(words, w[(group - 3) & 3]),
                                w[(group - 2) & 3], w[(group - 1) & 3]);
      const uint32x4_t message =
          vaddq_u32(words, vld1q_u32(ROUNDS + group * 4));
      const uint32x4_t abcd_now = abcd;
      abcd = vsha256hq_u32(abcd, efgh, message);
      efgh = vsha256h2q_u32(efgh, abcd_now, message);
    }
    abcd = vaddq_u32(abcd, abcd_before);
    efgh = vaddq_u32(efgh, efgh_before);
  }
  vst1q_u32(state, abcd);
  vst1q_u32(state + 4, efgh);
}

[[nodiscard]] bool cpu_has_sha2() noexcept {
#if defined(NX_PLATFORM_APPLE)
  return true;
#elif defined(NX_PLATFORM_WINDOWS)
  return IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE) !=
         0;
#elif defined(NX_PLATFORM_LINUX) || defined(NX_PLATFORM_ANDROID)
  constexpr unsigned long HWCAP_SHA2_BIT = 1ul << 6;
  return (getauxval(AT_HWCAP) & HWCAP_SHA2_BIT) != 0;
#else
  return false;
#endif
}
#endif

[[nodiscard]] compress_fn best_compress() noexcept {
#if NX_ARCH_X64
  static const compress_fn chosen =
      cpu_has_sha() ? compress_shani : compress_portable;
  return chosen;
#elif NX_ARCH_AARCH64
  static const compress_fn chosen =
      cpu_has_sha2() ? compress_armv8 : compress_portable;
  return chosen;
#else
  return compress_portable;
#endif
}

[[nodiscard]] std::array<u8, SHA256_BYTES>
digest_with(const compress_fn compress_blocks,
            const std::span<const u8> bytes) noexcept {
  u32 state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const usize whole = bytes.size() / 64 * 64;
  compress_blocks(state, bytes.data(), whole / 64);

  // The rest, a one bit, zeros and the length in bits fill one block or two.
  u8 tail[128] = {};
  const usize rest = bytes.size() - whole;
  for (usize i = 0; i < rest; ++i)
    tail[i] = bytes[whole + i];
  tail[rest] = 0x80;
  const usize blocks = rest < 56 ? 1 : 2;
  const u64 bits = nx::cast<u64>(bytes.size()) * 8u;
  for (usize i = 0; i < 8; ++i)
    tail[blocks * 64 - 1 - i] = nx::cast<u8>(bits >> (i * 8));
  compress_blocks(state, tail, blocks);

  std::array<u8, SHA256_BYTES> digest{};
  for (usize i = 0; i < 8; ++i)
    for (usize j = 0; j < 4; ++j)
      digest[i * 4 + j] = nx::cast<u8>(state[i] >> (24 - j * 8));
  return digest;
}

} // namespace

std::array<u8, SHA256_BYTES> sha256(const std::span<const u8> bytes) noexcept {
  return digest_with(best_compress(), bytes);
}

namespace detail {

std::array<u8, SHA256_BYTES>
sha256_portable(const std::span<const u8> bytes) noexcept {
  return digest_with(compress_portable, bytes);
}

bool sha256_accelerated() noexcept {
  return best_compress() != compress_portable;
}

} // namespace detail

} // namespace nx
