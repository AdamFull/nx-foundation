#include "core/foundation/vfs/vfs_internal.h"

#include "core/foundation/containers/concurrent_queue.h"
#include "core/foundation/core/scope_guard.h"
#include "core/foundation/diagnostics/log.h"
#include "core/foundation/diagnostics/profiler.h"

#include <mz.h>
#include <mz_strm.h>
#include <mz_strm_buf.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include <array>
#include <limits>

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

struct FoundationZipStream {
  mz_stream base{};
  nx::fs::stream_ptr source;
  i32 error = MZ_OK;
};

[[nodiscard]] FoundationZipStream *foundation_stream(void *stream) noexcept {
  return nx::cast<FoundationZipStream *>(stream);
}

i32 foundation_stream_open(void *stream, const char *, const i32) {
  FoundationZipStream *s = foundation_stream(stream);
  if (!s || !s->source) {
    if (s)
      s->error = MZ_OPEN_ERROR;
    return MZ_OPEN_ERROR;
  }
  if (!s->source->seek(0, nx::fs::seek_origin::begin)) {
    s->error = MZ_SEEK_ERROR;
    return MZ_SEEK_ERROR;
  }
  s->error = MZ_OK;
  return MZ_OK;
}

i32 foundation_stream_is_open(void *stream) {
  const FoundationZipStream *s = foundation_stream(stream);
  return s && s->source ? MZ_OK : MZ_OPEN_ERROR;
}

i32 foundation_stream_read(void *stream, void *buffer, const i32 size) {
  FoundationZipStream *s = foundation_stream(stream);
  if (!s || !s->source || !buffer || size < 0) {
    if (s)
      s->error = MZ_PARAM_ERROR;
    return MZ_PARAM_ERROR;
  }
  const usize read = s->source->read(buffer, nx::cast<usize>(size));
  if (read > nx::cast<usize>(INT32_MAX)) {
    s->error = MZ_READ_ERROR;
    return MZ_READ_ERROR;
  }
  return nx::cast<i32>(read);
}

i32 foundation_stream_write(void *, const void *, const i32) {
  return MZ_SUPPORT_ERROR;
}

i64 foundation_stream_tell(void *stream) {
  FoundationZipStream *s = foundation_stream(stream);
  if (!s || !s->source)
    return MZ_TELL_ERROR;
  const i64 position = s->source->tell();
  if (position < 0)
    s->error = MZ_TELL_ERROR;
  return position < 0 ? MZ_TELL_ERROR : position;
}

i32 foundation_stream_seek(void *stream, const i64 offset, const i32 origin) {
  FoundationZipStream *s = foundation_stream(stream);
  if (!s || !s->source)
    return MZ_SEEK_ERROR;

  nx::fs::seek_origin from;
  switch (origin) {
  case MZ_SEEK_SET:
    from = nx::fs::seek_origin::begin;
    break;
  case MZ_SEEK_CUR:
    from = nx::fs::seek_origin::current;
    break;
  case MZ_SEEK_END:
    from = nx::fs::seek_origin::end;
    break;
  default:
    s->error = MZ_PARAM_ERROR;
    return MZ_PARAM_ERROR;
  }

  if (!s->source->seek(offset, from)) {
    s->error = MZ_SEEK_ERROR;
    return MZ_SEEK_ERROR;
  }
  return MZ_OK;
}

i32 foundation_stream_close(void *stream) {
  FoundationZipStream *s = foundation_stream(stream);
  if (!s)
    return MZ_PARAM_ERROR;
  s->source.reset();
  return MZ_OK;
}

i32 foundation_stream_error(void *stream) {
  const FoundationZipStream *s = foundation_stream(stream);
  return s ? s->error : MZ_PARAM_ERROR;
}

