
#include "framework/nxtest.h"

#include "vfs_fixture.h"

#include "core/foundation/strings/format.h"
#include "core/foundation/threading/thread.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace vfs = nx::vfs;
using vfs_fixture::blob_of;
using vfs_fixture::bytes_of;
using vfs_fixture::has_name;
using vfs_fixture::read_eq;
using vfs_fixture::scoped_vfs;
using vfs_fixture::temp_dir;

namespace {

struct entry {
  const char *name;
  const char *data;
  vfs::ZipCompression method;
};

class CountingMemoryStream final : public nx::fs::stream {
public:
  CountingMemoryStream(const u8 *data, const usize size,
                       std::atomic<u64> *bytes_read) noexcept
      : m_data(data), m_size(size), m_bytes_read(bytes_read) {}

  usize read(void *dst, const usize bytes) override {
    if (!dst || m_position >= m_size)
      return 0;
    const usize count = nx::min(bytes, m_size - m_position);
    std::memcpy(dst, m_data + m_position, count);
    m_position += count;
    m_bytes_read->fetch_add(count, std::memory_order_relaxed);
    return count;
  }

  bool seek(const i64 offset, const nx::fs::seek_origin origin) override {
    i64 base = 0;
    switch (origin) {
    case nx::fs::seek_origin::begin:
      break;
    case nx::fs::seek_origin::current:
      base = nx::cast<i64>(m_position);
      break;
    case nx::fs::seek_origin::end:
      base = nx::cast<i64>(m_size);
      break;
    }
    if (offset < -base || offset > nx::cast<i64>(m_size) - base)
      return false;
    m_position = nx::cast<usize>(base + offset);
    return true;
  }

  [[nodiscard]] i64 tell() const override { return nx::cast<i64>(m_position); }
  [[nodiscard]] i64 size() const override { return nx::cast<i64>(m_size); }
  [[nodiscard]] bool eof() const override { return m_position >= m_size; }

private:
  const u8 *m_data = nullptr;
  usize m_size = 0;
  usize m_position = 0;
  std::atomic<u64> *m_bytes_read = nullptr;
};

/// A stream over bytes that counts how many of its kind are open.
class TrackedStream final : public nx::fs::stream {
public:
  TrackedStream(const u8 *data, const usize size,
                std::atomic<i32> *open) noexcept
      : m_data(data), m_size(size), m_open(open) {
    m_open->fetch_add(1);
  }
  ~TrackedStream() override { m_open->fetch_sub(1); }

  usize read(void *dst, const usize bytes) override {
    if (!dst || m_position >= m_size)
      return 0;
    const usize count = nx::min(bytes, m_size - m_position);
    std::memcpy(dst, m_data + m_position, count);
    m_position += count;
    return count;
  }
  bool seek(const i64 offset, const nx::fs::seek_origin origin) override {
    const i64 base = origin == nx::fs::seek_origin::begin ? 0
                     : origin == nx::fs::seek_origin::current
                         ? nx::cast<i64>(m_position)
                         : nx::cast<i64>(m_size);
    if (offset < -base || offset > nx::cast<i64>(m_size) - base)
      return false;
    m_position = nx::cast<usize>(base + offset);
    return true;
  }
  [[nodiscard]] i64 tell() const override { return nx::cast<i64>(m_position); }
  [[nodiscard]] i64 size() const override { return nx::cast<i64>(m_size); }
  [[nodiscard]] bool eof() const override { return m_position >= m_size; }

private:
  const u8 *m_data = nullptr;
  usize m_size = 0;
  usize m_position = 0;
  std::atomic<i32> *m_open = nullptr;
};

bool make_zip(const nx::string &path, std::initializer_list<entry> entries) {
  vfs::ZipWriter w;
  if (!w.open(path.view()))
    return false;
  for (const entry &e : entries)
    if (!w.add(e.name, bytes_of(e.data), e.method).has_value())
      return false;
  return w.close().has_value();
}

} // namespace

