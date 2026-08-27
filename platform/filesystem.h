#pragma once

#include "core/foundation/containers/blob.h"
#include "core/foundation/containers/small_vector.h"
#include "core/foundation/core/result.h"
#include "core/foundation/strings/utf8_string.h"

#include <span>

namespace nx::fs {
namespace path {
namespace detail {

constexpr bool is_alpha(const char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

constexpr usize last_separator(const string_view p) noexcept {
  for (usize i = p.size(); i-- > 0;)
    if (p[i] == '/' || p[i] == '\\')
      return i;
  return string_view::npos;
}

constexpr bool is_dot(const string_view s) noexcept {
  return s.size() == 1 && s[0] == '.';
}

constexpr bool is_dotdot(const string_view s) noexcept {
  return s.size() == 2 && s[0] == '.' && s[1] == '.';
}

} // namespace detail

constexpr bool is_separator(const char c) noexcept {
  return c == '/' || c == '\\';
}

constexpr bool is_absolute(const string_view p) noexcept {
  if (p.empty())
    return false;
  if (is_separator(p[0]))
    return true;
  if (p.size() >= 2 && detail::is_alpha(p[0]) && p[1] == ':')
    return p.size() >= 3 && is_separator(p[2]);
  return false;
}

constexpr bool is_relative(const string_view p) noexcept {
  return !is_absolute(p);
}

constexpr string_view filename(const string_view p) noexcept {
  const usize pos = detail::last_separator(p);
  if (pos == string_view::npos)
    return p;
  return p.substr(pos + 1);
}

constexpr string_view parent_path(const string_view p) noexcept {
  const usize pos = detail::last_separator(p);
  if (pos == string_view::npos)
    return {};
  if (pos == 0)
    return p.substr(0, 1);
  return p.substr(0, pos);
}

constexpr string_view extension(const string_view p) noexcept {
  const string_view fn = filename(p);
  if (detail::is_dot(fn) || detail::is_dotdot(fn))
    return {};
  const usize pos = fn.rfind('.');
  if (pos == string_view::npos || pos == 0)
    return {};
  return fn.substr(pos);
}

constexpr string_view stem(const string_view p) noexcept {
  const string_view fn = filename(p);
  if (detail::is_dot(fn) || detail::is_dotdot(fn))
    return fn;
  const usize pos = fn.rfind('.');
  if (pos == string_view::npos || pos == 0)
    return fn;
  return fn.substr(0, pos);
}

constexpr string_view root_name(const string_view p) noexcept {
  if (p.size() >= 2 && detail::is_alpha(p[0]) && p[1] == ':')
    return p.substr(0, 2);
  return {};
}

constexpr bool has_extension(const string_view p) noexcept {
  return !extension(p).empty();
}

[[nodiscard]] inline string join(const string_view base,
                                 const string_view tail) {
  if (tail.empty())
    return string(base);
  if (base.empty() || is_absolute(tail))
    return string(tail);
  string out(base);
  if (!is_separator(out.back()))
    out.push_back(u8'/');
  out.append(tail);
  return out;
}

// An absolute path stays absolute and keeps its root name ("C:"); a relative
// one stays relative, keeping leading ".." that cannot be resolved without
// knowing the base.
[[nodiscard]] inline string normalize(const string_view p) {
  const string_view root = root_name(p);
  const string_view rest = p.substr(root.size());
  const bool absolute = !rest.empty() && is_separator(rest[0]);

  small_vector<string_view, 16> segs;
  usize i = 0;
  while (i < rest.size()) {
    while (i < rest.size() && is_separator(rest[i]))
      ++i;
    const usize start = i;
    while (i < rest.size() && !is_separator(rest[i]))
      ++i;
    if (i == start)
      continue;

    const string_view seg = rest.substr(start, i - start);
    if (detail::is_dot(seg))
      continue;
    if (detail::is_dotdot(seg)) {
      if (!segs.empty() && !detail::is_dotdot(segs.back())) {
        segs.pop_back();
      } else if (absolute) {
        return {};
      } else {
        segs.push_back(seg);
      }
      continue;
    }
    segs.push_back(seg);
  }

  string out(root);
  if (absolute)
    out.push_back(u8'/');
  for (usize k = 0; k < segs.size(); ++k) {
    if (k)
      out.push_back(u8'/');
    out.append(segs[k]);
  }
  if (out.empty())
    return p.empty() ? string() : string(".");
  return out;
}

} // namespace path

/// Non-owning view over a UTF-8 filesystem path. It does not touch the
/// filesystem and copies nothing; the referenced bytes must outlive the view.
class path_view {
public:
  constexpr path_view() noexcept = default;

