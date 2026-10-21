#include "core/foundation/vfs/vfs.h"
#include "core/foundation/vfs/vfs_internal.h"

#include <optional>

namespace nx::vfs {

namespace {

class HostDevice final : public Device {
public:
  HostDevice(nx::string root, const HostDeviceAccess access)
      : m_root(std::move(root)), m_access(access) {}

  [[nodiscard]] io_result<nx::blob<u8>> read(const path_view rel) override {
    const auto path = full(rel);
    if (!path)
      return err(io_error::InvalidArgument);
    return nx::fs::file_read(*path);
  }

  [[nodiscard]] io_result<stream_ptr> open(const path_view rel) override {
    const auto path = full(rel);
    if (!path)
      return err(io_error::InvalidArgument);
    return nx::fs::open_file(*path);
  }

  [[nodiscard]] FileInfo stat(const path_view rel) override {
    const auto path = full(rel);
    if (!path)
      return {};
    const nx::fs::file_stat s = nx::fs::stat_file(*path);
    FileInfo info;
    info.exists = s.type != nx::fs::file_type::not_found &&
                  s.type != nx::fs::file_type::none;
    info.is_directory = s.type == nx::fs::file_type::directory;
    info.size = info.is_directory ? 0 : s.size;
    info.mtime_ns = s.mtime_ns;
    return info;
  }

  [[nodiscard]] io_result<void> list(const path_view rel_dir,
                                     const ListFn &cb) override {
    const auto path = full(rel_dir);
    if (!path)
      return err(io_error::InvalidArgument);
    auto entries = nx::fs::directory_entries(*path);
    if (!entries)
      return err(entries.error());
    for (const nx::fs::directory_entry &e : entries.value()) {
      FileInfo info;
      info.exists = true;
      info.is_directory = e.type == nx::fs::file_type::directory;
      const nx::fs::file_stat s = nx::fs::stat_file(e.path.view());
      info.size = info.is_directory ? 0 : s.size;
      info.mtime_ns = s.mtime_ns;
      cb(nx::fs::path::filename(e.path.view()), info);
    }
    return {};
  }

  [[nodiscard]] bool writable() const noexcept override {
    return m_access == HostDeviceAccess::ReadWrite;
  }
  [[nodiscard]] bool watchable() const noexcept override { return true; }
  [[nodiscard]] bool native_read_path(const path_view rel,
                                      nx::string &out) const override {
    auto path = full(rel);
    if (!path)
      return false;
    out = std::move(*path);
    return true;
  }

  io_result<void> write(const path_view rel,
                        const std::span<const u8> data) override {
    const auto full_path = full(rel);
    if (!full_path)
      return err(io_error::InvalidArgument);
    const nx::string &path = *full_path;
    const nx::string_view parent = nx::fs::path::parent_path(path.view());
    if (!parent.empty()) {
      if (auto r = nx::fs::create_directories(parent); !r)
        return r;
    }
    return nx::fs::file_write_atomic(path.view(), data,
                                     nx::fs::durability::flushed);
  }

  io_result<void> remove(const path_view rel) override {
    const auto path = full(rel);
    if (!path)
      return err(io_error::InvalidArgument);
    return nx::fs::remove(*path);
  }

  io_result<void> create_directories(path_view rel) override {
    const auto path = full(rel);
    if (!path)
      return err(io_error::InvalidArgument);
    return nx::fs::create_directories(*path);
  }

  [[nodiscard]] const char *name() const noexcept override { return "host"; }

private:
  // Nothing for a path that would leave the root: one that climbs above it,
  // or names a drive or a stream.
  [[nodiscard]] std::optional<nx::string> full(const path_view rel) const {
    const nx::string v = detail::canon_vpath(rel);
    if (v.empty())
      return std::nullopt;
    return nx::fs::path::join(m_root.view(), v.view().substr(1));
  }

  nx::string m_root;
  HostDeviceAccess m_access = HostDeviceAccess::ReadWrite;
};

} // namespace

Device *make_host_device(const path_view root, const HostDeviceAccess access) {
  return nx::allocate<HostDevice>(nx::string(root.native()), access);
}

} // namespace nx::vfs