mz_stream_vtbl foundation_stream_vtbl = {
    foundation_stream_open,
    foundation_stream_is_open,
    foundation_stream_read,
    foundation_stream_write,
    foundation_stream_tell,
    foundation_stream_seek,
    foundation_stream_close,
    foundation_stream_error,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

struct ZipReader {
  void *handle = nullptr;
  FoundationZipStream *source = nullptr;
  void *buffered = nullptr;
};

void destroy_reader(ZipReader *reader) {
  if (!reader)
    return;
  if (reader->handle)
    mz_zip_reader_delete(&reader->handle);
  if (reader->buffered) {
    mz_stream_buffered_close(reader->buffered);
    mz_stream_buffered_delete(&reader->buffered);
  }
  if (reader->source)
    nx::release(reader->source);
  nx::release(reader);
}

// What a zip device reads from: the archive's bytes or stream factory, and
// a pool of open readers. The device and each open entry stream share
// it, so a stream still open when its device is unmounted keeps reading.
class ZipArchive {
public:
  explicit ZipArchive(nx::blob<u8> zip_data)
      : m_zip_data(std::move(zip_data)) {}
  explicit ZipArchive(ZipStreamFactory stream_factory)
      : m_stream_factory(std::move(stream_factory)) {}

  ZipArchive(const ZipArchive &) = delete;
  ZipArchive &operator=(const ZipArchive &) = delete;

  ~ZipArchive() {
    ZipReader *reader = nullptr;
    while (m_readers.try_pop(reader))
      destroy_reader(reader);
  }

  static constexpr usize READER_POOL = 8;

  [[nodiscard]] ZipReader *acquire() {
    ZipReader *reader = nullptr;
    if (m_readers.try_pop(reader))
      return reader;

    reader = nx::allocate<ZipReader>();
    if (!reader)
      return nullptr;
    reader->handle = mz_zip_reader_create();
    if (!reader->handle) {
      destroy_reader(reader);
      return nullptr;
    }

    i32 opened = MZ_OPEN_ERROR;
    if (m_stream_factory) {
      auto source = m_stream_factory();
      if (!source) {
        destroy_reader(reader);
        return nullptr;
      }
      reader->source = nx::allocate<FoundationZipStream>();
      if (!reader->source) {
        destroy_reader(reader);
        return nullptr;
      }
      reader->source->base.vtbl = &foundation_stream_vtbl;
      reader->source->source = std::move(source).value();

      reader->buffered = mz_stream_buffered_create();
      if (!reader->buffered) {
        destroy_reader(reader);
        return nullptr;
      }
      mz_stream_set_base(reader->buffered, reader->source);
      opened =
          mz_stream_buffered_open(reader->buffered, nullptr, MZ_OPEN_MODE_READ);
      if (opened == MZ_OK)
        opened = mz_zip_reader_open(reader->handle, reader->buffered);
    } else {
      opened = mz_zip_reader_open_buffer(reader->handle, m_zip_data.data(),
                                         nx::cast<i32>(m_zip_data.size()), 0);
    }
    if (opened != MZ_OK) {
      destroy_reader(reader);
      return nullptr;
    }
    return reader;
  }

  void release_reader(ZipReader *reader) {
    if (!reader)
      return;
    if (!m_readers.try_push(reader))
      destroy_reader(reader);
  }

private:
  nx::blob<u8> m_zip_data;
  ZipStreamFactory m_stream_factory;
  nx::mpmc::Queue<ZipReader *, READER_POOL> m_readers;
};

// Read-only device over a .zip (or an APK).
class ZipDevice final : public Device {
public:
  class EntryStream;

  template <typename Source>
  ZipDevice(Source source, nx::string subdir)
      : m_archive(nx::make_shared<ZipArchive>(std::move(source))),
        m_subdir(std::move(subdir)) {}

  bool build_index() {
    ZipReader *reader = m_archive->acquire();
    void *r = reader ? reader->handle : nullptr;
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
    m_archive->release_reader(reader);
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
    ZipReader *reader = m_archive->acquire();
    void *r = reader ? reader->handle : nullptr;
    if (!r)
      return err(file_error{io_error::OpenFailed, 0});
    defer { m_archive->release_reader(reader); };

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

  [[nodiscard]] io_result<stream_ptr> open(path_view rel) override;

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

  nx::shared_ptr<ZipArchive> m_archive;
  nx::string m_subdir;
  nx::hash_map<nx::string, ZipEntry> m_index;
};

/// A live minizip entry rather than an eagerly materialised BlobStream. This
/// matters for large compressed textures: sequential reads now inflate only
/// the coarse mip tail initially, and continue through finer mips on demand.
class ZipDevice::EntryStream final : public nx::fs::stream {
public:
  EntryStream(nx::shared_ptr<ZipArchive> archive, ZipReader *reader,
              nx::string entry, const u64 size) noexcept
      : m_archive(std::move(archive)), m_reader(reader),
        m_entry(std::move(entry)), m_size(size) {}

  ~EntryStream() override {
    close_entry();
    m_archive->release_reader(m_reader);
  }

  usize read(void *destination, const usize bytes) override {
    if (!m_reader || !m_open || destination == nullptr || bytes == 0 ||
        m_position >= m_size)
      return 0;
    const u64 remaining = m_size - m_position;
    usize wanted = nx::cast<usize>(nx::min<u64>(remaining, bytes));
    usize total = 0;
    auto *output = static_cast<u8 *>(destination);
    while (total < wanted) {
      const i32 chunk = nx::cast<i32>(nx::min<usize>(
          wanted - total, nx::cast<usize>(std::numeric_limits<i32>::max())));
      const i32 count =
          mz_zip_reader_entry_read(m_reader->handle, output + total, chunk);
      if (count <= 0) {
        // The indexed uncompressed size is authoritative. Reaching the codec's
        // end before it is corruption, not a healthy stream that merely needs
        // another read call.
        m_failed = true;
        break;
      }
      total += nx::cast<usize>(count);
      m_position += nx::cast<u64>(count);
    }
    return total;
  }

  bool seek(const i64 offset, const nx::fs::seek_origin origin) override {
    if (!m_reader || m_failed)
      return false;
    i64 base = 0;
    switch (origin) {
    case nx::fs::seek_origin::begin:
      break;
    case nx::fs::seek_origin::current:
      if (m_position > nx::cast<u64>(std::numeric_limits<i64>::max()))
        return false;
      base = nx::cast<i64>(m_position);
      break;
    case nx::fs::seek_origin::end:
      if (m_size > nx::cast<u64>(std::numeric_limits<i64>::max()))
        return false;
      base = nx::cast<i64>(m_size);
      break;
    }
    if ((offset > 0 && base > std::numeric_limits<i64>::max() - offset) ||
        (offset < 0 && base < std::numeric_limits<i64>::min() - offset))
      return false;
    const i64 signed_target = base + offset;
    if (signed_target < 0 || nx::cast<u64>(signed_target) > m_size)
      return false;
    const u64 target = nx::cast<u64>(signed_target);
    if (target < m_position && !reopen())
      return false;

    std::array<u8, 64u * 1024u> discard{};
    while (m_position < target) {
      const usize count =
          nx::cast<usize>(nx::min<u64>(discard.size(), target - m_position));
      if (read(discard.data(), count) != count)
        return false;
    }
    return true;
  }

  [[nodiscard]] i64 tell() const override {
    return m_position <= nx::cast<u64>(std::numeric_limits<i64>::max())
               ? nx::cast<i64>(m_position)
               : -1;
  }

  [[nodiscard]] i64 size() const override {
    return m_size <= nx::cast<u64>(std::numeric_limits<i64>::max())
               ? nx::cast<i64>(m_size)
               : -1;
  }

  [[nodiscard]] bool eof() const override {
    return m_failed || m_position >= m_size;
  }

private:
  void close_entry() noexcept {
    if (m_reader && m_open) {
      (void)mz_zip_reader_entry_close(m_reader->handle);
      m_open = false;
    }
  }

  [[nodiscard]] bool reopen() {
    close_entry();
    if (mz_zip_reader_locate_entry(m_reader->handle, m_entry.c_str(), 0) !=
            MZ_OK ||
        mz_zip_reader_entry_open(m_reader->handle) != MZ_OK) {
      m_failed = true;
      return false;
    }
    m_open = true;
    m_position = 0;
    return true;
  }

  nx::shared_ptr<ZipArchive> m_archive;
  ZipReader *m_reader = nullptr;
  nx::string m_entry;
  u64 m_size = 0;
  u64 m_position = 0;
  bool m_open = true;
  bool m_failed = false;
};

io_result<stream_ptr> ZipDevice::open(const path_view rel) {
  const nx::string key = canon_rel(rel);
  const auto found = m_index.find(key);
  if (found == m_index.end() || found->second.is_dir)
    return err(file_error{io_error::NotFound, 0});
  if (found->second.size > nx::cast<u64>(std::numeric_limits<i64>::max()))
    return err(file_error{io_error::FileTooLarge, 0});

  const nx::string full = archive_name(key.view());
  ZipReader *reader = m_archive->acquire();
  if (!reader)
    return err(file_error{io_error::OpenFailed, 0});
  if (mz_zip_reader_locate_entry(reader->handle, full.c_str(), 0) != MZ_OK ||
      mz_zip_reader_entry_open(reader->handle) != MZ_OK) {
    m_archive->release_reader(reader);
    return err(file_error{io_error::NotFound, 0});
  }

  auto *entry =
      nx::allocate<EntryStream>(m_archive, reader, full, found->second.size);
  if (!entry) {
    (void)mz_zip_reader_entry_close(reader->handle);
    m_archive->release_reader(reader);
    return err(file_error{io_error::OpenFailed, 0});
  }
  return stream_ptr(entry);
}

} // namespace

io_result<Device *> make_zip_device(path_view zip_path,
                                    const path_view archive_subdir) {
  // One open file for every reader, so the archive stays what was mounted
  // even when the file is replaced on disk.
  auto views = nx::fs::open_file_views(zip_path);
  if (!views) {
    nx::logw(log_vfs, "vfs: cannot open zip '{}'", zip_path);
    return err(views.error());
  }
  nx::string subdir = canon_rel(archive_subdir);
  auto *dev = nx::allocate<ZipDevice>(std::move(*views), std::move(subdir));
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

io_result<Device *> make_zip_device(ZipStreamFactory stream_factory,
                                    const path_view archive_subdir) {
  if (!stream_factory)
    return err(file_error{io_error::InvalidArgument, 0});
  nx::string subdir = canon_rel(archive_subdir);
  auto *dev =
      nx::allocate<ZipDevice>(std::move(stream_factory), std::move(subdir));
  if (!dev)
    return err(file_error{io_error::OpenFailed, 0});
  if (!dev->build_index()) {
    nx::logw(log_vfs, "vfs: cannot open zip from stream factory");
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

} // namespace nx::vfs
