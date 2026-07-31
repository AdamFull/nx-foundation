#include "core/foundation/platform/filesystem.h"
#include "core/foundation/core/scope_guard.h"

#include <optional>

#if defined(NX_PLATFORM_WINDOWS)
#include <windows.h>
#elif defined(NX_PLATFORM_POSIX)
#include <cerrno>
#include <climits>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(NX_PLATFORM_APPLE)
#include <mach-o/dyld.h> // _NSGetExecutablePath
#endif
#else
#error "nx::fs: unsupported platform"
#endif

namespace nx::fs {
namespace {

constexpr usize kChunk = 1u << 16;
constexpr i32 kMaxCopyDepth = 1024;

struct byte_buf {
  using A = Allocator<u8>;
  using AT = std::allocator_traits<A>;

  NX_NO_UNIQUE_ADDRESS A a;
  u8 *p = nullptr;
  usize cap = 0;
  usize len = 0;

  byte_buf() = default;
  byte_buf(const byte_buf &) = delete;
  byte_buf &operator=(const byte_buf &) = delete;
  ~byte_buf() {
    if (p)
      AT::deallocate(a, p, cap);
  }

  void reserve(const usize need) {
    if (need <= cap)
      return;
    usize ncap = cap ? cap : kChunk;
    while (ncap < need)
      ncap += ncap / 2 + 1;
    u8 *np = AT::allocate(a, ncap);
    if (len)
      std::memcpy(np, p, len);
    if (p)
      AT::deallocate(a, p, cap);
    p = np;
    cap = ncap;
  }

  void fit() {
    if (cap == len)
      return;
    if (len == 0) {
      if (p)
        AT::deallocate(a, p, cap);
      p = nullptr;
      cap = 0;
      return;
    }
    u8 *np = AT::allocate(a, len);
    std::memcpy(np, p, len);
    AT::deallocate(a, p, cap);
    p = np;
    cap = len;
  }

  blob<u8> into_blob() {
    fit();
    blob<u8> b = blob<u8>::from_owned(p, len);
    p = nullptr;
    cap = 0;
    len = 0;
    return b;
  }
};

#if defined(NX_PLATFORM_WINDOWS)

file_error map_last_error(const io_error fallback) noexcept {
  const DWORD e = ::GetLastError();
  io_error kind = fallback;
  switch (e) {
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
  case ERROR_INVALID_DRIVE:
    kind = io_error::NotFound;
    break;
  case ERROR_ACCESS_DENIED:
  case ERROR_SHARING_VIOLATION:
    kind = io_error::PermissionDenied;
    break;
  case ERROR_FILE_EXISTS:
  case ERROR_ALREADY_EXISTS:
    kind = io_error::AlreadyExists;
    break;
  case ERROR_DISK_FULL:
    kind = io_error::NoSpace;
    break;
  case ERROR_INVALID_NAME:
  case ERROR_BAD_PATHNAME:
    kind = io_error::InvalidArgument;
    break;
  default:
    break;
  }
  return file_error{kind, nx::cast<i32>(e)};
}

struct handle_guard {
  HANDLE h = INVALID_HANDLE_VALUE;
  ~handle_guard() {
    if (h != INVALID_HANDLE_VALUE)
      ::CloseHandle(h);
  }
  [[nodiscard]] HANDLE get() const noexcept { return h; }
  HANDLE release() noexcept {
    HANDLE t = h;
    h = INVALID_HANDLE_VALUE;
    return t;
  }
};

// Converts a UTF-8 path to a null-terminated UTF-16 path. Uses the stack for
// the common case and heaps only for very long paths.
struct wide_path {
  wchar_t stack[512];
  vector<wchar_t> heap;
  wchar_t *ptr = nullptr;
  bool ok = false;

  explicit wide_path(const path_view path) {
    if (path.empty()) {
      stack[0] = L'\0';
      ptr = stack;
      ok = true;
      return;
    }
    const auto path_str = path.native();
    const i32 need =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path_str.data(),
                              nx::cast<i32>(path_str.size()), nullptr, 0);
    if (need <= 0)
      return;
    wchar_t *dst;
    if (nx::cast<usize>(need) + 1 <= std::size(stack)) {
      dst = stack;
    } else {
      heap.resize(nx::cast<usize>(need) + 1);
      dst = heap.data();
    }
    const i32 wrote =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path_str.data(),
                              nx::cast<i32>(path_str.size()), dst, need);
    if (wrote != need)
      return;
    dst[need] = L'\0';
    ptr = dst;
    ok = true;
  }
};

#elif defined(NX_PLATFORM_POSIX)

file_error map_errno(io_error fallback) noexcept {
  const i32 e = errno;
  io_error kind = fallback;
  switch (e) {
  case ENOENT:
    kind = io_error::NotFound;
    break;
  case EACCES:
  case EPERM:
    kind = io_error::PermissionDenied;
    break;
  case EEXIST:
    kind = io_error::AlreadyExists;
    break;
  case EISDIR:
    kind = io_error::IsADirectory;
    break;
  case ENOTDIR:
    kind = io_error::NotADirectory;
    break;
  case ENOSPC:
    kind = io_error::NoSpace;
    break;
  case EFBIG:
    kind = io_error::FileTooLarge;
    break;
  case EINVAL:
  case ENAMETOOLONG:
    kind = io_error::InvalidArgument;
    break;
  default:
    break;
  }
  return file_error{kind, nx::cast<i32>(e)};
}