TEST_CASE("vfs zip round-trips supported compression methods") {
  temp_dir dir("zip");
  const nx::string zip = dir.file("test.zip");
  REQUIRE(
      make_zip(zip, {{"hello.txt", "ZIPPED", vfs::ZipCompression::Deflate},
                     {"sub/x.bin", "BINARYDATA", vfs::ZipCompression::Store},
                     {"zstd.bin", "ZSTANDARD", vfs::ZipCompression::Zstd}}));

  auto zd = vfs::make_zip_device(zip.view());
  REQUIRE(zd.has_value());
  scoped_vfs guard;
  vfs::mount("/", *zd, 0);

  SUBCASE("deflated entry") { CHECK(read_eq("/hello.txt", "ZIPPED")); }
  SUBCASE("stored entry") { CHECK(read_eq("/sub/x.bin", "BINARYDATA")); }
  SUBCASE("zstd entry") { CHECK(read_eq("/zstd.bin", "ZSTANDARD")); }

  SUBCASE("metadata") {
    CHECK(vfs::stat("/hello.txt").exists);
    CHECK(vfs::stat("/hello.txt").size == 6);
    CHECK_FALSE(vfs::stat("/hello.txt").is_directory);
    CHECK(vfs::stat("/sub").is_directory);
    CHECK_FALSE(vfs::exists("/missing.txt"));
  }

  SUBCASE("listing") {
    auto ls = vfs::list("/");
    REQUIRE(ls.has_value());
    CHECK(has_name(*ls, "hello.txt"));
    CHECK(has_name(*ls, "sub"));
    auto sub = vfs::list("/sub");
    REQUIRE(sub.has_value());
    CHECK(has_name(*sub, "x.bin"));
  }

  SUBCASE("the archive is read-only") {
    CHECK_FALSE(vfs::write("/hello.txt", bytes_of("nope")).has_value());
    CHECK_FALSE(vfs::remove("/hello.txt").has_value());
  }

  SUBCASE("open streams a decoded entry") {
    auto s = vfs::open("/hello.txt");
    REQUIRE(s.has_value());
    CHECK((*s)->size() == 6);
    char buf[6] = {};
    CHECK((*s)->read(buf, 6) == 6);
    CHECK(std::memcmp(buf, "ZIPPED", 6) == 0);
  }

  SUBCASE("entry streams seek forward and reopen for backward seeks") {
    auto s = vfs::open("/zstd.bin");
    REQUIRE(s.has_value());
    REQUIRE((*s)->seek(3, nx::fs::seek_origin::begin));
    char middle[4]{};
    REQUIRE((*s)->read(middle, sizeof(middle)) == sizeof(middle));
    CHECK(std::memcmp(middle, "ANDA", sizeof(middle)) == 0);

    REQUIRE((*s)->seek(-3, nx::fs::seek_origin::end));
    char tail[3]{};
    REQUIRE((*s)->read(tail, sizeof(tail)) == sizeof(tail));
    CHECK(std::memcmp(tail, "ARD", sizeof(tail)) == 0);

    REQUIRE((*s)->seek(0, nx::fs::seek_origin::begin));
    char first{};
    REQUIRE((*s)->read(&first, 1) == 1);
    CHECK(first == 'Z');
  }
}

TEST_CASE("vfs zip archive_subdir exposes only that subtree") {
  temp_dir dir("apk");
  const nx::string zip = dir.file("apk.zip");
  REQUIRE(
      make_zip(zip, {{"assets/s/shader.spv", "SPV", vfs::ZipCompression::Store},
                     {"assets/tex.png", "PNG", vfs::ZipCompression::Deflate},
                     {"META-INF/MANIFEST.MF", "mf", vfs::ZipCompression::Store},
                     {"classes.dex", "dex", vfs::ZipCompression::Store}}));

  auto zd = vfs::make_zip_device(zip.view(), "assets");
  REQUIRE(zd.has_value());
  scoped_vfs guard;
  vfs::mount("/", *zd, 0);

  SUBCASE("the prefix is stripped from mounted paths") {
    CHECK(read_eq("/s/shader.spv", "SPV"));
    CHECK(read_eq("/tex.png", "PNG"));
  }

  SUBCASE("sibling trees stay hidden") {
    CHECK_FALSE(vfs::read("/META-INF/MANIFEST.MF").has_value());
    CHECK_FALSE(vfs::read("/classes.dex").has_value());
    CHECK_FALSE(vfs::exists("/assets/tex.png"));
  }

  SUBCASE("listing only shows the subtree") {
    auto ls = vfs::list("/");
    REQUIRE(ls.has_value());
    CHECK(has_name(*ls, "s"));
    CHECK(has_name(*ls, "tex.png"));
    CHECK_FALSE(has_name(*ls, "META-INF"));
    CHECK_FALSE(has_name(*ls, "classes.dex"));
  }
}

