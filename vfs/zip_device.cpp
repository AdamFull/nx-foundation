#include "core/foundation/vfs/vfs_internal.h"

#include "core/foundation/containers/concurrent_queue.h"
#include "core/foundation/core/scope_guard.h"
#include "core/foundation/diagnostics/log.h"
#include "core/foundation/diagnostics/profiler.h"

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

namespace nx::vfs {

using detail::BlobStream;
using detail::canon_rel;
using detail::ChildSet;

namespace {

const nx::log::Category log_vfs = nx::log::category("vfs");

struct ZipEntry {
  u64 size = 0;
  bool is_dir = false;
};

// Read-only device over a .zip (or an APK).
class ZipDevice final : public Device {
public:
  ZipDevice(nx::string zip_path, nx::string subdir)
      : m_zip_path(std::move(zip_path)), m_subdir(std::move(subdir)) {}
  ZipDevice(nx::blob<u8> zip_data, nx::string subdir)
      : m_zip_data(std::move(zip_data)), m_subdir(std::move(subdir)) {}

  ~ZipDevice() override {
    void *h = nullptr;
    while (m_readers.try_pop(h))
      mz_zip_reader_delete(&h);
  }

  bool build_index() {
    void *r = acquire();
    if (!r)
      return false;
    i32 e = mz_zip_reader_goto_first_entry(r);
    while (e == MZ_OK) {
      mz_zip_file *info = nullptr;
      if (mz_zip_reader_entry_get_info(r, &info) == MZ_OK && info &&
          info->filename) {
        nx::string_view full(info->filename);
        nx::string_view rel;
        if (strip_subdir(full, rel) && !rel.empty()) {
          const bool is_dir = rel.back() == '/';
          nx::string key(is_dir ? rel.substr(0, rel.size() - 1) : rel);
          if (!key.empty()) {
            const i64 sz = info->uncompressed_size;
            m_index[std::move(key)] = ZipEntry{
                .size = nx::cast<u64>(sz < 0 ? 0 : sz), .is_dir = is_dir};
          }
        }
      }
      e = mz_zip_reader_goto_next_entry(r);
    }
    release(r);
    return true;
  }

  [[nodiscard]] io_result<nx::blob<u8>> read(const path_view rel) override {
    NX_PROFILE_ZONE("zip::read");
    const nx::string key = canon_rel(rel);
    const auto it = m_index.find(key);
    if (it == m_index.end() || it->second.is_dir)
      return err(file_error{io_error::NotFound, 0});
    const u64 size = it->second.size;
    if (size > nx::cast<u64>(0x7fffffff))
      return err(file_error{io_error::FileTooLarge, 0});

    const nx::string full = archive_name(key.view());
    void *r = acquire();
    if (!r)
      return err(file_error{io_error::OpenFailed, 0});
    defer { release(r); };

    if (mz_zip_reader_locate_entry(r, full.c_str(), 0) != MZ_OK ||
        mz_zip_reader_entry_open(r) != MZ_OK)
      return err(file_error{io_error::NotFound, 0});

    nx::blob<u8> data(nx::cast<usize>(size));
    usize off = 0;
    bool ok = true;
    while (off < size) {
      const i32 want = nx::cast<i32>(nx::min<u64>(size - off, 1u << 20));
      const i32 n = mz_zip_reader_entry_read(r, data.data() + off, want);
      if (n < 0) {
        ok = false;
        break;
      }
      if (n == 0)
        break;
      off += nx::cast<usize>(n);
    }
    mz_zip_reader_entry_close(r);
    if (!ok || off != size)
      return err(file_error{io_error::ReadFailed, 0});
    return data;
  }

  [[nodiscard]] io_result<stream_ptr> open(const path_view rel) override {
    auto r = read(rel);
    if (!r)
      return err(r.error());
    auto *s = nx::allocate<BlobStream>(std::move(r).value());
    if (!s)
      return err(file_error{io_error::OpenFailed, 0});
    return stream_ptr(s);
  }

  [[nodiscard]] FileInfo stat(const path_view rel) override {
    const nx::string key = canon_rel(rel);
    FileInfo info;
    if (key.empty()) {
      info.exists = true;
      info.is_directory = true;
      return info;
    }
    if (const auto it = m_index.find(key); it != m_index.end()) {
      info.exists = true;
      info.is_directory = it->second.is_dir;
      if (!it->second.is_dir)
        info.size = it->second.size;
      return info;
    }
    nx::string prefix = key;
    prefix.push_back('/');
    for (const auto &[k, v] : m_index) {
      if (k.view().starts_with(prefix.view())) {
        info.exists = true;
        info.is_directory = true;
        break;
      }
    }
    return info;
  }

  [[nodiscard]] io_result<void> list(const path_view rel_dir,
                                     const ListFn &cb) override {
    const nx::string dir = canon_rel(rel_dir);
    const nx::string prefix =
        dir.empty() ? nx::string() : dir + nx::string("/");

    ChildSet seen;
    bool any = false;
    for (const auto &[k, entry] : m_index) {
      const nx::string_view kv = k.view();
      if (!prefix.empty() && !kv.starts_with(prefix.view()))
        continue;
      const nx::string_view rest = kv.substr(prefix.size());
      if (rest.empty())
        continue;
      const usize slash = rest.find('/');
      const bool child_is_dir = slash != nx::string_view::npos || entry.is_dir;
      const nx::string_view child =
          slash != nx::string_view::npos ? rest.substr(0, slash) : rest;
      if (!seen.insert(child))
        continue;
      any = true;
      FileInfo info;
      info.exists = true;
      info.is_directory = child_is_dir;
      if (!child_is_dir)
        info.size = entry.size;
      cb(child, info);
    }
    if (!any && !dir.empty())
      return err(file_error{io_error::NotFound, 0});
    return {};
  }