  constexpr path_view(const string_view s) noexcept { m_p = s; }

  constexpr path_view(const char *s) noexcept : path_view(string_view(s)) {}

  template <class A>
  path_view(const basic_string<A> &s) noexcept : path_view(string_view(s)) {}

  [[nodiscard]] constexpr string_view native() const noexcept { return m_p; }
  [[nodiscard]] std::span<const std::byte> as_bytes() const noexcept {
    return std::as_bytes(std::span{m_p.data(), m_p.size()});
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return m_p.empty(); }

  [[nodiscard]] constexpr bool is_absolute() const noexcept {
    return path::is_absolute(m_p);
  }
  [[nodiscard]] constexpr bool is_relative() const noexcept {
    return path::is_relative(m_p);
  }
  [[nodiscard]] constexpr path_view filename() const noexcept {
    return path::filename(m_p);
  }
  [[nodiscard]] constexpr path_view stem() const noexcept {
    return path::stem(m_p);
  }
  [[nodiscard]] constexpr string_view extension() const noexcept {
    return path::extension(m_p);
  }
  [[nodiscard]] constexpr path_view parent_path() const noexcept {
    return path::parent_path(m_p);
  }
  [[nodiscard]] constexpr string_view root_name() const noexcept {
    return path::root_name(m_p);
  }
  [[nodiscard]] constexpr bool has_extension() const noexcept {
    return path::has_extension(m_p);
  }
  [[nodiscard]] string join(const path_view tail) const {
    return path::join(m_p, tail.native());
  }
  [[nodiscard]] string operator/(const path_view tail) const {
    return path::join(m_p, tail.native());
  }

private:
  string_view m_p;
};

enum class io_error : u32 {
  Unknown = 0,
  NotFound,
  PermissionDenied,
  AlreadyExists,
  IsADirectory,
  NotADirectory,
  NoSpace,
  FileTooLarge,
  InvalidArgument,
  Interrupted,
  OpenFailed,
  ReadFailed,
  WriteFailed,
  Count,
};

struct file_error {
  io_error kind = io_error::Unknown;
  i32 native = 0;
};

[[nodiscard]] constexpr string_view to_string(io_error e) noexcept {
  switch (e) {
  case io_error::NotFound:
    return "file not found";
  case io_error::PermissionDenied:
    return "permission denied";
  case io_error::AlreadyExists:
    return "already exists";
  case io_error::IsADirectory:
    return "is a directory";
  case io_error::NotADirectory:
    return "not a directory";
  case io_error::NoSpace:
    return "no space left on device";
  case io_error::FileTooLarge:
    return "file too large";
  case io_error::InvalidArgument:
    return "invalid argument";
  case io_error::Interrupted:
    return "interrupted";
  case io_error::OpenFailed:
    return "open failed";
  case io_error::ReadFailed:
    return "read failed";
  case io_error::WriteFailed:
    return "write failed";
  default:
    return "unknown error";
  }
}

template <typename T> using io_result = result<T, file_error>;

[[nodiscard]] io_result<blob<u8>> file_read(path_view path);
[[nodiscard]] io_result<string> file_read_text(path_view path);

/// Stages a file beside its destination and publishes it with one rename.
///
/// Write the complete output to temporary_path(), then call commit(). If the
/// object is destroyed first, the temporary file is removed and any existing
/// destination is left untouched. The destination's parent must already
/// exist. Publication is atomic to readers on supported host filesystems, but
/// does not imply crash-durable storage flushing.
class [[nodiscard]] staged_file {
public:
  explicit staged_file(path_view destination);
  ~staged_file();

  staged_file(const staged_file &) = delete;
  staged_file &operator=(const staged_file &) = delete;
  staged_file(staged_file &&) = delete;
  staged_file &operator=(staged_file &&) = delete;

