#include "foundation/vfs/vfs.h"

#include "foundation/strings/format.h"

#include <atomic>

namespace nx::vfs {

namespace {

/// Backed by a real directory: every operation joins the root with the request
/// path and defers to nx::fs. The only writable device.
///
/// stat()/list() leave size and mtime at 0 - nx::fs has no cheap query for
/// them yet. The memory and zip devices, which know their sizes, fill them in.
class HostDevice final : public Device {
public:
  explicit HostDevice(nx::string root) : m_root(std::move(root)) {}

  [[nodiscard]] io_result<nx::blob<u8>> read(const path_view rel) override {
    return nx::fs::file_read(full(rel));
  }

  [[nodiscard]] io_result<stream_ptr> open(const path_view rel) override {
    return nx::fs::open_file(full(rel));
  }

  [[nodiscard]] FileInfo stat(const path_view rel) override {
    const nx::fs::file_type t = nx::fs::status(full(rel));
    FileInfo info;
    info.exists =
        t != nx::fs::file_type::not_found && t != nx::fs::file_type::none;
    info.is_directory = t == nx::fs::file_type::directory;
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

  [[nodiscard]] bool writable() const noexcept override { return true; }

  io_result<void> write(const path_view rel,
                        const std::span<const u8> data) override {
    const nx::string path = full(rel);
    const nx::string_view parent = nx::fs::path::parent_path(path.view());
    if (!parent.empty()) {
      if (auto r = nx::fs::create_directories(parent); !r)
        return r;
    }
    // Write to a temp file and rename it into place, so a reader never sees a
    // half-written file and a crash can't corrupt an existing one. The counter
    // keeps two threads writing the same path off a shared temp file.
    static std::atomic<u64> temp_seq{0};
    const u64 seq = temp_seq.fetch_add(1, std::memory_order_relaxed);
    const nx::string tmp = nx::format("{}.{}.nxtmp", path.view(), seq);
    if (auto r = nx::fs::file_write(tmp.view(), data); !r)
      return r;
    if (auto r = nx::fs::rename(tmp.view(), path.view()); !r) {
      (void)nx::fs::remove(tmp.view()); // best-effort cleanup
      return r;
    }
    return {};
  }

  io_result<void> remove(const path_view rel) override {
    return nx::fs::remove(full(rel));
  }

  io_result<void> create_directories(path_view rel) override {
    return nx::fs::create_directories(full(rel));
  }

  [[nodiscard]] const char *name() const noexcept override { return "host"; }

private:
  // path::join treats an empty tail as the root itself, and a non-absolute rel
  // can never replace the root.
  [[nodiscard]] nx::string full(const path_view rel) const {
    return nx::fs::path::join(m_root.view(), rel.native());
  }

  nx::string m_root;
};

} // namespace

Device *make_host_device(const path_view root) {
  return nx::allocate<HostDevice>(nx::string(root.native()));
}

} // namespace nx::vfs