TEST_CASE("vfs zip mounts an archive held in memory") {
  temp_dir dir("zipmem");
  const nx::string zip = dir.file("memory.zip");
  REQUIRE(
      make_zip(zip, {{"packed.txt", "MEMORY", vfs::ZipCompression::Deflate}}));
  auto bytes = nx::fs::file_read(zip.view());
  REQUIRE(bytes.has_value());

  auto zd = vfs::make_zip_device(std::move(bytes).value());
  REQUIRE(zd.has_value());
  scoped_vfs guard;
  vfs::mount("/", *zd, 0);

  CHECK(read_eq("/packed.txt", "MEMORY"));
}

TEST_CASE("vfs zip stream factory reads a large archive on demand") {
  temp_dir dir("zipstream");
  const nx::string zip = dir.file("stream.zip");
  nx::blob<u8> large(8u * 1024u * 1024u);
  std::memset(large.data(), 0x5a, large.size());

  vfs::ZipWriter writer;
  REQUIRE(writer.open(zip.view()));
  REQUIRE(writer
              .add("large.bin", std::span<const u8>(large.data(), large.size()),
                   vfs::ZipCompression::Store)
              .has_value());
  REQUIRE(writer.add("small.txt", bytes_of("SMALL"), vfs::ZipCompression::Store)
              .has_value());
  REQUIRE(writer.close().has_value());

  auto archive = nx::fs::file_read(zip.view());
  REQUIRE(archive.has_value());
  REQUIRE(archive->size() > large.size());
  std::atomic<u64> bytes_read{0};
  std::atomic<usize> streams_opened{0};
  const u8 *const archive_data = archive->data();
  const usize archive_size = archive->size();

  vfs::ZipStreamFactory factory(
      [&bytes_read, &streams_opened, archive_data,
       archive_size]() -> nx::fs::io_result<nx::fs::stream_ptr> {
        streams_opened.fetch_add(1, std::memory_order_relaxed);
        auto *stream = nx::allocate<CountingMemoryStream>(
            archive_data, archive_size, &bytes_read);
        if (!stream)
          return nx::err(nx::fs::file_error{nx::fs::io_error::OpenFailed, 0});
        return nx::fs::stream_ptr(stream);
      });

  auto zd = vfs::make_zip_device(std::move(factory));
  REQUIRE(zd.has_value());
  CHECK(streams_opened.load(std::memory_order_relaxed) == 1);
  // Indexing reads the ZIP tail/central directory, not the stored 8 MiB entry.
  CHECK(bytes_read.load(std::memory_order_relaxed) < archive_size / 2);

  scoped_vfs guard;
  vfs::mount("/", *zd, 0);
  CHECK(read_eq("/small.txt", "SMALL"));
  CHECK(bytes_read.load(std::memory_order_relaxed) < archive_size / 2);
}

TEST_CASE("vfs zip writer canonicalizes entry names") {
  temp_dir dir("zipname");
  const nx::string zip = dir.file("names.zip");
  REQUIRE(
      make_zip(zip, {{"/lead.txt", "LEAD", vfs::ZipCompression::Store},
                     {"dir\\win.txt", "WIN", vfs::ZipCompression::Store},
                     {"a/./b/../c.txt", "MESSY", vfs::ZipCompression::Store}}));

  auto zd = vfs::make_zip_device(zip.view());
  REQUIRE(zd.has_value());
  scoped_vfs guard;
  vfs::mount("/", *zd, 0);

  CHECK(read_eq("/lead.txt", "LEAD"));
  CHECK(read_eq("/dir/win.txt", "WIN"));
  CHECK(read_eq("/a/c.txt", "MESSY"));

  SUBCASE("a name that escapes the archive root is refused") {
    vfs::ZipWriter w;
    REQUIRE(w.open(dir.file("evil.zip").view()));
    CHECK_FALSE(w.add("../escape.txt", bytes_of("x")).has_value());
    CHECK_FALSE(w.add("/", bytes_of("x")).has_value());
    CHECK(w.close().has_value());
  }
}