  [[nodiscard]] const char *name() const noexcept override { return "zip"; }

private:
  static constexpr usize READER_POOL = 8;

  [[nodiscard]] void *acquire() {
    void *h = nullptr;
    if (m_readers.try_pop(h))
      return h;
    h = mz_zip_reader_create();
    if (!h)
      return nullptr;
    const i32 opened = m_zip_data.empty()
                           ? mz_zip_reader_open_file(h, m_zip_path.c_str())
                           : mz_zip_reader_open_buffer(
                                 h, m_zip_data.data(),
                                 nx::cast<i32>(m_zip_data.size()), 0);
    if (opened != MZ_OK) {
      mz_zip_reader_delete(&h);
      return nullptr;
    }
    return h;
  }

  void release(void *h) {
    if (!h)
      return;
    if (!m_readers.try_push(h))
      mz_zip_reader_delete(&h);
  }

  [[nodiscard]] bool strip_subdir(const nx::string_view full,
                                  nx::string_view &out) const {
    if (m_subdir.empty()) {
      out = full;
      return true;
    }
    const nx::string_view sd = m_subdir.view();
    if (full.size() > sd.size() + 1 && full.starts_with(sd) &&
        full[sd.size()] == '/') {
      out = full.substr(sd.size() + 1);
      return true;
    }
    return false;
  }

  [[nodiscard]] nx::string archive_name(const nx::string_view rel) const {
    if (m_subdir.empty())
      return nx::string(rel);
    nx::string s = m_subdir;
    s.push_back('/');
    s.append(rel);
    return s;
  }

  nx::string m_zip_path;
  nx::blob<u8> m_zip_data;
  nx::string m_subdir;
  nx::hash_map<nx::string, ZipEntry> m_index;
  nx::mpmc::Queue<void *, READER_POOL> m_readers;
};

}

io_result<Device *> make_zip_device(path_view zip_path,
                                    const path_view archive_subdir) {
  nx::string subdir = canon_rel(archive_subdir);
  auto *dev =
      nx::allocate<ZipDevice>(nx::string(zip_path.native()), std::move(subdir));
  if (!dev)
    return err(file_error{io_error::OpenFailed, 0});
  if (!dev->build_index()) {
    nx::logw(log_vfs, "vfs: cannot open zip '{}'", zip_path);
    nx::release(dev);
    return err(file_error{io_error::OpenFailed, 0});
  }
  return static_cast<Device *>(dev);
}

io_result<Device *> make_zip_device(nx::blob<u8> zip_data,
                                    const path_view archive_subdir) {
  if (zip_data.empty() || zip_data.size() > nx::cast<usize>(0x7fffffff))
    return err(file_error{io_error::InvalidArgument, 0});
  nx::string subdir = canon_rel(archive_subdir);
  auto *dev = nx::allocate<ZipDevice>(std::move(zip_data), std::move(subdir));
  if (!dev)
    return err(file_error{io_error::OpenFailed, 0});
  if (!dev->build_index()) {
    nx::logw(log_vfs, "vfs: cannot open zip from memory");
    nx::release(dev);
    return err(file_error{io_error::OpenFailed, 0});
  }
  return static_cast<Device *>(dev);
}

ZipWriter::~ZipWriter() {
  if (m_handle) {
    void *h = m_handle;
    mz_zip_writer_delete(&h);
    m_handle = nullptr;
  }
}

bool ZipWriter::open(const path_view path) {
  if (m_handle)
    return false;
  m_handle = mz_zip_writer_create();
  if (!m_handle)
    return false;
  const nx::string p(path.native());
  if (mz_zip_writer_open_file(m_handle, p.c_str(), 0, 0) != MZ_OK) {
    void *h = m_handle;
    mz_zip_writer_delete(&h);
    m_handle = nullptr;
    return false;
  }
  return true;
}

io_result<void> ZipWriter::add(const path_view rel, std::span<const u8> data,
                               ZipCompression method) const {
  if (!m_handle)
    return err(file_error{io_error::InvalidArgument, 0});
  if (data.size() > nx::cast<usize>(0x7fffffff))
    return err(file_error{io_error::FileTooLarge, 0});

  const nx::string name = canon_rel(rel);
  if (name.empty())
    return err(file_error{io_error::InvalidArgument, 0});

  mz_zip_file fi = {};
  fi.filename = name.c_str();
  fi.compression_method = nx::cast<u16>(method);
  fi.flag = MZ_ZIP_FLAG_UTF8;

  mz_zip_writer_set_compress_method(m_handle, nx::cast<u16>(method));
  if (mz_zip_writer_add_buffer(m_handle, const_cast<u8 *>(data.data()),
                               nx::cast<i32>(data.size()), &fi) != MZ_OK)
    return err(file_error{io_error::WriteFailed, 0});
  return {};
}

io_result<void> ZipWriter::close() {
  if (!m_handle)
    return {};
  const i32 rc = mz_zip_writer_close(m_handle);
  void *h = m_handle;
  mz_zip_writer_delete(&h);
  m_handle = nullptr;
  if (rc != MZ_OK)
    return err(file_error{io_error::WriteFailed, 0});
  return {};
}

}