  [[nodiscard]] path_view temporary_path() const noexcept {
    return m_temporary.view();
  }
  [[nodiscard]] io_result<void> commit();

private:
  string m_destination;
  string m_temporary;
};

[[nodiscard]] io_result<void> file_write(path_view path,
                                         std::span<const u8> data);
[[nodiscard]] io_result<void> file_write_text(path_view path, string_view text);
/// Atomically replaces the destination after the complete payload is written.
[[nodiscard]] io_result<void> file_write_atomic(path_view path,
                                                std::span<const u8> data);
/// Text equivalent of file_write_atomic().
[[nodiscard]] io_result<void> file_write_text_atomic(path_view path,
                                                     string_view text);

[[nodiscard]] io_result<void> create_directories(path_view path);

enum class file_type : u8 {
  none,
  not_found,
  regular,
  directory,
  symlink,
  other,
};

[[nodiscard]] file_type status(path_view path);
[[nodiscard]] file_type symlink_status(path_view path);

[[nodiscard]] inline bool exists(path_view path) noexcept {
  const file_type t = status(path);
  return t != file_type::not_found && t != file_type::none;
}

struct file_stat {
  file_type type = file_type::none;
  u64 size = 0;
  /// Last write, in nanoseconds since the Unix epoch. 0 when the platform
  /// would not say. Comparable between calls, which is what watching a file
  /// for a change needs; not a wall clock.
  u64 mtime_ns = 0;
};

[[nodiscard]] file_stat stat_file(path_view path);

struct directory_entry {
  string path;
  file_type type = file_type::none;
};

[[nodiscard]] io_result<vector<directory_entry>>
directory_entries(path_view root);

[[nodiscard]] io_result<vector<directory_entry>>
recursive_directory_entries(path_view root);

[[nodiscard]] io_result<void> copy_file(path_view from, path_view to,
                                        bool overwrite_existing = false);
/// Copies to a staged sibling before atomically replacing @p to.
[[nodiscard]] io_result<void> copy_file_atomic(path_view from, path_view to);

[[nodiscard]] io_result<void> copy(path_view from, path_view to,
                                   bool overwrite_existing = false);

[[nodiscard]] io_result<void> rename(path_view from, path_view to);

[[nodiscard]] io_result<void> remove(path_view path);
[[nodiscard]] io_result<void> remove_all(path_view path);

[[nodiscard]] io_result<string> executable_dir();
/// Returns the platform-standard per-user data directory for an nx2d
/// application. The directory is not created by this function.
[[nodiscard]] io_result<string> application_data_dir(string_view application);

enum class seek_origin : u8 { begin, current, end };

/// Read-only sequential/seekable byte stream. A minimal cross-platform stream
/// interface over a file (OS handle) or an in-memory buffer. Obtain one from
/// open_file() / open_memory(); the returned stream_ptr owns it and closes it
/// on destruction.
struct stream {
  stream() = default;
  virtual ~stream() = default;
  stream(const stream &) = delete;
  stream &operator=(const stream &) = delete;

  /// Reads up to @p bytes into @p dst; returns the count actually read (a
  /// short read, including 0, signals end of stream).
  virtual usize read(void *dst, usize bytes) = 0;
  virtual bool seek(i64 offset, seek_origin origin = seek_origin::begin) = 0;
  /// Current cursor position in bytes, or -1 on error.
  [[nodiscard]] virtual i64 tell() const = 0;
  /// Total size in bytes, or -1 if unknown.
  [[nodiscard]] virtual i64 size() const = 0;
  [[nodiscard]] virtual bool eof() const = 0;
};

class stream_ptr {
public:
  stream_ptr() = default;
  explicit stream_ptr(stream *s) noexcept : m_s(s) {}
  ~stream_ptr() { reset(); }

  stream_ptr(stream_ptr &&o) noexcept : m_s(o.m_s) { o.m_s = nullptr; }
  stream_ptr &operator=(stream_ptr &&o) noexcept {
    if (this != &o) {
      reset();
      m_s = o.m_s;
      o.m_s = nullptr;
    }
    return *this;
  }
  stream_ptr(const stream_ptr &) = delete;
  stream_ptr &operator=(const stream_ptr &) = delete;

  [[nodiscard]] stream *get() const noexcept { return m_s; }
  stream *operator->() const noexcept { return m_s; }
  stream &operator*() const noexcept { return *m_s; }
  [[nodiscard]] bool valid() const noexcept { return m_s != nullptr; }
  explicit operator bool() const noexcept { return m_s != nullptr; }

  void reset() noexcept {
    if (m_s) {
      release(m_s);
      m_s = nullptr;
    }
  }

private:
  stream *m_s = nullptr;
};

[[nodiscard]] io_result<stream_ptr> open_file(path_view path);
/// Wraps an existing read-only buffer (not copied; must outlive the stream).
[[nodiscard]] io_result<stream_ptr> open_memory(const void *data, usize size);

} // namespace nx::fs

// Formats a path_view as its UTF-8 string representation with no copy.
template <>
struct std::formatter<nx::fs::path_view> : std::formatter<std::string_view> {
  auto format(const nx::fs::path_view p, std::format_context &ctx) const {
    return std::formatter<std::string_view>::format(p.native(), ctx);
  }
};
