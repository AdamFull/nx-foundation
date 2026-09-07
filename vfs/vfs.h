#pragma once

#include "core/foundation/containers/blob.h"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/core/callable.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/core/handle.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/threading/sync.h"

#include <span>

namespace nx::vfs {

using nx::fs::file_error;
using nx::fs::io_error;
using nx::fs::io_result;
using nx::fs::path_view;
using nx::fs::stream_ptr;

struct FileInfo {
  u64 size = 0;
  u64 mtime_ns = 0;
  bool is_directory = false;
  bool exists = false;
};

struct DirEntry {
  nx::string name;
  FileInfo info;
};

using MountId = nx::basic_handle<u32, 24, struct MountTag>;

using ListFn = nx::function<void(path_view name, const FileInfo &)>;

class Device {
public:
  virtual ~Device() = default;

  Device(const Device &) = delete;
  Device &operator=(const Device &) = delete;

  [[nodiscard]] virtual io_result<nx::blob<u8>> read(path_view rel) = 0;
  [[nodiscard]] virtual io_result<stream_ptr> open(path_view rel) = 0;
  [[nodiscard]] virtual FileInfo stat(path_view rel) = 0;
  [[nodiscard]] virtual io_result<void> list(path_view rel_dir,
                                             const ListFn &cb) = 0;

  [[nodiscard]] virtual bool writable() const noexcept { return false; }
  /// Whether stat().mtime_ns can be polled to discover source changes. This is
  /// a capability declaration, not a promise of push notifications.
  [[nodiscard]] virtual bool watchable() const noexcept { return false; }
  /// Resolves a read-only entry to an OS filesystem path when this device is
  /// directly backed by one. VFS keeps this capability provider-neutral;
  /// asynchronous services use it to select native kernel I/O without making
  /// HostDevice part of their public contract.
  [[nodiscard]] virtual bool native_read_path(path_view rel,
                                              nx::string &out) const {
    (void)rel;
    (void)out;
    return false;
  }
  virtual io_result<void> write(const path_view rel,
                                const std::span<const u8> data) {
    (void)rel;
    (void)data;
    return err(file_error{io_error::InvalidArgument, 0});
  }
  virtual io_result<void> remove(const path_view rel) {
    (void)rel;
    return err(file_error{io_error::InvalidArgument, 0});
  }
  virtual io_result<void> create_directories(const path_view rel) {
    (void)rel;
    return err(file_error{io_error::InvalidArgument, 0});
  }

  [[nodiscard]] virtual const char *name() const noexcept = 0;

protected:
  Device() = default;
};

class MemoryDevice final : public Device {
public:
  MemoryDevice() = default;
  ~MemoryDevice() override = default;

  void add(path_view rel, nx::blob<u8> bytes);

  [[nodiscard]] io_result<nx::blob<u8>> read(path_view rel) override;
  [[nodiscard]] io_result<stream_ptr> open(path_view rel) override;
  [[nodiscard]] FileInfo stat(path_view rel) override;
  [[nodiscard]] io_result<void> list(path_view rel_dir,
                                     const ListFn &cb) override;

  [[nodiscard]] bool writable() const noexcept override { return true; }
  [[nodiscard]] bool watchable() const noexcept override { return true; }
  io_result<void> write(path_view rel, std::span<const u8> data) override;
  io_result<void> remove(path_view rel) override;
  io_result<void> create_directories(path_view /*rel*/) override { return {}; }

  [[nodiscard]] const char *name() const noexcept override { return "memory"; }

private:
  struct Entry {
    nx::blob<u8> bytes;
    u64 mtime_ns = 0;
  };

  nx::rw_lock m_lock;
  nx::hash_map<nx::string, Entry> m_files;
};

// Device factories (ownership passes to mount())

enum class HostDeviceAccess : u8 { ReadOnly, ReadWrite };

[[nodiscard]] Device *
make_host_device(path_view root,
                 HostDeviceAccess access = HostDeviceAccess::ReadWrite);

[[nodiscard]] MemoryDevice *make_memory_device();

[[nodiscard]] io_result<Device *>
make_zip_device(path_view zip_path, path_view archive_subdir = {});

using ZipStreamFactory = nx::function<io_result<stream_ptr>()>;
[[nodiscard]] io_result<Device *>
make_zip_device(ZipStreamFactory stream_factory, path_view archive_subdir = {});

/// Creates a ZIP device that intentionally owns the complete archive in RAM.
/// Prefer the path or stream-factory overload for production asset packs.
[[nodiscard]] io_result<Device *>
make_zip_device(nx::blob<u8> zip_data, path_view archive_subdir = {});

enum class ZipCompression : u16 { Store = 0, Deflate = 8, Zstd = 93 };

/// Produces archives a ZipDevice can mount. Single-writer, not thread-safe.
class ZipWriter {
public:
  ZipWriter() noexcept = default;
  ~ZipWriter();

  ZipWriter(const ZipWriter &) = delete;
  ZipWriter &operator=(const ZipWriter &) = delete;

  [[nodiscard]] bool open(path_view path);
  io_result<void> add(path_view rel, std::span<const u8> data,
                      ZipCompression method = ZipCompression::Deflate) const;
  io_result<void> close();

private:
  void *m_handle = nullptr;
};

bool initialize();
void shutdown();
[[nodiscard]] bool initialized() noexcept;

/// Provider-agnostic metadata for editor/tool discovery. Host backing paths
/// stay private: a remote client addresses resources only by virtual path.
struct MountInfo {
  i32 priority = 0;
  nx::string prefix;
  nx::string provider;
  bool writable = false;
  bool watchable = false;
};

[[nodiscard]] nx::vector<MountInfo> mounts();

/// Mounts @p device at @p virtual_prefix; higher @p priority wins. Takes
/// ownership even when the call fails, so a factory result can be passed
/// straight in. Invalid MountId if initialize() has not run.
MountId mount(path_view virtual_prefix, Device *device, i32 priority = 0);
void unmount(MountId id);
void unmount_all();

[[nodiscard]] io_result<nx::blob<u8>> read(path_view vpath);
[[nodiscard]] io_result<nx::string> read_text(path_view vpath);
[[nodiscard]] io_result<stream_ptr> open(path_view vpath);
[[nodiscard]] FileInfo stat(path_view vpath);
[[nodiscard]] bool exists(path_view vpath);
[[nodiscard]] io_result<nx::vector<DirEntry>> list(path_view vdir);

/// Stable hot-reload generation from a virtual path and its observable file
/// metadata. Centralising this avoids each subsystem growing its own hash
/// constants and subtly different missing-file handling.
[[nodiscard]] u64 file_generation(path_view vpath,
                                  const FileInfo &info) noexcept;
[[nodiscard]] u64 file_generation(path_view vpath) noexcept;
[[nodiscard]] u64
files_generation(std::span<const nx::string> vpaths) noexcept;

io_result<void> write(path_view vpath, std::span<const u8> data);
io_result<void> write_text(path_view vpath, nx::string_view text);
io_result<void> remove(path_view vpath);
io_result<void> create_directories(path_view vpath);

}
