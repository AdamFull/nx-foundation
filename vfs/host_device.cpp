#include "core/foundation/vfs/vfs.h"

namespace nx::vfs {

namespace {

class HostDevice final : public Device {
public:
  HostDevice(nx::string root, const HostDeviceAccess access)
      : m_root(std::move(root)), m_access(access) {}

  [[nodiscard]] io_result<nx::blob<u8>> read(const path_view rel) override {
    return nx::fs::file_read(full(rel));
  }

  [[nodiscard]] io_result<stream_ptr> open(const path_view rel) override {
    return nx::fs::open_file(full(rel));
  }

  [[nodiscard]] FileInfo stat(const path_view rel) override {
    const nx::fs::file_stat s = nx::fs::stat_file(full(rel));
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
    auto entries = nx::fs::directory_entries(full(rel_dir));
    if (!entries)
      return err(entries.error());
    for (const nx::fs::directory_entry &e : entries.value()) {
      FileInfo info;
      info.exists = true;
      info.is_directory = e.type == nx::fs::file_type::directory;
      cb(nx::fs::path::filename(e.path.view()), info);
    }
    return {};
  }

  [[nodiscard]] bool writable() const noexcept override {
    return m_access == HostDeviceAccess::ReadWrite;
  }
  [[nodiscard]] bool watchable() const noexcept override { return true; }

  io_result<void> write(const path_view rel,
                        const std::span<const u8> data) override {
    const nx::string path = full(rel);
    const nx::string_view parent = nx::fs::path::parent_path(path.view());
    if (!parent.empty()) {
      if (auto r = nx::fs::create_directories(parent); !r)
        return r;
    }
    return nx::fs::file_write_atomic(path.view(), data);
  }

  io_result<void> remove(const path_view rel) override {
    return nx::fs::remove(full(rel));
  }

  io_result<void> create_directories(path_view rel) override {
    return nx::fs::create_directories(full(rel));
  }

  [[nodiscard]] const char *name() const noexcept override { return "host"; }

private:
  [[nodiscard]] nx::string full(const path_view rel) const {
    return nx::fs::path::join(m_root.view(), rel.native());
  }

  nx::string m_root;
  HostDeviceAccess m_access = HostDeviceAccess::ReadWrite;
};

} // namespace

Device *make_host_device(const path_view root, const HostDeviceAccess access) {
  return nx::allocate<HostDevice>(nx::string(root.native()), access);
}

} // namespace nx::vfs
