#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

#include <cstring>
#include <optional>
#include <span>
#include <utility>

namespace nx::nva {

/// "NVA1" as a little-endian u32.
inline constexpr u32 MAGIC = 0x3141564Eu;

/// A four-character code packed into a u32 in file (little-endian) byte order.
[[nodiscard]] constexpr u32 fourcc(const char a, const char b, const char c,
                                   const char d) noexcept {
  return nx::cast<u32>(nx::cast<u8>(a)) |
         (nx::cast<u32>(nx::cast<u8>(b)) << 8) |
         (nx::cast<u32>(nx::cast<u8>(c)) << 16) |
         (nx::cast<u32>(nx::cast<u8>(d)) << 24);
}

/// Little-endian byte writer for a chunk payload.
class Writer {
public:
  void u8v(const u8 v) { m_buf.push_back(v); }
  void u16v(const u16 v) {
    u8v(nx::cast<u8>(v));
    u8v(nx::cast<u8>(v >> 8));
  }
  void u32v(const u32 v) {
    u16v(nx::cast<u16>(v));
    u16v(nx::cast<u16>(v >> 16));
  }
  void u64v(const u64 v) {
    u32v(nx::cast<u32>(v));
    u32v(nx::cast<u32>(v >> 32));
  }
  void f32v(const f32 v) {
    u32 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    u32v(bits);
  }
  /// A u32 length then the bytes.
  void str(const nx::string_view s) {
    u32v(nx::cast<u32>(s.size()));
    for (const char c : s)
      u8v(nx::cast<u8>(c));
  }
  void raw(const std::span<const u8> b) {
    for (const u8 x : b)
      m_buf.push_back(x);
  }

  [[nodiscard]] std::span<const u8> span() const {
    return {m_buf.data(), m_buf.size()};
  }
  [[nodiscard]] nx::vector<u8> take() { return std::move(m_buf); }

private:
  nx::vector<u8> m_buf;
};

/// Little-endian byte reader over a chunk payload. Bounds-checked: a read past
/// the end leaves ok() false and returns zeroes, so a truncated file fails
/// rather than reads out of bounds.
class Reader {
public:
  explicit Reader(const std::span<const u8> b) : m_b(b) {}

  u8 u8v() {
    if (m_pos + 1 > m_b.size()) {
      m_ok = false;
      return 0;
    }
    return m_b[m_pos++];
  }
  u16 u16v() {
    const u16 lo = u8v();
    const u16 hi = u8v();
    return nx::cast<u16>(lo | (hi << 8));
  }
  u32 u32v() {
    const u32 lo = u16v();
    const u32 hi = u16v();
    return lo | (hi << 16);
  }
  u64 u64v() {
    const u64 lo = u32v();
    const u64 hi = u32v();
    return lo | (hi << 32);
  }
  f32 f32v() {
    const u32 bits = u32v();
    f32 v = 0.f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
  /// A view into the reader's bytes, valid as long as they are.
  [[nodiscard]] nx::string_view str() {
    const u32 n = u32v();
    if (!m_ok || m_pos + n > m_b.size()) {
      m_ok = false;
      return {};
    }
    const nx::string_view s(reinterpret_cast<const char *>(m_b.data() + m_pos),
                            n);
    m_pos += n;
    return s;
  }

  /// The next @p n bytes as a view, advancing past them - for a chunk that nests
  /// another container's bytes. Empty and not-ok past the end.
  [[nodiscard]] std::span<const u8> raw(const usize n) {
    if (!m_ok || m_pos + n > m_b.size()) {
      m_ok = false;
      return {};
    }
    const std::span<const u8> s = m_b.subspan(m_pos, n);
    m_pos += n;
    return s;
  }

  [[nodiscard]] bool ok() const noexcept { return m_ok; }

private:
  std::span<const u8> m_b;
  usize m_pos = 0;
  bool m_ok = true;
};

class ChunkWriter {
public:
  ChunkWriter(const u32 format, const u16 version, const u16 flags = 0) noexcept
      : m_format(format), m_version(version), m_flags(flags) {}