TEST_CASE("vfs zip device fails to open a missing or invalid archive") {
  temp_dir dir("zipbad");
  CHECK_FALSE(vfs::make_zip_device(dir.file("nope.zip").view()).has_value());

  const nx::string junk = dir.file("junk.zip");
  REQUIRE(nx::fs::file_write(junk.view(), bytes_of("not a zip at all"))
              .has_value());
  CHECK_FALSE(vfs::make_zip_device(junk.view()).has_value());
}

TEST_CASE("vfs zip handles an empty archive") {
  temp_dir dir("zipempty");
  const nx::string zip = dir.file("empty.zip");
  REQUIRE(make_zip(zip, {}));

  auto zd = vfs::make_zip_device(zip.view());
  REQUIRE(zd.has_value());
  scoped_vfs guard;
  vfs::mount("/", *zd, 0);
  CHECK_FALSE(vfs::exists("/anything"));
  auto ls = vfs::list("/");
  CHECK(ls.has_value());
  CHECK(ls->empty());
}

TEST_CASE("vfs zip serves concurrent reads from its handle pool") {
  temp_dir dir("zippar");
  const nx::string zip = dir.file("par.zip");
  REQUIRE(make_zip(zip, {{"a.txt", "AAAA", vfs::ZipCompression::Deflate},
                         {"b.txt", "BBBB", vfs::ZipCompression::Store}}));

  auto zd = vfs::make_zip_device(zip.view());
  REQUIRE(zd.has_value());
  scoped_vfs guard;
  vfs::mount("/", *zd, 0);

  constexpr usize kThreads = 12;
  constexpr usize kPerThread = 16;
  std::atomic<usize> ok{0};
  nx::vector<nx::thread> readers;
  readers.reserve(kThreads);
  for (usize t = 0; t < kThreads; ++t)
    readers.push_back(nx::thread([&, t] {
      for (usize i = 0; i < kPerThread; ++i) {
        auto r = vfs::read(((t + i) % 2) ? "/a.txt" : "/b.txt");
        if (r.has_value() && r->size() == 4)
          ok.fetch_add(1, std::memory_order_relaxed);
      }
    }));
  for (nx::thread &r : readers)
    r.join();

  CHECK(ok.load() == kThreads * kPerThread);
}

TEST_CASE("vfs zip is shadowed by a higher-priority overlay") {
  temp_dir dir("overlay");
  const nx::string zip = dir.file("base.zip");
  REQUIRE(make_zip(zip, {{"a.txt", "ZIP", vfs::ZipCompression::Store},
                         {"zonly.txt", "ZONLY", vfs::ZipCompression::Store}}));

  scoped_vfs guard;
  auto zd = vfs::make_zip_device(zip.view());
  REQUIRE(zd.has_value());
  vfs::mount("/", *zd, 0);

  vfs::MemoryDevice *patch = vfs::make_memory_device();
  patch->add("a.txt", blob_of("PATCHED"));
  vfs::mount("/", patch, 100);

  CHECK(read_eq("/a.txt", "PATCHED"));
  CHECK(read_eq("/zonly.txt", "ZONLY"));
}