struct fd_guard {
  i32 fd = -1;
  ~fd_guard() {
    if (fd >= 0)
      ::close(fd);
  }
  i32 get() const noexcept { return fd; }
  i32 release() noexcept {
    i32 t = fd;
    fd = -1;
    return t;
  }
};

// open() needs a null-terminated path; the view may not be terminated.
struct cpath {
  char stack[1024];
  vector<char> heap;
  const char *ptr = nullptr;

  explicit cpath(path_view path) {
    const string_view native = path.native();
    char *dst;
    if (native.size() + 1 <= sizeof(stack))
      dst = stack;
    else {
      heap.resize(native.size() + 1);
      dst = heap.data();
    }
    std::memcpy(dst, native.data(), native.size());
    dst[native.size()] = '\0';
    ptr = dst;
  }
};

i32 open_retry(const char *path, i32 flags, mode_t mode) noexcept {
  for (;;) {
    const i32 fd = ::open(path, flags, mode);
    if (fd < 0 && errno == EINTR)
      continue;
    return fd;
  }
}

#endif

} // namespace

#if defined(NX_PLATFORM_WINDOWS)

io_result<blob<u8>> file_read(const path_view path) {
  const wide_path wp(path);
  if (!wp.ok)
    return err(file_error{io_error::InvalidArgument, 0});

  handle_guard g{::CreateFileW(wp.ptr, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (g.get() == INVALID_HANDLE_VALUE)
    return err(map_last_error(io_error::OpenFailed));

  BY_HANDLE_FILE_INFORMATION info;
  if (::GetFileInformationByHandle(g.get(), &info) &&
      (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    return err(file_error{io_error::IsADirectory, 0});

  byte_buf buf;
  LARGE_INTEGER sz;
  if (::GetFileSizeEx(g.get(), &sz) && sz.QuadPart > 0)
    buf.reserve(nx::cast<usize>(sz.QuadPart));

  for (;;) {
    buf.reserve(buf.len + kChunk);
    const usize room = buf.cap - buf.len;
    const DWORD want = nx::cast<DWORD>(min<usize>(room, 1u << 30));
    DWORD got = 0;
    if (!::ReadFile(g.get(), buf.p + buf.len, want, &got, nullptr))
      return err(map_last_error(io_error::ReadFailed));
    if (got == 0)
      break;
    buf.len += got;
  }

  return buf.into_blob();
}

io_result<void> file_write(const path_view path,
                           const std::span<const u8> data) {
  const wide_path wp(path);
  if (!wp.ok)
    return err(file_error{io_error::InvalidArgument, 0});

  handle_guard g{::CreateFileW(wp.ptr, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (g.get() == INVALID_HANDLE_VALUE)
    return err(map_last_error(io_error::OpenFailed));

  usize total = 0;
  while (total < data.size()) {
    const usize remaining = data.size() - total;
    const DWORD want = nx::cast<DWORD>(min<usize>(remaining, 1u << 30));
    DWORD wrote = 0;
    if (!::WriteFile(g.get(), data.data() + total, want, &wrote, nullptr))
      return err(map_last_error(io_error::WriteFailed));
    total += wrote;
  }

  if (!::CloseHandle(g.release()))
    return err(map_last_error(io_error::WriteFailed));
  return {};
}

io_result<void> create_directories(const path_view path) {
  const wide_path wp(path);
  if (!wp.ok)
    return err(file_error{io_error::InvalidArgument, 0});

  const auto make = [](const wchar_t *p) -> io_result<void> {
    if (!p[0])
      return {};
    if (::CreateDirectoryW(p, nullptr))
      return {};
    const DWORD e = ::GetLastError();
    if (e == ERROR_ALREADY_EXISTS || e == ERROR_ACCESS_DENIED)
      return {};
    return err(map_last_error(io_error::OpenFailed));
  };

  for (wchar_t *s = wp.ptr; *s; ++s) {
    if (*s == L'/' || *s == L'\\') {
      const wchar_t saved = *s;
      *s = L'\0';
      auto r = make(wp.ptr);
      *s = saved;
      if (!r)
        return r;
    }
  }
  return make(wp.ptr);
}

namespace {

bool append_wide_utf8(string &out, const wchar_t *w, i32 wlen) {
  if (wlen == 0)
    return true;
  const i32 need =
      ::WideCharToMultiByte(CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
  if (need <= 0)
    return false;
  const usize base = out.size();
  out.resize(base + nx::cast<usize>(need));
  const i32 wrote = ::WideCharToMultiByte(
      CP_UTF8, 0, w, wlen, reinterpret_cast<char *>(out.data() + base), need,
      nullptr, nullptr);
  return wrote == need;
}

io_result<void> copy_node_w(string &from, string &to, bool overwrite,
                            i32 depth);

io_result<void> copy_dir_w(string &from, string &to, const bool overwrite,
                           i32 depth) {
  {
    const wide_path wt(to.view());
    if (!wt.ok)
      return err(file_error{io_error::InvalidArgument, 0});
    if (!::CreateDirectoryW(wt.ptr, nullptr) &&
        ::GetLastError() != ERROR_ALREADY_EXISTS)
      return err(map_last_error(io_error::OpenFailed));
  }

  string pattern = from;
  pattern.push_back(u8'\\');
  pattern.push_back(u8'*');
  const wide_path wp(pattern.view());
  if (!wp.ok)
    return err(file_error{io_error::InvalidArgument, 0});

  WIN32_FIND_DATAW fd;
  const HANDLE h = ::FindFirstFileW(wp.ptr, &fd);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD e = ::GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_NO_MORE_FILES)
      return {};
    return err(map_last_error(io_error::ReadFailed));
  }

  const usize from_len = from.size();
  const usize to_len = to.size();
  io_result<void> result{};

  do {
    const wchar_t *name = fd.cFileName;
    if (name[0] == L'.' &&
        (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0')))
      continue;

    const i32 nlen = nx::cast<i32>(::wcslen(name));
    from.resize(from_len);
    from.push_back(u8'\\');
    to.resize(to_len);
    to.push_back(u8'\\');
    if (!append_wide_utf8(from, name, nlen) ||
        !append_wide_utf8(to, name, nlen)) {
      result = err(file_error{io_error::InvalidArgument, 0});
      break;
    }

    result = copy_node_w(from, to, overwrite, depth + 1);
    if (!result)
      break;
  } while (::FindNextFileW(h, &fd));

  if (result && ::GetLastError() != ERROR_NO_MORE_FILES)
    result = err(map_last_error(io_error::ReadFailed));

  ::FindClose(h);
  from.resize(from_len);
  to.resize(to_len);
  return result;
}

io_result<void> copy_node_w(string &from, string &to, const bool overwrite,
                            i32 depth) {
  if (depth > kMaxCopyDepth)
    return err(file_error{io_error::InvalidArgument, 0});

  const wide_path wf(from.view());
  if (!wf.ok)
    return err(file_error{io_error::InvalidArgument, 0});

  const DWORD attr = ::GetFileAttributesW(wf.ptr);
  if (attr == INVALID_FILE_ATTRIBUTES)
    return err(map_last_error(io_error::OpenFailed));

  if (attr & FILE_ATTRIBUTE_DIRECTORY)
    return copy_dir_w(from, to, overwrite, depth);
  return copy_file(from.view(), to.view(), overwrite);
}

io_result<void> remove_node_w(string &path, i32 depth);

io_result<void> remove_dir_w(string &path, i32 depth) {
  string pattern = path;
  pattern.push_back(u8'\\');
  pattern.push_back(u8'*');
  const wide_path wp(pattern.view());
  if (!wp.ok)
    return err(file_error{io_error::InvalidArgument, 0});

  WIN32_FIND_DATAW fd;
  HANDLE h = ::FindFirstFileW(wp.ptr, &fd);
  if (h != INVALID_HANDLE_VALUE) {
    const usize len = path.size();
    io_result<void> result{};
    do {
      const wchar_t *name = fd.cFileName;
      if (name[0] == L'.' &&
          (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0')))
        continue;
      path.resize(len);
      path.push_back(u8'\\');
      if (!append_wide_utf8(path, name, nx::cast<i32>(::wcslen(name)))) {
        result = err(file_error{io_error::InvalidArgument, 0});
        break;
      }
      result = remove_node_w(path, depth + 1);
      if (!result)
        break;
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    path.resize(len);
    if (!result)
      return result;
  }

  const wide_path wd(path.view());
  if (!wd.ok)
    return err(file_error{io_error::InvalidArgument, 0});
  if (!::RemoveDirectoryW(wd.ptr) && ::GetLastError() != ERROR_FILE_NOT_FOUND)
    return err(map_last_error(io_error::OpenFailed));
  return {};
}

io_result<void> remove_node_w(string &path, i32 depth) {
  if (depth > kMaxCopyDepth)
    return err(file_error{io_error::InvalidArgument, 0});

  const wide_path wp(path.view());
  if (!wp.ok)
    return err(file_error{io_error::InvalidArgument, 0});

  const DWORD attr = ::GetFileAttributesW(wp.ptr);
  if (attr == INVALID_FILE_ATTRIBUTES) {
    const DWORD e = ::GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
      return {};
    return err(map_last_error(io_error::OpenFailed));
  }

  if (attr & FILE_ATTRIBUTE_READONLY)
    ::SetFileAttributesW(wp.ptr, attr & ~DWORD{FILE_ATTRIBUTE_READONLY});

  // A reparse-point directory is unlinked, never recursed into.
  if ((attr & FILE_ATTRIBUTE_DIRECTORY) &&
      !(attr & FILE_ATTRIBUTE_REPARSE_POINT))
    return remove_dir_w(path, depth);

  const BOOL ok = (attr & FILE_ATTRIBUTE_DIRECTORY) ? ::RemoveDirectoryW(wp.ptr)
                                                    : ::DeleteFileW(wp.ptr);
  if (!ok && ::GetLastError() != ERROR_FILE_NOT_FOUND)
    return err(map_last_error(io_error::OpenFailed));
  return {};
}

file_type status_w(const path_view path, const bool follow) {
  const wide_path wp(path);
  if (!wp.ok)
    return file_type::none;
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!::GetFileAttributesExW(wp.ptr, GetFileExInfoStandard, &d)) {
    const DWORD e = ::GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
      return file_type::not_found;
    return file_type::none;
  }
  const DWORD a = d.dwFileAttributes;
  if (!follow && (a & FILE_ATTRIBUTE_REPARSE_POINT))
    return file_type::symlink;
  if (a & FILE_ATTRIBUTE_DIRECTORY)
    return file_type::directory;
  return file_type::regular;
}

std::optional<file_error> list_dir_w(string &dir, vector<directory_entry> &out,
                                     const bool recursive, const i32 depth) {
  if (depth > kMaxCopyDepth)
    return file_error{io_error::InvalidArgument, 0};

  const usize dir_len = dir.size();

  string pattern = dir;
  pattern.push_back(u8'\\');
  pattern.push_back(u8'*');
  const wide_path wp(pattern.view());
  if (!wp.ok)
    return file_error{io_error::InvalidArgument, 0};

  WIN32_FIND_DATAW fd;
  const HANDLE h = ::FindFirstFileW(wp.ptr, &fd);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD e = ::GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_NO_MORE_FILES)
      return std::nullopt;
    // A descendant we cannot open is skipped; a bad root is reported.
    if (depth > 0)
      return std::nullopt;
    return map_last_error(io_error::OpenFailed);
  }

  std::optional<file_error> err;
  do {
    const wchar_t *name = fd.cFileName;
    if (name[0] == L'.' &&
        (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0')))
      continue;

    dir.resize(dir_len);
    dir.push_back(u8'\\');
    if (!append_wide_utf8(dir, name, nx::cast<i32>(::wcslen(name)))) {
      err = file_error{io_error::InvalidArgument, 0};
      break;
    }

    const DWORD attr = fd.dwFileAttributes;
    file_type t;
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT)
      t = file_type::symlink;
    else if (attr & FILE_ATTRIBUTE_DIRECTORY)
      t = file_type::directory;
    else
      t = file_type::regular;

    out.push_back(directory_entry{.path = dir, .type = t});

    if (recursive && t == file_type::directory) {
      err = list_dir_w(dir, out, true, depth + 1);
      if (err)
        break;
    }
  } while (::FindNextFileW(h, &fd));

  if (!err && ::GetLastError() != ERROR_NO_MORE_FILES)
    err = map_last_error(io_error::ReadFailed);

  ::FindClose(h);
  dir.resize(dir_len);
  return err;
}

} // namespace

io_result<void> copy_file(const path_view from, const path_view to,
                          const bool overwrite_existing) {
  const wide_path wf(from);
  const wide_path wt(to);
  if (!wf.ok || !wt.ok)
    return err(file_error{io_error::InvalidArgument, 0});

  const DWORD attr = ::GetFileAttributesW(wf.ptr);
  if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
    return err(file_error{io_error::IsADirectory, 0});

  if (!::CopyFileW(wf.ptr, wt.ptr, overwrite_existing ? FALSE : TRUE)) {
    const DWORD e = ::GetLastError();
    if (!overwrite_existing &&
        (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS))
      return err(file_error{io_error::AlreadyExists, nx::cast<i32>(e)});
    return err(map_last_error(io_error::WriteFailed));
  }
  return {};
}

io_result<void> copy(const path_view from, const path_view to,
                     const bool overwrite_existing) {
  string f(from.native());
  string t(to.native());
  return copy_node_w(f, t, overwrite_existing, 0);
}

io_result<void> remove(const path_view path) {
  const wide_path wp(path);
  if (!wp.ok)
    return err(file_error{io_error::InvalidArgument, 0});

  const DWORD attr = ::GetFileAttributesW(wp.ptr);
  if (attr == INVALID_FILE_ATTRIBUTES) {
    const DWORD e = ::GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
      return err(file_error{io_error::NotFound, nx::cast<i32>(e)});
    return err(map_last_error(io_error::OpenFailed));
  }

  if (attr & FILE_ATTRIBUTE_READONLY)
    ::SetFileAttributesW(wp.ptr, attr & ~DWORD{FILE_ATTRIBUTE_READONLY});

  const BOOL ok = (attr & FILE_ATTRIBUTE_DIRECTORY) ? ::RemoveDirectoryW(wp.ptr)
                                                    : ::DeleteFileW(wp.ptr);
  if (!ok)
    return err(map_last_error(io_error::OpenFailed));
  return {};
}

io_result<void> remove_all(const path_view path) {
  string p(path.native());
  return remove_node_w(p, 0);
}

io_result<void> rename(const path_view from, const path_view to) {
  const wide_path wf(from);
  const wide_path wt(to);
  if (!wf.ok || !wt.ok)
    return err(file_error{io_error::InvalidArgument, 0});

  if (::MoveFileExW(wf.ptr, wt.ptr,
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
    return {};

  const file_error me = map_last_error(io_error::OpenFailed);
  if (me.native != nx::cast<i32>(ERROR_NOT_SAME_DEVICE))
    return err(me);

  auto copied = copy(from, to, true);
  if (!copied)
    return copied;
  return remove_all(from);
}

file_type status(const path_view path) { return status_w(path, true); }
file_type symlink_status(const path_view path) { return status_w(path, false); }

io_result<vector<directory_entry>> directory_entries(const path_view root) {
  vector<directory_entry> out;
  string dir(root.native());
  if (const auto e = list_dir_w(dir, out, false, 0))
    return err(*e);
  return out;
}

io_result<vector<directory_entry>>
recursive_directory_entries(const path_view root) {
  vector<directory_entry> out;
  string dir(root.native());
  if (const auto e = list_dir_w(dir, out, true, 0))
    return err(*e);
  return out;
}

#elif defined(NX_PLATFORM_POSIX)

io_result<blob<u8>> file_read(const path_view path) {
  cpath cp(path);

  fd_guard g{open_retry(cp.ptr, O_RDONLY | O_CLOEXEC, 0)};
  if (g.get() < 0)
    return err(map_errno(io_error::OpenFailed));

  struct stat st;
  if (::fstat(g.get(), &st) != 0)
    return err(map_errno(io_error::OpenFailed));
  if (S_ISDIR(st.st_mode))
    return err(file_error{io_error::IsADirectory, 0});

  byte_buf buf;
  if (st.st_size > 0)
    buf.reserve(nx::cast<usize>(st.st_size));

  for (;;) {
    buf.reserve(buf.len + kChunk);
    const ssize_t n = ::read(g.get(), buf.p + buf.len, buf.cap - buf.len);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return err(map_errno(io_error::ReadFailed));
    }
    if (n == 0)
      break;
    buf.len += nx::cast<usize>(n);
  }

  return buf.into_blob();
}

io_result<void> file_write(const path_view path, std::span<const u8> data) {
  cpath cp(path);

  fd_guard g{
      open_retry(cp.ptr, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)};
  if (g.get() < 0)
    return err(map_errno(io_error::OpenFailed));

  usize total = 0;
  while (total < data.size()) {
    const ssize_t n =
        ::write(g.get(), data.data() + total, data.size() - total);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return err(map_errno(io_error::WriteFailed));
    }
    total += nx::cast<usize>(n);
  }

  if (::close(g.release()) != 0)
    return err(map_errno(io_error::WriteFailed));
  return {};
}

io_result<void> create_directories(const path_view path) {
  cpath cp(path);
  char *p = const_cast<char *>(cp.ptr);

  const auto make = [](const char *dir) -> io_result<void> {
    if (!dir[0])
      return {};
    if (::mkdir(dir, 0755) == 0 || errno == EEXIST)
      return {};
    return err(map_errno(io_error::OpenFailed));
  };

  for (char *s = p + (p[0] == '/' ? 1 : 0); *s; ++s) {
    if (*s == '/') {
      *s = '\0';
      auto r = make(p);
      *s = '/';
      if (!r)
        return r;
    }
  }
  return make(p);
}

namespace {

io_result<void> copy_node_posix(string &from, string &to, bool overwrite,
                                i32 depth);

io_result<void> copy_dir_posix(string &from, string &to, bool overwrite,
                               i32 depth, mode_t mode) {
  if (::mkdir(to.c_str(), mode & 0777) != 0 && errno != EEXIST)
    return err(map_errno(io_error::OpenFailed));

  DIR *d = ::opendir(from.c_str());
  if (!d)
    return err(map_errno(io_error::OpenFailed));
  defer { ::closedir(d); };

  const usize from_len = from.size();
  const usize to_len = to.size();
  io_result<void> result{};

  for (;;) {
    errno = 0;
    const struct dirent *e = ::readdir(d);
    if (!e) {
      if (errno != 0)
        result = err(map_errno(io_error::ReadFailed));
      break;
    }
    const char *name = e->d_name;
    if (name[0] == '.' &&
        (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
      continue;

    from.resize(from_len);
    from.push_back(u8'/');
    from.append(name);
    to.resize(to_len);
    to.push_back(u8'/');
    to.append(name);

    result = copy_node_posix(from, to, overwrite, depth + 1);
    if (!result)
      break;
  }

  from.resize(from_len);
  to.resize(to_len);
  return result;
}

io_result<void> copy_symlink_posix(const string &from, const string &to,
                                   const struct stat &st, bool overwrite) {
  usize cap = st.st_size > 0 ? nx::cast<usize>(st.st_size) + 1 : 256;
  vector<char> buf(cap);
  for (;;) {
    const ssize_t n = ::readlink(from.c_str(), buf.data(), buf.size());
    if (n < 0)
      return err(map_errno(io_error::ReadFailed));
    if (nx::cast<usize>(n) < buf.size()) {
      buf[nx::cast<usize>(n)] = '\0';
      break;
    }
    buf.resize(buf.size() * 2);
  }

  if (overwrite)
    ::unlink(to.c_str());
  if (::symlink(buf.data(), to.c_str()) != 0) {
    if (errno == EEXIST)
      return err(file_error{io_error::AlreadyExists, EEXIST});
    return err(map_errno(io_error::WriteFailed));
  }
  return {};
}

io_result<void> copy_node_posix(string &from, string &to, bool overwrite,
                                i32 depth) {
  if (depth > kMaxCopyDepth)
    return err(file_error{io_error::InvalidArgument, 0});

  struct stat st;
  if (::lstat(from.c_str(), &st) != 0)
    return err(map_errno(io_error::OpenFailed));

  if (S_ISLNK(st.st_mode))
    return copy_symlink_posix(from, to, st, overwrite);
  if (S_ISDIR(st.st_mode))
    return copy_dir_posix(from, to, overwrite, depth, st.st_mode);
  if (S_ISREG(st.st_mode))
    return copy_file(from.view(), to.view(), overwrite);

  return err(file_error{io_error::InvalidArgument, 0});
}

io_result<void> remove_node_posix(string &path, i32 depth) {
  if (depth > kMaxCopyDepth)
    return err(file_error{io_error::InvalidArgument, 0});

  struct stat st;
  if (::lstat(path.c_str(), &st) != 0) {
    if (errno == ENOENT)
      return {};
    return err(map_errno(io_error::OpenFailed));
  }

  if (S_ISDIR(st.st_mode)) {
    DIR *d = ::opendir(path.c_str());
    if (!d)
      return err(map_errno(io_error::OpenFailed));
    defer { ::closedir(d); };

    const usize len = path.size();
    io_result<void> result{};
    for (;;) {
      errno = 0;
      const struct dirent *e = ::readdir(d);
      if (!e) {
        if (errno != 0)
          result = err(map_errno(io_error::ReadFailed));
        break;
      }
      const char *name = e->d_name;
      if (name[0] == '.' &&
          (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
        continue;
      path.resize(len);
      path.push_back(u8'/');
      path.append(name);
      result = remove_node_posix(path, depth + 1);
      if (!result)
        break;
    }
    path.resize(len);
    if (!result)
      return result;
    if (::rmdir(path.c_str()) != 0 && errno != ENOENT)
      return err(map_errno(io_error::OpenFailed));
    return {};
  }

  if (::unlink(path.c_str()) != 0 && errno != ENOENT)
    return err(map_errno(io_error::OpenFailed));
  return {};
}

file_type type_from_mode(mode_t m) noexcept {
  if (S_ISREG(m))
    return file_type::regular;
  if (S_ISDIR(m))
    return file_type::directory;
  if (S_ISLNK(m))
    return file_type::symlink;
  return file_type::other;
}

std::optional<file_error> list_dir_posix(string &dir,
                                         vector<directory_entry> &out,
                                         bool recursive, i32 depth) {
  if (depth > kMaxCopyDepth)
    return file_error{io_error::InvalidArgument, 0};

  DIR *d = ::opendir(dir.c_str());
  if (!d) {
    // A descendant we cannot open is skipped; a bad root is reported.
    if (depth > 0)
      return std::nullopt;
    return map_errno(io_error::OpenFailed);
  }
  defer { ::closedir(d); };

  const usize dir_len = dir.size();
  std::optional<file_error> err;

  for (;;) {
    errno = 0;
    const struct dirent *e = ::readdir(d);
    if (!e) {
      if (errno != 0)
        err = map_errno(io_error::ReadFailed);
      break;
    }
    const char *name = e->d_name;
    if (name[0] == '.' &&
        (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
      continue;

    dir.resize(dir_len);
    dir.push_back(u8'/');
    dir.append(name);

    // lstat, not stat: symlinks are reported as such and never followed.
    struct stat st;
    if (::lstat(dir.c_str(), &st) != 0)
      continue;
    const file_type t = type_from_mode(st.st_mode);

    out.push_back(directory_entry{.path = dir, .type = t});

    if (recursive && t == file_type::directory) {
      err = list_dir_posix(dir, out, true, depth + 1);
      if (err)
        break;
    }
  }

  dir.resize(dir_len);
  return err;
}

} // namespace

io_result<void> copy_file(path_view from, path_view to,
                          bool overwrite_existing) {
  cpath src(from);
  cpath dst(to);

  fd_guard in{open_retry(src.ptr, O_RDONLY | O_CLOEXEC, 0)};
  if (in.get() < 0)
    return err(map_errno(io_error::OpenFailed));

  struct stat st;
  if (::fstat(in.get(), &st) != 0)
    return err(map_errno(io_error::OpenFailed));
  if (S_ISDIR(st.st_mode))
    return err(file_error{io_error::IsADirectory, 0});

  i32 flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
  if (!overwrite_existing)
    flags |= O_EXCL;
  fd_guard out{open_retry(dst.ptr, flags, st.st_mode & 0777)};
  if (out.get() < 0) {
    if (!overwrite_existing && errno == EEXIST)
      return err(file_error{io_error::AlreadyExists, EEXIST});
    return err(map_errno(io_error::OpenFailed));
  }

  vector<u8> buf(kChunk);
  for (;;) {
    ssize_t n;
    do {
      n = ::read(in.get(), buf.data(), buf.size());
    } while (n < 0 && errno == EINTR);
    if (n < 0)
      return err(map_errno(io_error::ReadFailed));
    if (n == 0)
      break;

    usize off = 0;
    while (off < nx::cast<usize>(n)) {
      const ssize_t w =
          ::write(out.get(), buf.data() + off, nx::cast<usize>(n) - off);
      if (w < 0) {
        if (errno == EINTR)
          continue;
        return err(map_errno(io_error::WriteFailed));
      }
      off += nx::cast<usize>(w);
    }
  }

  if (::close(out.release()) != 0)
    return err(map_errno(io_error::WriteFailed));
  return {};
}

io_result<void> copy(path_view from, path_view to, bool overwrite_existing) {
  string f(from.native());
  string t(to.native());
  return copy_node_posix(f, t, overwrite_existing, 0);
}

io_result<void> remove(path_view path) {
  cpath cp(path);
  struct stat st;
  if (::lstat(cp.ptr, &st) != 0) {
    if (errno == ENOENT)
      return err(file_error{io_error::NotFound, ENOENT});
    return err(map_errno(io_error::OpenFailed));
  }
  const i32 rc = S_ISDIR(st.st_mode) ? ::rmdir(cp.ptr) : ::unlink(cp.ptr);
  if (rc != 0)
    return err(map_errno(io_error::OpenFailed));
  return {};
}

io_result<void> remove_all(path_view path) {
  string p(path.native());
  return remove_node_posix(p, 0);
}

io_result<void> rename(path_view from, path_view to) {
  cpath f(from);
  cpath t(to);
  if (::rename(f.ptr, t.ptr) == 0)
    return {};
  if (errno != EXDEV)
    return err(map_errno(io_error::OpenFailed));

  auto copied = copy(from, to, true);
  if (!copied)
    return copied;
  return remove_all(from);
}

file_type status(path_view path) {
  cpath cp(path);
  struct stat st;
  if (::stat(cp.ptr, &st) != 0)
    return errno == ENOENT ? file_type::not_found : file_type::none;
  return type_from_mode(st.st_mode);
}

file_type symlink_status(path_view path) {
  cpath cp(path);
  struct stat st;
  if (::lstat(cp.ptr, &st) != 0)
    return errno == ENOENT ? file_type::not_found : file_type::none;
  return type_from_mode(st.st_mode);
}

io_result<vector<directory_entry>> directory_entries(path_view root) {
  vector<directory_entry> out;
  string dir(root.native());
  if (auto e = list_dir_posix(dir, out, false, 0))
    return err(*e);
  return out;
}

io_result<vector<directory_entry>> recursive_directory_entries(path_view root) {
  vector<directory_entry> out;
  string dir(root.native());
  if (auto e = list_dir_posix(dir, out, true, 0))
    return err(*e);
  return out;
}

#endif

io_result<string> file_read_text(const path_view path) {
  auto r = file_read(path);
  if (!r)
    return err(r.error());
  const blob<u8> &b = r.value();
  return string(reinterpret_cast<const char *>(b.data()), b.size());
}

io_result<void> file_write_text(const path_view path, const string_view text) {
  return file_write(
      path, std::span<const u8>(reinterpret_cast<const u8 *>(text.data()),
                                text.size()));
}

// streams

namespace {

class memory_stream final : public stream {
public:
  memory_stream(const void *data, const usize size) noexcept
      : m_data(static_cast<const u8 *>(data)), m_size(size) {}

  usize read(void *dst, const usize bytes) override {
    const usize n = min(bytes, m_size - m_cursor);
    if (n)
      std::memcpy(dst, m_data + m_cursor, n);
    m_cursor += n;
    return n;
  }

  bool seek(const i64 offset, const seek_origin origin) override {
    const i64 base = origin == seek_origin::begin     ? 0
                     : origin == seek_origin::current ? nx::cast<i64>(m_cursor)
                                                      : nx::cast<i64>(m_size);
    const i64 pos = base + offset;
    if (pos < 0 || pos > nx::cast<i64>(m_size))
      return false;
    m_cursor = nx::cast<usize>(pos);
    return true;
  }

  [[nodiscard]] i64 tell() const override { return nx::cast<i64>(m_cursor); }
  [[nodiscard]] i64 size() const override { return nx::cast<i64>(m_size); }
  [[nodiscard]] bool eof() const override { return m_cursor >= m_size; }

private:
  const u8 *m_data = nullptr;
  usize m_size = 0;
  usize m_cursor = 0;
};

#if defined(NX_PLATFORM_WINDOWS)

class file_stream final : public stream {
public:
  explicit file_stream(const HANDLE h) noexcept : m_h(h) {}
  ~file_stream() override {
    if (m_h != INVALID_HANDLE_VALUE)
      ::CloseHandle(m_h);
  }

  usize read(void *dst, const usize bytes) override {
    usize total = 0;
    auto *p = static_cast<u8 *>(dst);
    while (total < bytes) {
      const DWORD want = nx::cast<DWORD>(min<usize>(bytes - total, 1u << 30));
      DWORD got = 0;
      if (!::ReadFile(m_h, p + total, want, &got, nullptr))
        break;
      if (got == 0) {
        m_at_eof = true;
        break;
      }
      total += got;
    }
    return total;
  }

  bool seek(const i64 offset, const seek_origin origin) override {
    LARGE_INTEGER dist;
    dist.QuadPart = offset;
    const DWORD method = origin == seek_origin::begin     ? FILE_BEGIN
                         : origin == seek_origin::current ? FILE_CURRENT
                                                          : FILE_END;
    LARGE_INTEGER np;
    if (!::SetFilePointerEx(m_h, dist, &np, method))
      return false;
    m_at_eof = false;
    return true;
  }

  [[nodiscard]] i64 tell() const override {
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    LARGE_INTEGER np;
    if (!::SetFilePointerEx(m_h, zero, &np, FILE_CURRENT))
      return -1;
    return np.QuadPart;
  }

  [[nodiscard]] i64 size() const override {
    LARGE_INTEGER sz;
    if (!::GetFileSizeEx(m_h, &sz))
      return -1;
    return sz.QuadPart;
  }

  [[nodiscard]] bool eof() const override {
    if (m_at_eof)
      return true;
    const i64 t = tell(), s = size();
    return t >= 0 && s >= 0 && t >= s;
  }

private:
  HANDLE m_h = INVALID_HANDLE_VALUE;
  bool m_at_eof = false;
};

#elif defined(NX_PLATFORM_POSIX)

class file_stream final : public stream {
public:
  explicit file_stream(const int fd) noexcept : m_fd(fd) {}
  ~file_stream() override {
    if (m_fd >= 0)
      ::close(m_fd);
  }

  usize read(void *dst, const usize bytes) override {
    usize total = 0;
    auto *p = static_cast<u8 *>(dst);
    while (total < bytes) {
      const ssize_t got = ::read(m_fd, p + total, bytes - total);
      if (got < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (got == 0) {
        m_at_eof = true;
        break;
      }
      total += nx::cast<usize>(got);
    }
    return total;
  }

  bool seek(const i64 offset, const seek_origin origin) override {
    const int whence = origin == seek_origin::begin     ? SEEK_SET
                       : origin == seek_origin::current ? SEEK_CUR
                                                        : SEEK_END;
    if (::lseek(m_fd, nx::cast<off_t>(offset), whence) < 0)
      return false;
    m_at_eof = false;
    return true;
  }

  [[nodiscard]] i64 tell() const override {
    const off_t p = ::lseek(m_fd, 0, SEEK_CUR);
    return p < 0 ? -1 : nx::cast<i64>(p);
  }

  [[nodiscard]] i64 size() const override {
    struct stat st;
    if (::fstat(m_fd, &st) != 0)
      return -1;
    return nx::cast<i64>(st.st_size);
  }

  [[nodiscard]] bool eof() const override {
    if (m_at_eof)
      return true;
    const i64 t = tell(), s = size();
    return t >= 0 && s >= 0 && t >= s;
  }

private:
  int m_fd = -1;
  bool m_at_eof = false;
};

#endif

} // namespace

io_result<stream_ptr> open_memory(const void *data, const usize size) {
  if (!data && size)
    return err(file_error{io_error::InvalidArgument, 0});
  auto *s = allocate<memory_stream>(data, size);
  if (!s)
    return err(file_error{io_error::OpenFailed, 0});
  return stream_ptr(s);
}

#if defined(NX_PLATFORM_WINDOWS)

io_result<stream_ptr> open_file(const path_view path) {
  const wide_path wp(path);
  if (!wp.ok)
    return err(file_error{io_error::InvalidArgument, 0});

  const HANDLE h = ::CreateFileW(wp.ptr, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return err(map_last_error(io_error::OpenFailed));

  auto *s = allocate<file_stream>(h);
  if (!s) {
    ::CloseHandle(h);
    return err(file_error{io_error::OpenFailed, 0});
  }
  return stream_ptr(s);
}

io_result<string> executable_dir() {
  wchar_t buf[1024];
  const DWORD n =
      ::GetModuleFileNameW(nullptr, buf, nx::cast<DWORD>(std::size(buf)));
  if (n == 0 || n >= std::size(buf))
    return err(map_last_error(io_error::Unknown));

  string full;
  if (!append_wide_utf8(full, buf, nx::cast<i32>(n)))
    return err(file_error{io_error::Unknown, 0});
  return string(path::parent_path(full.view()));
}

#elif defined(NX_PLATFORM_POSIX)

io_result<stream_ptr> open_file(const path_view path) {
  cpath cp(path);
  const int fd = open_retry(cp.ptr, O_RDONLY | O_CLOEXEC, 0);
  if (fd < 0)
    return err(map_errno(io_error::OpenFailed));

  auto *s = allocate<file_stream>(fd);
  if (!s) {
    ::close(fd);
    return err(file_error{io_error::OpenFailed, 0});
  }
  return stream_ptr(s);
}

io_result<string> executable_dir() {
#if defined(NX_PLATFORM_APPLE)
  // No /proc on Darwin. _NSGetExecutablePath can hand back a path containing
  // symlinks or "..", so realpath() it into the canonical form callers expect.
  char raw[4096];
  u32 size = nx::cast<u32>(sizeof(raw));
  if (_NSGetExecutablePath(raw, &size) != 0)
    return err(file_error{io_error::InvalidArgument, 0});

  char resolved[PATH_MAX];
  const char *full = ::realpath(raw, resolved) != nullptr ? resolved : raw;
  return string(path::parent_path(string_view(full, strlen(full))));
#else
  char buf[4096];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0)
    return err(map_errno(io_error::Unknown));
  return string(path::parent_path(string_view(buf, nx::cast<usize>(n))));
#endif
}

#endif

} // namespace nx::fs