#include "framework/nxtest.h"

#include "core/foundation/serialization/zstd.h"

#include <algorithm>
#include <span>

namespace {

/// @p size bytes that compress well but are not one byte repeated.
[[nodiscard]] nx::vector<u8> pattern(const usize size) {
  nx::vector<u8> out(size);
  for (usize i = 0; i < size; ++i)
    out[i] = static_cast<u8>((i / 7) % 13);
  return out;
}

[[nodiscard]] bool same(const std::span<const u8> a,
                        const std::span<const u8> b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end());
}

/// @p source fed to @p compressor in pieces of @p piece bytes.
[[nodiscard]] nx::vector<u8> streamed(nx::zstd::compressor &compressor,
                                      const std::span<const u8> source,
                                      const usize piece,
                                      const u64 content_size) {
  nx::vector<u8> out;
  REQUIRE(compressor.begin(content_size));
  for (usize at = 0; at < source.size(); at += piece)
    REQUIRE(compressor.feed(
        source.subspan(at, std::min(piece, source.size() - at)), out));
  REQUIRE(compressor.finish(out));
  return out;
}

} // namespace

TEST_CASE("zstd: a buffer compresses smaller and decompresses to itself") {
  const nx::vector<u8> source = pattern(100'000);
  const auto frame = nx::zstd::compress(source);
  REQUIRE(frame);
  CHECK(frame->size() < source.size() / 10);
  CHECK(frame->size() <= nx::zstd::compress_bound(source.size()));
  const auto back = nx::zstd::decompress(*frame, source.size());
  REQUIRE(back);
  CHECK(same(*back, source));

  const auto empty = nx::zstd::compress({});
  REQUIRE(empty);
  const auto nothing = nx::zstd::decompress(*empty, 0);
  REQUIRE(nothing);
  CHECK(nothing->empty());
}

TEST_CASE("zstd: one context compresses many buffers as fresh ones do, and "
          "levels past the range are clamped") {
  nx::zstd::compressor kept;
  nx::zstd::decompressor reader;
  for (usize size : {1u, 4'000u, 70'000u, 9u}) {
    const nx::vector<u8> source = pattern(size);
    nx::vector<u8> frame{1, 2, 3};
    REQUIRE(kept.compress(source, frame));
    const std::span<const u8> appended = std::span<const u8>(frame).subspan(3);
    const auto fresh = nx::zstd::compress(source);
    REQUIRE(fresh);
    CHECK(same(appended, *fresh));
    const auto back = reader.decompress(appended, size);
    REQUIRE(back);
    CHECK(same(*back, source));
  }

  nx::zstd::compressor high(1000);
  nx::vector<u8> frame;
  REQUIRE(high.compress(pattern(500), frame));
  const auto back = nx::zstd::decompress(frame, 500);
  REQUIRE(back);
  CHECK(same(*back, pattern(500)));
}

TEST_CASE("zstd: a frame fed in pieces, its size known or not, reads back "
          "whole or in pieces") {
  const nx::vector<u8> source = pattern(300'000);
  nx::zstd::compressor compressor;
  for (const u64 size :
       {nx::zstd::compressor::UNKNOWN_SIZE, static_cast<u64>(source.size())}) {
    const nx::vector<u8> frame = streamed(compressor, source, 4'096, size);
    const auto whole = nx::zstd::decompress(frame, source.size());
    REQUIRE(whole);
    CHECK(same(*whole, source));

    nx::zstd::decompressor reader;
    reader.begin(source.size());
    nx::vector<u8> out;
    bool ended = false;
    for (usize at = 0; at < frame.size() && !ended; at += 1'000) {
      const auto step =
          reader.feed(std::span<const u8>(frame).subspan(
                          at, std::min<usize>(1'000, frame.size() - at)),
                      out);
      REQUIRE(step);
      ended = *step;
    }
    CHECK(ended);
    CHECK(same(out, source));
  }
}

TEST_CASE("zstd: frames one after another read back as one") {
  const nx::vector<u8> first = pattern(10'000);
  const nx::vector<u8> second = pattern(3'000);
  nx::zstd::compressor compressor;
  nx::vector<u8> sized;
  REQUIRE(compressor.compress(first, sized));
  nx::vector<u8> mixed = sized;
  REQUIRE(compressor.compress(second, sized));
  const nx::vector<u8> unsized =
      streamed(compressor, second, 512, nx::zstd::compressor::UNKNOWN_SIZE);
  mixed.insert(mixed.end(), unsized.begin(), unsized.end());

  nx::vector<u8> both = first;
  both.insert(both.end(), second.begin(), second.end());
  for (const nx::vector<u8> *const frames : {&sized, &mixed}) {
    const auto back = nx::zstd::decompress(*frames, both.size());
    REQUIRE(back);
    CHECK(same(*back, both));
  }
}

TEST_CASE("zstd: a frame that would produce more than the limit is refused "
          "before or while it is read") {
  const nx::vector<u8> source = pattern(50'000);
  const auto sized = nx::zstd::compress(source);
  REQUIRE(sized);
  const auto refused = nx::zstd::decompress(*sized, source.size() - 1);
  REQUIRE(!refused);
  CHECK(refused.error() == nx::zstd::zstd_error::TooLarge);

  nx::zstd::compressor compressor;
  const nx::vector<u8> unsized =
      streamed(compressor, source, 8'192, nx::zstd::compressor::UNKNOWN_SIZE);
  const auto unsized_refused = nx::zstd::decompress(unsized, 1'000);
  REQUIRE(!unsized_refused);
  CHECK(unsized_refused.error() == nx::zstd::zstd_error::TooLarge);

  nx::zstd::decompressor reader;
  reader.begin(1'000);
  nx::vector<u8> out;
  const auto step = reader.feed(unsized, out);
  REQUIRE(!step);
  CHECK(step.error() == nx::zstd::zstd_error::TooLarge);
}

TEST_CASE("zstd: no frame, a damaged frame, a cut one and trailing bytes "
          "are corrupt") {
  using nx::zstd::zstd_error;
  const nx::vector<u8> source = pattern(20'000);
  const auto frame = nx::zstd::compress(source);
  REQUIRE(frame);
  nx::zstd::compressor compressor;
  const nx::vector<u8> unsized =
      streamed(compressor, source, 2'048, nx::zstd::compressor::UNKNOWN_SIZE);

  const auto corrupt = [&](const std::span<const u8> bytes) {
    const auto back = nx::zstd::decompress(bytes, source.size());
    return !back && back.error() == zstd_error::Corrupt;
  };
  CHECK(corrupt({}));
  const nx::vector<u8> garbage = pattern(64);
  CHECK(corrupt(garbage));
  for (const nx::vector<u8> *const whole : {&*frame, &unsized}) {
    CHECK(corrupt(std::span<const u8>(*whole).first(whole->size() / 2)));
    CHECK(corrupt(std::span<const u8>(*whole).first(whole->size() - 1)));
    nx::vector<u8> trailing = *whole;
    trailing.insert(trailing.end(), {7, 7, 7, 7, 7, 7, 7, 7});
    CHECK(corrupt(trailing));
  }
  // Each frame has a content checksum (RFC 8878: bit 2 of the byte after
  // the magic number), its last four bytes.
  for (const nx::vector<u8> *const whole : {&*frame, &unsized}) {
    CHECK(((*whole)[4] & 0x04u) != 0u);
    nx::vector<u8> flipped = *whole;
    flipped.back() ^= 0x01;
    CHECK(corrupt(flipped));
  }
  CHECK(nx::zstd::to_string(zstd_error::TooLarge) == "too large");
}
