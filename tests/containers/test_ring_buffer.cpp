#include "framework/nxtest.h"

#include "core/foundation/containers/ring_buffer.h"

#include <numeric>
#include <thread>
#include <vector>

namespace {

usize write_seq(nx::spsc::Ring<u32> &ring, const u32 first, const usize count) {
  std::vector<u32> src(count);
  std::iota(src.begin(), src.end(), first);
  return ring.write(src.data(), src.size());
}

}

TEST_CASE("ring_buffer: a default-constructed ring is invalid and empty") {
  nx::spsc::Ring<u32> ring;
  CHECK_FALSE(ring.valid());
  CHECK(ring.capacity() == 0u);
  CHECK(ring.size() == 0u);
  CHECK(ring.empty());
}

TEST_CASE("ring_buffer: capacity is rounded up to a power of two") {
  nx::spsc::Ring<u32> ring;
  REQUIRE(ring.reset(100));
  CHECK(ring.capacity() == 128u);
  CHECK(ring.valid());
  CHECK(ring.space() == 128u);

  REQUIRE(ring.reset(64));
  CHECK(ring.capacity() == 64u);
}

TEST_CASE("ring_buffer: a capacity of zero is refused") {
  nx::spsc::Ring<u32> ring;
  CHECK_FALSE(ring.reset(0));
  CHECK_FALSE(ring.valid());
}

TEST_CASE("ring_buffer: reset drops whatever was buffered") {
  nx::spsc::Ring<u32> ring;
  REQUIRE(ring.reset(16));
  CHECK(write_seq(ring, 0, 8) == 8u);
  CHECK(ring.size() == 8u);

  REQUIRE(ring.reset(16));
  CHECK(ring.size() == 0u);
  CHECK(ring.space() == 16u);
}

TEST_CASE("ring_buffer: a write is read back in order") {
  nx::spsc::Ring<u32> ring;
  REQUIRE(ring.reset(16));
  CHECK(write_seq(ring, 100, 10) == 10u);
  CHECK(ring.size() == 10u);
  CHECK(ring.space() == 6u);

  u32 out[10] = {};
  CHECK(ring.read(out, 10) == 10u);
  for (u32 i = 0; i < 10; ++i)
    CHECK(out[i] == 100u + i);
  CHECK(ring.empty());
}

TEST_CASE("ring_buffer: size and space always sum to the capacity") {
  nx::spsc::Ring<u32> ring;
  REQUIRE(ring.reset(32));
  for (u32 n = 0; n < 32; ++n) {
    CHECK(ring.size() + ring.space() == ring.capacity());
    CHECK(write_seq(ring, n, 1) == 1u);
  }
  CHECK(ring.size() + ring.space() == ring.capacity());
  CHECK(ring.full());
}

TEST_CASE("ring_buffer: a write past the end takes what fits and reports it") {
  nx::spsc::Ring<u32> ring;
  REQUIRE(ring.reset(8));
  CHECK(write_seq(ring, 0, 6) == 6u);
  CHECK(write_seq(ring, 6, 4) == 2u);
  CHECK(ring.full());
  CHECK(write_seq(ring, 99, 1) == 0u);

  u32 out[8] = {};
  CHECK(ring.read(out, 8) == 8u);
  for (u32 i = 0; i < 8; ++i)
    CHECK(out[i] == i);
}

TEST_CASE("ring_buffer: a read past the end takes what is there") {
  nx::spsc::Ring<u32> ring;
  REQUIRE(ring.reset(8));
  CHECK(write_seq(ring, 0, 3) == 3u);

  u32 out[8] = {0xffffffffu, 0xffffffffu, 0xffffffffu,
                0xffffffffu, 0xffffffffu, 0xffffffffu};
  CHECK(ring.read(out, 8) == 3u);
  CHECK(out[0] == 0u);
  CHECK(out[1] == 1u);
  CHECK(out[2] == 2u);
  CHECK(out[3] == 0xffffffffu);
  CHECK(ring.read(out, 8) == 0u);
}

TEST_CASE("ring_buffer: a write that wraps the end reads back contiguous") {
  nx::spsc::Ring<u32> ring;
  REQUIRE(ring.reset(8));
  CHECK(write_seq(ring, 0, 6) == 6u);

  u32 drain[4] = {};
  CHECK(ring.read(drain, 4) == 4u);
  CHECK(write_seq(ring, 6, 6) == 6u);
  CHECK(ring.size() == 8u);

  u32 out[8] = {};
  CHECK(ring.read(out, 8) == 8u);
  for (u32 i = 0; i < 8; ++i)
    CHECK(out[i] == 4u + i);
}

TEST_CASE("ring_buffer: many wraps preserve order") {
  nx::spsc::Ring<u32> ring;
  REQUIRE(ring.reset(8));
  u32 next_written = 0;
  u32 next_expected = 0;

  for (u32 round = 0; round < 50; ++round) {
    const usize chunk = 1 + (round % 5);
    CHECK(write_seq(ring, next_written, chunk) == chunk);
    next_written += nx::cast<u32>(chunk);

    u32 out[8] = {};
    const usize got = ring.read(out, chunk);
    CHECK(got == chunk);
    for (usize i = 0; i < got; ++i)
      CHECK(out[i] == next_expected + nx::cast<u32>(i));
    next_expected += nx::cast<u32>(got);
  }
  CHECK(next_expected == next_written);
}