TEST_CASE("vfs zip stream keeps reading after its device is unmounted") {
  temp_dir dir("zipunmount");
  const nx::string zip = dir.file("overlay.zip");
  nx::string text;
  for (int i = 0; i < 4096; ++i)
    text.append("overlay stream payload ");
  REQUIRE(make_zip(
      zip, {{"entry.txt", text.c_str(), vfs::ZipCompression::Deflate}}));
  auto archive = nx::fs::file_read(zip.view());
  REQUIRE(archive.has_value());
  std::atomic<i32> open{0};
  const u8 *const data = archive->data();
  const usize size = archive->size();
  auto zd = vfs::make_zip_device(vfs::ZipStreamFactory(
      [&open, data, size]() -> nx::fs::io_result<nx::fs::stream_ptr> {
        return nx::fs::stream_ptr(
            nx::allocate<TrackedStream>(data, size, &open));
      }));
  REQUIRE(zd.has_value());

  scoped_vfs guard;
  const vfs::MountId id = vfs::mount("/", *zd, 0);
  {
    auto stream = vfs::open("/entry.txt");
    REQUIRE(stream.has_value());
    nx::string head(64, '\0');
    REQUIRE((*stream)->read(head.data(), head.size()) == head.size());

    vfs::unmount(id);
    CHECK_FALSE(vfs::exists("/entry.txt"));
    // The device is gone; the stream still has the archive open.
    CHECK(open.load() >= 1);

    nx::string rest(text.size() - head.size(), '\0');
    CHECK((*stream)->read(rest.data(), rest.size()) == rest.size());
    head.append(rest.view());
    CHECK(head == text);
  }
  // With the last stream closed, nothing of the archive stays open.
  CHECK(open.load() == 0);
}

TEST_CASE("vfs zip keeps its archive when the file is replaced on disk") {
  temp_dir dir("zipreplace");
  const nx::string folder = dir.file("mod");
  REQUIRE(nx::fs::create_directories(folder.view()).has_value());
  const nx::string zip = nx::format("{}/companion.nxp", folder);
  REQUIRE(make_zip(zip, {{"a.txt", "OLD A", vfs::ZipCompression::Deflate},
                         {"b.txt", "OLD B", vfs::ZipCompression::Store}}));

  auto zd = vfs::make_zip_device(zip.view());
  REQUIRE(zd.has_value());
  scoped_vfs guard;
  vfs::mount("/", *zd, 0);
  CHECK(read_eq("/a.txt", "OLD A"));

  // What an installer does to update a mounted package: remove its folder
  // and write the new version at the same path.
  REQUIRE(nx::fs::remove_all(folder.view()).has_value());
  REQUIRE(nx::fs::create_directories(folder.view()).has_value());
  REQUIRE(make_zip(zip, {{"a.txt", "NEW A", vfs::ZipCompression::Store}}));

  // Two streams at once, so one reads through a reader opened after.
  auto first = vfs::open("/a.txt");
  auto second = vfs::open("/b.txt");
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  nx::string a(5, '\0');
  nx::string b(5, '\0');
  CHECK((*first)->read(a.data(), a.size()) == a.size());
  CHECK((*second)->read(b.data(), b.size()) == b.size());
  CHECK(a == "OLD A");
  CHECK(b == "OLD B");

  auto fresh = vfs::make_zip_device(zip.view());
  REQUIRE(fresh.has_value());
  vfs::mount("/new", *fresh, 0);
  CHECK(read_eq("/new/a.txt", "NEW A"));
  CHECK_FALSE(vfs::exists("/new/b.txt"));
}

