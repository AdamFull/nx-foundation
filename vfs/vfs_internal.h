#pragma once

/**
 * @file vfs_internal.h
 * @brief Bits shared by the device implementations. Not a public header.
 */

#include "core/foundation/containers/hash_map.h"
#include "core/foundation/vfs/vfs.h"

#include <cstring>

namespace nx::vfs::detail {

/// Canonical virtual path: always absolute, so a bare "shaders/x" means
/// "/shaders/x". "" if it escapes the root, or is not a virtual path at all
/// (a drive-rooted "C:/x" normalizes to itself, never under '/').
[[nodiscard]] inline nx::string canon_vpath(const path_view v) {
  nx::string s = nx::fs::path::normalize(nx::fs::path::join("/", v.native()));
  if (!s.empty() && s.front() != '/')
    return {};
  return s;
}

/// canon_vpath() with the leading '/' stripped: "" for the device root.
[[nodiscard]] inline nx::string canon_rel(const path_view rel) {
  nx::string s = canon_vpath(rel);
  if (!s.empty() && s.front() == '/')
    s.erase(0, 1);
  return s;
}

/// Read-only stream that owns its bytes, so it survives the source entry being
/// replaced or removed - unlike nx::fs::open_memory, which only aliases.
class BlobStream final : public nx::fs::stream {
public:
  explicit BlobStream(nx::blob<u8> data) : m_data(std::move(data)) {}

  usize read(void *dst, usize bytes) override {
    const usize n = nx::min(bytes, m_data.size() - m_cursor);
    if (n)
      std::memcpy(dst, m_data.data() + m_cursor, n);
    m_cursor += n;
    return n;
  }

  bool seek(i64 offset, nx::fs::seek_origin origin) override {
    const i64 base = origin == nx::fs::seek_origin::begin ? 0
                     : origin == nx::fs::seek_origin::current
                         ? nx::cast<i64>(m_cursor)
                         : nx::cast<i64>(m_data.size());
    const i64 pos = base + offset;
    if (pos < 0 || pos > nx::cast<i64>(m_data.size()))
      return false;
    m_cursor = nx::cast<usize>(pos);
    return true;
  }

  [[nodiscard]] i64 tell() const override { return nx::cast<i64>(m_cursor); }
  [[nodiscard]] i64 size() const override {
    return nx::cast<i64>(m_data.size());
  }
  [[nodiscard]] bool eof() const override { return m_cursor >= m_data.size(); }

private:
  nx::blob<u8> m_data;
  usize m_cursor = 0;
};

/// Dedups directory children: the flat-key devices (memory, zip) rediscover a
/// subdirectory once per file under it. Keyed by name, not by hash - a
/// collision would silently drop a file, and listing is not a hot path.
class ChildSet {
public:
  /// True the first time @p name is offered, false for every repeat.
  [[nodiscard]] bool insert(const nx::string_view name) {
    return m_seen.try_emplace(nx::string(name), u8{0}).second;
  }

private:
  nx::hash_map<nx::string, u8> m_seen;
};

} // namespace nx::vfs::detail