TEST_CASE("ring_buffer: discard drops elements without copying them") {
  nx::spsc::Ring<u32> ring;
  REQUIRE(ring.reset(16));
  CHECK(write_seq(ring, 0, 10) == 10u);
  CHECK(ring.discard(4) == 4u);
  CHECK(ring.size() == 6u);

  u32 out[6] = {};
  CHECK(ring.read(out, 6) == 6u);
  CHECK(out[0] == 4u);
  CHECK(out[5] == 9u);
}

TEST_CASE("ring_buffer: discard is clamped to what is buffered") {
  nx::spsc::Ring<u32> ring;
  REQUIRE(ring.reset(16));
  CHECK(write_seq(ring, 0, 3) == 3u);
  CHECK(ring.discard(100) == 3u);
  CHECK(ring.empty());
}

TEST_CASE("ring_buffer: clear empties without disturbing the capacity") {
  nx::spsc::Ring<u32> ring;
  REQUIRE(ring.reset(16));
  CHECK(write_seq(ring, 0, 12) == 12u);
  ring.clear();
  CHECK(ring.empty());
  CHECK(ring.space() == 16u);
  CHECK(ring.capacity() == 16u);

  CHECK(write_seq(ring, 77, 2) == 2u);
  u32 out[2] = {};
  CHECK(ring.read(out, 2) == 2u);
  CHECK(out[0] == 77u);
}

TEST_CASE("ring_buffer: carries floats and plain aggregates") {
  nx::spsc::Ring<f32> samples;
  REQUIRE(samples.reset(4));
  const f32 in[4] = {-1.f, -0.5f, 0.5f, 1.f};
  CHECK(samples.write(in, 4) == 4u);
  f32 out[4] = {};
  CHECK(samples.read(out, 4) == 4u);
  CHECK(out[0] == -1.f);
  CHECK(out[3] == 1.f);

  struct Frame {
    f32 left;
    f32 right;
  };
  nx::spsc::Ring<Frame> frames;
  REQUIRE(frames.reset(4));
  const Frame f[2] = {{1.f, 2.f}, {3.f, 4.f}};
  CHECK(frames.write(f, 2) == 2u);
  Frame got[2] = {};
  CHECK(frames.read(got, 2) == 2u);
  CHECK(got[1].left == 3.f);
  CHECK(got[1].right == 4.f);
}

TEST_CASE("ring_buffer: a producer and a consumer transfer a stream intact") {
  constexpr u32 TOTAL = 1u << 20;
  constexpr usize CHUNK = 64;

  nx::spsc::Ring<u32> ring;
  REQUIRE(ring.reset(1024));

  std::thread producer([&ring] {
    u32 next = 0;
    u32 buffer[CHUNK] = {};
    while (next < TOTAL) {
      const usize want = nx::min<usize>(CHUNK, TOTAL - next);
      for (usize i = 0; i < want; ++i)
        buffer[i] = next + nx::cast<u32>(i);
      usize written = 0;
      while (written < want) {
        const usize n = ring.write(buffer + written, want - written);
        if (n == 0)
          std::this_thread::yield();
        written += n;
      }
      next += nx::cast<u32>(want);
    }
  });

  u32 expected = 0;
  bool ordered = true;
  u32 buffer[CHUNK] = {};
  while (expected < TOTAL) {
    const usize n = ring.read(buffer, CHUNK);
    if (n == 0) {
      std::this_thread::yield();
      continue;
    }
    for (usize i = 0; i < n; ++i)
      if (buffer[i] != expected + nx::cast<u32>(i))
        ordered = false;
    expected += nx::cast<u32>(n);
  }
  producer.join();

  CHECK(ordered);
  CHECK(expected == TOTAL);
  CHECK(ring.empty());
}

TEST_CASE("ring_buffer: a slow consumer never loses or reorders elements") {
  constexpr u32 TOTAL = 1u << 16;

  nx::spsc::Ring<u32> ring;
  REQUIRE(ring.reset(16));

  std::thread producer([&ring] {
    for (u32 v = 0; v < TOTAL;) {
      if (ring.write(&v, 1) == 1)
        ++v;
      else
        std::this_thread::yield();
    }
  });

  u32 expected = 0;
  bool ordered = true;
  while (expected < TOTAL) {
    u32 value = 0;
    if (ring.read(&value, 1) == 1) {
      if (value != expected)
        ordered = false;
      ++expected;
    }
  }
  producer.join();

  CHECK(ordered);
  CHECK(expected == TOTAL);
}

TEST_CASE("spsc ring: a capacity whose bytes overflow is refused") {
  nx::spsc::Ring<u64> ring;
  CHECK_FALSE(ring.reset(~usize{0}));
  CHECK_FALSE(ring.valid());
  CHECK_FALSE(ring.reset(usize{1} << (sizeof(usize) * 8 - 2)));
  CHECK_FALSE(ring.valid());
  CHECK(ring.capacity() == 0u);
  REQUIRE(ring.reset(5));
  CHECK(ring.capacity() == 8u);
}
