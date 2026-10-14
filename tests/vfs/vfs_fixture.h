#pragma once

#include "core/foundation/platform/filesystem.h"
#include "core/foundation/platform/os.h"
#include "core/foundation/vfs/vfs.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

namespace vfs_fixture {

// ctest's discovery model re-invokes the owning executable once per
// TEST_CASE (nxtest_discover.cmake: `add_test(... "--test-case=${name}")`),
// so many concurrent processes - across different test binaries that also
// include this header, and repeated invocations of the same one - can be
// live at once under a parallel ctest run. A path with no process-unique
// component let one process's remove_all() delete another's in-flight
// scratch tree out from under it; the process id makes each instance
// genuinely private.
class scratch_root {
public:
  static const nx::string &get() {
    static const scratch_root instance;
    return instance.m_path;
  }

private:
  scratch_root() {
    const char *tmp = std::getenv("TEMP");
    if (!tmp)
      tmp = std::getenv("TMP");
    if (!tmp)
      tmp = std::getenv("TMPDIR");
    if (!tmp)
      tmp = ".";
    m_path = nx::fs::path::join(
        nx::string_view(tmp),
        nx::string_view("nx_vfs_tests_" + std::to_string(nx::os::process_id())));
    (void)nx::fs::remove_all(nx::fs::path_view(m_path));
    (void)nx::fs::create_directories(nx::fs::path_view(m_path));
  }
  ~scratch_root() { (void)nx::fs::remove_all(nx::fs::path_view(m_path)); }

  nx::string m_path;
};

class temp_dir {
public:
  explicit temp_dir(const char *label) {
    static std::atomic<int> counter{0};
    m_path = nx::fs::path::join(
        nx::string_view(scratch_root::get()),
        nx::string_view(std::string(label) + "_" +
                        std::to_string(counter.fetch_add(1))));
    (void)nx::fs::remove_all(nx::fs::path_view(m_path));
    (void)nx::fs::create_directories(nx::fs::path_view(m_path));
  }
  ~temp_dir() { (void)nx::fs::remove_all(nx::fs::path_view(m_path)); }

  temp_dir(const temp_dir &) = delete;
  temp_dir &operator=(const temp_dir &) = delete;

  [[nodiscard]] const nx::string &str() const { return m_path; }
  [[nodiscard]] nx::fs::path_view view() const {
    return nx::fs::path_view(m_path);
  }
  [[nodiscard]] nx::string file(const char *name) const {
    return nx::fs::path::join(m_path.view(), nx::string_view(name));
  }

private:
  nx::string m_path;
};

class scoped_vfs {
public:
  scoped_vfs() {
    nx::vfs::shutdown();
    nx::vfs::initialize();
  }
  ~scoped_vfs() { nx::vfs::shutdown(); }

  scoped_vfs(const scoped_vfs &) = delete;
  scoped_vfs &operator=(const scoped_vfs &) = delete;
};

inline std::span<const u8> bytes_of(const char *s) {
  return {reinterpret_cast<const u8 *>(s), std::strlen(s)};
}

inline nx::blob<u8> blob_of(const char *s) { return nx::blob<u8>(bytes_of(s)); }

inline bool read_eq(const char *path, const char *expect) {
  auto r = nx::vfs::read(path);
  if (!r)
    return false;
  const usize n = std::strlen(expect);
  return r->size() == n && std::memcmp(r->data(), expect, n) == 0;
}

inline bool has_name(const nx::vector<nx::vfs::DirEntry> &v, const char *name) {
  for (const nx::vfs::DirEntry &e : v)
    if (e.name == name)
      return true;
  return false;
}

}
