#pragma once

/**
 * @file vfs.h
 * @brief Virtual file system: one mountable namespace over many backends.
 *
 * Process-wide, like nx::log: initialize() once, mount devices, then read by
 * virtual path from anywhere. State lives in vfs.cpp.
 *
 * Paths are `/`-rooted and resolved through an ordered mount table. A lookup
 * walks the matching mounts from highest priority down and takes the first hit,
 * so a loose-files host mount transparently overrides a shipped archive (dev
 * overlays, patches, DLC).
 *
 * There are no platform backends here: Android mounts its APK through the
 * ordinary ZipDevice, and only *finding* the APK path is runtime code.
 *
 * Reads are safe from any thread.
 */

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
  u64 mtime_ns = 0; ///< 0 if unknown.
  bool is_directory = false;
  bool exists = false;
};

struct DirEntry {
  nx::string name;
  FileInfo info;
};

/// Keys a mount in the table. Default-constructs to null, and a handle left
/// over from an unmounted entry reports as absent.
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
  /// Enumerates the direct children of a directory, calling @p cb for each.
  [[nodiscard]] virtual io_result<void> list(path_view rel_dir,
                                             const ListFn &cb) = 0;

  [[nodiscard]] virtual bool writable() const noexcept { return false; }
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

  /// Inserts or replaces a file.
  void add(path_view rel, nx::blob<u8> bytes);

  [[nodiscard]] io_result<nx::blob<u8>> read(path_view rel) override;
  [[nodiscard]] io_result<stream_ptr> open(path_view rel) override;
  [[nodiscard]] FileInfo stat(path_view rel) override;
  [[nodiscard]] io_result<void> list(path_view rel_dir,
                                     const ListFn &cb) override;

  [[nodiscard]] bool writable() const noexcept override { return true; }
  io_result<void> write(path_view rel, std::span<const u8> data) override;
  io_result<void> remove(path_view rel) override;
  io_result<void> create_directories(path_view /*rel*/) override { return {}; }

  [[nodiscard]] const char *name() const noexcept override { return "memory"; }

private:
  nx::rw_lock m_lock;
  nx::hash_map<nx::string, nx::blob<u8>> m_files;
};

// Device factories (ownership passes to mount())

/// Native-filesystem device rooted at @p root. Writable.
[[nodiscard]] Device *make_host_device(path_view root);

[[nodiscard]] MemoryDevice *make_memory_device();

/// Opens and indexes a ZIP. @p archive_subdir, if set, mounts only
/// that subtree, so its entries appear at the mount root.
[[nodiscard]] io_result<Device *>
make_zip_device(path_view zip_path, path_view archive_subdir = {});

/// Zstd only where minizip-ng was built with it.
enum class ZipCompression : u16 { Store = 0, Deflate = 8, Zstd = 93 };

/// Produces archives a ZipDevice can mount. Single-writer, not thread-safe.
class ZipWriter {
public:
  ZipWriter() noexcept = default;
  ~ZipWriter();

  ZipWriter(const ZipWriter &) = delete;
  ZipWriter &operator=(const ZipWriter &) = delete;

  /// Creates (truncating) the archive at @p path.
  [[nodiscard]] bool open(path_view path);
  io_result<void> add(path_view rel, std::span<const u8> data,
                      ZipCompression method = ZipCompression::Deflate) const;
  io_result<void> close();

private:
  void *m_handle = nullptr; // mz_zip_writer
};

// The file system

/// Idempotent; everything below is inert until this succeeds.
bool initialize();
/// Unmounts everything, destroying the devices. Safe when already down.
void shutdown();
[[nodiscard]] bool initialized() noexcept;

// Mount table

/// Mounts @p device at @p virtual_prefix; higher @p priority wins. Takes
/// ownership even when the call fails, so a factory result can be passed
/// straight in. Invalid MountId if initialize() has not run.
MountId mount(path_view virtual_prefix, Device *device, i32 priority = 0);
/// Destroys the device, so no other thread may be reading through it.
void unmount(MountId id);
void unmount_all();

// Reading

[[nodiscard]] io_result<nx::blob<u8>> read(path_view vpath);
[[nodiscard]] io_result<nx::string> read_text(path_view vpath);
[[nodiscard]] io_result<stream_ptr> open(path_view vpath);
[[nodiscard]] FileInfo stat(path_view vpath);
[[nodiscard]] bool exists(path_view vpath);
/// Merged across every matching mount; a higher-priority entry shadows a
/// same-named one below it.
[[nodiscard]] io_result<nx::vector<DirEntry>> list(path_view vdir);

// Writing (routes to the highest-priority writable mount)

io_result<void> write(path_view vpath, std::span<const u8> data);
io_result<void> write_text(path_view vpath, nx::string_view text);
io_result<void> remove(path_view vpath);
io_result<void> create_directories(path_view vpath);

} // namespace nx::vfs