  void add(const u32 id, const std::span<const u8> data) {
    m_chunks.push_back({id, nx::vector<u8>(data.begin(), data.end())});
  }

  [[nodiscard]] nx::vector<u8> finish() const {
    constexpr usize header = 16;
    usize offset = header + m_chunks.size() * 12;
    nx::vector<u32> offsets;
    offsets.reserve(m_chunks.size());
    for (const Chunk &c : m_chunks) {
      offset = (offset + 15) & ~usize(15);
      offsets.push_back(nx::cast<u32>(offset));
      offset += c.data.size();
    }

    Writer w;
    w.u32v(MAGIC);
    w.u32v(m_format);
    w.u16v(m_version);
    w.u16v(m_flags);
    w.u32v(nx::cast<u32>(m_chunks.size()));
    for (usize i = 0; i < m_chunks.size(); ++i) {
      w.u32v(m_chunks[i].id);
      w.u32v(offsets[i]);
      w.u32v(nx::cast<u32>(m_chunks[i].data.size()));
    }

    nx::vector<u8> out = w.take();
    for (usize i = 0; i < m_chunks.size(); ++i) {
      out.resize(offsets[i], 0); // pad to the aligned offset
      out.insert(out.end(), m_chunks[i].data.begin(), m_chunks[i].data.end());
    }
    return out;
  }

private:
  struct Chunk {
    u32 id;
    nx::vector<u8> data;
  };
  u32 m_format;
  u16 m_version;
  u16 m_flags;
  nx::vector<Chunk> m_chunks;
};

/// A validated read-only view over a container's bytes. The bytes must outlive
/// it; chunk() returns sub-spans into them.
class View {
public:
  [[nodiscard]] static std::optional<View> open(const std::span<const u8> bytes,
                                                const u32 expect_format = 0) {
    if (bytes.size() < 16)
      return std::nullopt;
    if (u32_at(bytes, 0) != MAGIC)
      return std::nullopt;
    const u32 format = u32_at(bytes, 4);
    if (expect_format != 0 && format != expect_format)
      return std::nullopt;
    const u32 count = u32_at(bytes, 12);
    if (16 + usize(count) * 12 > bytes.size())
      return std::nullopt;
    for (u32 i = 0; i < count; ++i) {
      const usize base = 16 + usize(i) * 12;
      if (usize(u32_at(bytes, base + 4)) + u32_at(bytes, base + 8) > bytes.size())
        return std::nullopt;
    }
    View v;
    v.m_bytes = bytes;
    v.m_format = format;
    v.m_version = nx::cast<u16>(u32_at(bytes, 8) & 0xffffu);
    v.m_flags = nx::cast<u16>(u32_at(bytes, 8) >> 16);
    v.m_count = count;
    return v;
  }

  [[nodiscard]] u32 format() const noexcept { return m_format; }
  [[nodiscard]] u16 version() const noexcept { return m_version; }
  [[nodiscard]] u16 flags() const noexcept { return m_flags; }

  [[nodiscard]] std::span<const u8> chunk(const u32 id) const {
    for (u32 i = 0; i < m_count; ++i) {
      const usize base = 16 + usize(i) * 12;
      if (u32_at(m_bytes, base) == id)
        return m_bytes.subspan(u32_at(m_bytes, base + 4),
                               u32_at(m_bytes, base + 8));
    }
    return {};
  }

private:
  [[nodiscard]] static u32 u32_at(const std::span<const u8> b, const usize p) {
    return u32(b[p]) | (u32(b[p + 1]) << 8) | (u32(b[p + 2]) << 16) |
           (u32(b[p + 3]) << 24);
  }

  std::span<const u8> m_bytes;
  u32 m_format = 0;
  u16 m_version = 0;
  u16 m_flags = 0;
  u32 m_count = 0;
};

}
