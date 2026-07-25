#pragma once

/**
 * @file filesystem.h
 * @brief Path inspection (nx::fs::path_view) and filesystem I/O helpers.
 */

#include "foundation/containers/blob.h"
#include "foundation/containers/small_vector.h"
#include "foundation/core/result.h"
#include "foundation/strings/utf8_string.h"

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
  // A drive letter is absolute only when followed by a separator (C:\x);
  // bare "C:" or "C:x" is drive-relative.
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

// Lexically joins tail onto base. An absolute (or separator-rooted) tail
// replaces base; an empty tail leaves base unchanged. Windows root-name
// semantics such as "C:foo" are not applied.
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

// Lexically resolves "." and "..", collapses redundant separators and returns a
// forward-slash path. Touches no filesystem, so symlinks are not followed.
//
// An absolute path stays absolute and keeps its root name ("C:"); a relative
// one stays relative, keeping leading ".." that cannot be resolved without
// knowing the base. A path that normalizes away yields ".".
//
// Returns "" for an empty path, and for a ".." that escapes an absolute root -
// which is what makes this usable as a containment check.
[[nodiscard]] inline string normalize(const string_view p) {
  const string_view root = root_name(p);
  const string_view rest = p.substr(root.size());
  const bool absolute = !rest.empty() && is_separator(rest[0]);

  // Inline: paths are shallow, and normalize() sits on every VFS lookup.
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
        return {}; // escapes the root
      } else {
        segs.push_back(seg); // relative: nothing to pop against
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

/**
 * @brief Non-owning view over a UTF-8 filesystem path.
 *
 * Wraps a string_view and offers lexical path queries (filename, stem,
 * extension, parent_path, root_name, is_absolute) and joining via join() or
 * operator/. It does not touch the filesystem and copies nothing; the
 * referenced bytes must outlive the view.
 */
class path_view {
public:
  constexpr path_view() noexcept = default;

  constexpr path_view(const string_view s) noexcept { p_ = s; }

  constexpr path_view(const char *s) noexcept : path_view(string_view(s)) {}

  template <class A>
  path_view(const basic_string<A> &s) noexcept : path_view(string_view(s)) {}

  [[nodiscard]] constexpr string_view native() const noexcept { return p_; }
  [[nodiscard]] std::span<const std::byte> as_bytes() const noexcept {
    return std::as_bytes(std::span{p_.data(), p_.size()});
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return p_.empty(); }

  [[nodiscard]] constexpr bool is_absolute() const noexcept {
    return path::is_absolute(p_);
  }
  [[nodiscard]] constexpr bool is_relative() const noexcept {
    return path::is_relative(p_);
  }
  [[nodiscard]] constexpr path_view filename() const noexcept {
    return path::filename(p_);
  }
  [[nodiscard]] constexpr path_view stem() const noexcept {
    return path::stem(p_);
  }
  [[nodiscard]] constexpr string_view extension() const noexcept {
    return path::extension(p_);
  }
  [[nodiscard]] constexpr path_view parent_path() const noexcept {
    return path::parent_path(p_);
  }
  [[nodiscard]] constexpr string_view root_name() const noexcept {
    return path::root_name(p_);
  }
  [[nodiscard]] constexpr bool has_extension() const noexcept {
    return path::has_extension(p_);
  }
  [[nodiscard]] string join(const path_view tail) const {
    return path::join(p_, tail.native());
  }
  [[nodiscard]] string operator/(const path_view tail) const {
    return path::join(p_, tail.native());
  }

private:
  string_view p_;
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

[[nodiscard]] io_result<void> file_write(path_view path,
                                         std::span<const u8> data);
[[nodiscard]] io_result<void> file_write_text(path_view path, string_view text);

[[nodiscard]] io_result<void> create_directories(path_view path);

enum class file_type : u8 {
  none,
  not_found,
  regular,
  directory,
  symlink,
  other,
};

// status follows symlinks, symlink_status does not. not_found means nothing
// exists at path; none means the query itself failed.
[[nodiscard]] file_type status(path_view path);
[[nodiscard]] file_type symlink_status(path_view path);

[[nodiscard]] inline bool exists(path_view path) noexcept {
  const file_type t = status(path);
  return t != file_type::not_found && t != file_type::none;
}

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

// Recursively copies the entry at from to the exact path to. On POSIX symlinks
// are recreated rather than followed; on Windows file symlinks are copied as
// their target. Recursion is bounded by an internal depth limit.
[[nodiscard]] io_result<void> copy(path_view from, path_view to,
                                   bool overwrite_existing = false);

// Moves from to to, falling back to copy + remove_all across volumes.
[[nodiscard]] io_result<void> rename(path_view from, path_view to);

[[nodiscard]] io_result<void> remove(path_view path);
[[nodiscard]] io_result<void> remove_all(path_view path);

// Directory containing the running executable (no trailing separator).
[[nodiscard]] io_result<string> executable_dir();

enum class seek_origin : u8 { begin, current, end };

/**
 * @brief Read-only sequential/seekable byte stream.
 *
 * A minimal cross-platform stream interface over a file (OS handle) or an
 * in-memory buffer. Obtain one from open_file() / open_memory(); the returned
 * stream_ptr owns it and closes it on destruction.
 */
struct stream {
  stream() = default;
  virtual ~stream() = default;
  stream(const stream &) = delete;
  stream &operator=(const stream &) = delete;

  /// Reads up to @p bytes into @p dst; returns the count actually read (a
  /// short read, including 0, signals end of stream).
  virtual usize read(void *dst, usize bytes) = 0;
  /// Moves the read cursor; returns false on failure.
  virtual bool seek(i64 offset, seek_origin origin = seek_origin::begin) = 0;
  /// Current cursor position in bytes, or -1 on error.
  [[nodiscard]] virtual i64 tell() const = 0;
  /// Total size in bytes, or -1 if unknown.
  [[nodiscard]] virtual i64 size() const = 0;
  /// True once the cursor has reached the end of the stream.
  [[nodiscard]] virtual bool eof() const = 0;
};

/// Move-only owning handle to a stream; closes it on destruction.
class stream_ptr {
public:
  stream_ptr() = default;
  explicit stream_ptr(stream *s) noexcept : s_(s) {}
  ~stream_ptr() { reset(); }

  stream_ptr(stream_ptr &&o) noexcept : s_(o.s_) { o.s_ = nullptr; }
  stream_ptr &operator=(stream_ptr &&o) noexcept {
    if (this != &o) {
      reset();
      s_ = o.s_;
      o.s_ = nullptr;
    }
    return *this;
  }
  stream_ptr(const stream_ptr &) = delete;
  stream_ptr &operator=(const stream_ptr &) = delete;

  [[nodiscard]] stream *get() const noexcept { return s_; }
  stream *operator->() const noexcept { return s_; }
  stream &operator*() const noexcept { return *s_; }
  [[nodiscard]] bool valid() const noexcept { return s_ != nullptr; }
  explicit operator bool() const noexcept { return s_ != nullptr; }

  void reset() noexcept {
    if (s_) {
      release(s_);
      s_ = nullptr;
    }
  }

private:
  stream *s_ = nullptr;
};

/// Opens @p path for reading.
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