namespace {

/// Stored entries written byte by byte, for archives ZipWriter refuses to
/// make: wrong CRCs, forged sizes, names that climb out.
struct raw_entry {
  std::string name;
  std::string data;
  u32 crc = 0;
  u32 declared = 0;
};

u32 crc32_of(const std::string_view s) {
  u32 c = ~0u;
  for (const unsigned char b : s) {
    c ^= b;
    for (int k = 0; k < 8; ++k)
      c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
  }
  return ~c;
}

raw_entry stored(std::string name, std::string data) {
  const u32 crc = crc32_of(data);
  const auto size = nx::cast<u32>(data.size());
  return {std::move(name), std::move(data), crc, size};
}

void put16(std::string &o, const u32 v) {
  o.push_back(nx::cast<char>(v & 0xffu));
  o.push_back(nx::cast<char>((v >> 8) & 0xffu));
}

void put32(std::string &o, const u32 v) {
  put16(o, v & 0xffffu);
  put16(o, v >> 16);
}

std::string raw_zip(const std::vector<raw_entry> &entries) {
  std::string out;
  std::string central;
  for (const raw_entry &e : entries) {
    const auto offset = nx::cast<u32>(out.size());
    const auto stored_size = nx::cast<u32>(e.data.size());
    const auto name_size = nx::cast<u32>(e.name.size());
    put32(out, 0x04034b50u);
    put16(out, 20);
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    put16(out, 0x21);
    put32(out, e.crc);
    put32(out, stored_size);
    put32(out, e.declared);
    put16(out, name_size);
    put16(out, 0);
    out += e.name;
    out += e.data;

    put32(central, 0x02014b50u);
    put16(central, 20);
    put16(central, 20);
    put16(central, 0);
    put16(central, 0);
    put16(central, 0);
    put16(central, 0x21);
    put32(central, e.crc);
    put32(central, stored_size);
    put32(central, e.declared);
    put16(central, name_size);
    put16(central, 0);
    put16(central, 0);
    put16(central, 0);
    put16(central, 0);
    put32(central, 0);
    put32(central, offset);
    central += e.name;
  }
  const auto directory_at = nx::cast<u32>(out.size());
  out += central;
  put32(out, 0x06054b50u);
  put16(out, 0);
  put16(out, 0);
  put16(out, nx::cast<u32>(entries.size()));
  put16(out, nx::cast<u32>(entries.size()));
  put32(out, nx::cast<u32>(central.size()));
  put32(out, directory_at);
  put16(out, 0);
  return out;
}

auto mount_raw(const std::string &zip) {
  return vfs::make_zip_device(nx::blob<u8>(std::span<const u8>(
      reinterpret_cast<const u8 *>(zip.data()), zip.size())));
}

} // namespace

TEST_CASE("vfs zip refuses an entry whose CRC does not match") {
  raw_entry bad = stored("bad.txt", "HELLO");
  bad.crc ^= 1u;
  auto zd = mount_raw(raw_zip({stored("good.txt", "FINE"), bad}));
  REQUIRE(zd.has_value());
  scoped_vfs guard;
  vfs::mount("/", *zd, 0);

  CHECK(read_eq("/good.txt", "FINE"));
  CHECK_FALSE(vfs::read("/bad.txt").has_value());

  auto stream = vfs::open("/bad.txt");
  REQUIRE(stream.has_value());
  char buffer[16] = {};
  CHECK((*stream)->status().has_value());
  CHECK((*stream)->read(buffer, 5) == 0u);
  CHECK((*stream)->eof());
  const auto status = (*stream)->status();
  REQUIRE_FALSE(status.has_value());
  CHECK(status.error().kind == nx::fs::io_error::ReadFailed);
}

TEST_CASE("vfs zip does not trust an entry's declared size") {
  raw_entry forged = stored("forged.bin", "SHORT");
  forged.declared = 0x7fff0000u;
  auto zd = mount_raw(raw_zip({forged}));
  REQUIRE(zd.has_value());
  scoped_vfs guard;
  vfs::mount("/", *zd, 0);
  CHECK_FALSE(vfs::read("/forged.bin").has_value());
}

TEST_CASE("vfs zip refuses an archive whose directory breaks off") {
  std::string zip = raw_zip({stored("one.txt", "1"), stored("two.txt", "2")});
  const usize first = zip.find("PK\x01\x02");
  REQUIRE(first != std::string::npos);
  const usize second = zip.find("PK\x01\x02", first + 4);
  REQUIRE(second != std::string::npos);
  zip[second + 3] = '\x09';
  CHECK_FALSE(mount_raw(zip).has_value());
}

TEST_CASE("vfs zip leaves out entries that are not plain paths") {
  auto zd = mount_raw(
      raw_zip({stored("../evil.txt", "E"), stored("a/../../up.txt", "U"),
               stored("ok.txt", "K")}));
  REQUIRE(zd.has_value());
  scoped_vfs guard;
  vfs::mount("/", *zd, 0);
  CHECK(read_eq("/ok.txt", "K"));
  const auto listed = vfs::list("/");
  REQUIRE(listed.has_value());
  CHECK(listed->size() == 1u);
  CHECK(has_name(*listed, "ok.txt"));
}
