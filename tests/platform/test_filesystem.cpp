#include "framework/nxtest.h"

#include "core/foundation/platform/filesystem.h"
#include "core/foundation/platform/windows_path.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/threading/thread.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = nx::fs;

namespace {

[[nodiscard]] long process_id() noexcept {
#ifdef _WIN32
  return _getpid();
#else
  return getpid();
#endif
}

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
    const std::string name = "nx_fs_tests_" + std::to_string(process_id());
    m_path = fs::path::join(nx::string_view(tmp), nx::string_view(name));
    (void)fs::remove_all(fs::path_view(m_path));
    (void)fs::create_directories(fs::path_view(m_path));
  }
  ~scratch_root() { (void)fs::remove_all(fs::path_view(m_path)); }

  nx::string m_path;
};

class temp_dir {
public:
  explicit temp_dir(const char *label) {
    static std::atomic<int> counter{0};
    m_path =
        fs::path::join(nx::string_view(scratch_root::get()),
                       nx::string_view(std::string(label) + "_" +
                                       std::to_string(counter.fetch_add(1))));
    (void)fs::remove_all(fs::path_view(m_path));
    const auto r = fs::create_directories(fs::path_view(m_path));
    (void)r;
  }
  ~temp_dir() { (void)fs::remove_all(fs::path_view(m_path)); }

  temp_dir(const temp_dir &) = delete;
  temp_dir &operator=(const temp_dir &) = delete;

  [[nodiscard]] const nx::string &str() const { return m_path; }
  [[nodiscard]] fs::path_view view() const { return fs::path_view(m_path); }
  [[nodiscard]] nx::string child(nx::string_view name) const {
    return fs::path::join(nx::string_view(m_path), name);
  }

private:
  nx::string m_path;
};

std::vector<std::string> entry_names(const nx::vector<fs::directory_entry> &v) {
  std::vector<std::string> out;
  out.reserve(v.size());
  for (const fs::directory_entry &e : v)
    out.emplace_back(fs::path::filename(nx::string_view(e.path)));
  std::sort(out.begin(), out.end());
  return out;
}

const std::span<const u8> bytes_of(const char *s, usize n) {
  return {reinterpret_cast<const u8 *>(s), n};
}

} // namespace

TEST_CASE("fs::path: is_separator accepts both slashes") {
  CHECK(fs::path::is_separator('/'));
  CHECK(fs::path::is_separator('\\'));
  CHECK_FALSE(fs::path::is_separator('a'));
  CHECK_FALSE(fs::path::is_separator(':'));
  CHECK_FALSE(fs::path::is_separator('\0'));
  static_assert(fs::path::is_separator('/'));
}

TEST_CASE("fs::path: is_absolute understands roots and drive letters") {
  CHECK(fs::path::is_absolute("/usr/bin"));
  CHECK(fs::path::is_absolute("\\\\server\\share"));
  CHECK(fs::path::is_absolute("/"));
  CHECK(fs::path::is_absolute("C:/Windows"));
  CHECK(fs::path::is_absolute("C:\\Windows"));
  CHECK(fs::path::is_absolute("z:/x"));

  CHECK_FALSE(fs::path::is_absolute("C:"));
  CHECK_FALSE(fs::path::is_absolute("C:foo"));

  CHECK_FALSE(fs::path::is_absolute(""));
  CHECK_FALSE(fs::path::is_absolute("foo"));
  CHECK_FALSE(fs::path::is_absolute("foo/bar"));
  CHECK_FALSE(fs::path::is_absolute("./foo"));
  CHECK_FALSE(fs::path::is_absolute("../foo"));
  CHECK_FALSE(fs::path::is_absolute("1:/x"));

  for (const char *p : {"/a", "C:/a", "C:", "", "a", "./a"})
    CHECK(fs::path::is_relative(p) == !fs::path::is_absolute(p));

  static_assert(fs::path::is_absolute("/x"));
  static_assert(!fs::path::is_absolute("x"));
}

TEST_CASE("fs::path: filename returns the trailing component") {
  CHECK(fs::path::filename("/a/b/c.txt") == "c.txt");
  CHECK(fs::path::filename("c.txt") == "c.txt");
  CHECK(fs::path::filename("a\\b\\c.txt") == "c.txt");
  CHECK(fs::path::filename("/a/b/") == "");
  CHECK(fs::path::filename("/") == "");
  CHECK(fs::path::filename("") == "");
  CHECK(fs::path::filename("C:/x") == "x");
  CHECK(fs::path::filename(".") == ".");
  CHECK(fs::path::filename("..") == "..");
  static_assert(fs::path::filename("/a/b") == "b");
}

TEST_CASE("fs::path: parent_path strips the trailing component") {
  CHECK(fs::path::parent_path("/a/b/c.txt") == "/a/b");
  CHECK(fs::path::parent_path("a/b") == "a");
  CHECK(fs::path::parent_path("a\\b") == "a");
  CHECK(fs::path::parent_path("/a") == "/");
  CHECK(fs::path::parent_path("/") == "/");
  CHECK(fs::path::parent_path("c.txt") == "");
  CHECK(fs::path::parent_path("") == "");
  CHECK(fs::path::parent_path("/a/b/") == "/a/b");
  static_assert(fs::path::parent_path("a/b") == "a");
}

TEST_CASE("fs::path: extension and stem split on the last dot") {
  CHECK(fs::path::extension("a/b/c.txt") == ".txt");
  CHECK(fs::path::extension("archive.tar.gz") == ".gz");
  CHECK(fs::path::extension("noext") == "");
  CHECK(fs::path::extension("") == "");
  CHECK(fs::path::extension("a/b/noext") == "");
  CHECK(fs::path::extension("trailing.") == ".");

  CHECK(fs::path::extension(".hidden") == "");
  CHECK(fs::path::extension("/a/.hidden") == "");
  CHECK(fs::path::extension(".hidden.txt") == ".txt");

  CHECK(fs::path::extension(".") == "");
  CHECK(fs::path::extension("..") == "");

  CHECK(fs::path::stem("a/b/c.txt") == "c");
  CHECK(fs::path::stem("archive.tar.gz") == "archive.tar");
  CHECK(fs::path::stem("noext") == "noext");
  CHECK(fs::path::stem(".hidden") == ".hidden");
  CHECK(fs::path::stem(".hidden.txt") == ".hidden");
  CHECK(fs::path::stem("trailing.") == "trailing");
  CHECK(fs::path::stem(".") == ".");
  CHECK(fs::path::stem("..") == "..");
  CHECK(fs::path::stem("") == "");

  for (const char *p : {"a/b/c.txt", "archive.tar.gz", "noext", ".hidden",
                        ".hidden.txt", "trailing.", ".", ".."}) {
    const std::string joined =
        std::string(fs::path::stem(p)) + std::string(fs::path::extension(p));
    CHECK(joined == std::string(fs::path::filename(p)));
  }

  CHECK(fs::path::has_extension("a.txt"));
  CHECK_FALSE(fs::path::has_extension("a"));
  CHECK_FALSE(fs::path::has_extension(".hidden"));
  static_assert(fs::path::extension("a.txt") == ".txt");
}

TEST_CASE("fs::path: root_name extracts a drive prefix") {
  CHECK(fs::path::root_name("C:/x") == "C:");
  CHECK(fs::path::root_name("c:") == "c:");
  CHECK(fs::path::root_name("C:foo") == "C:");
  CHECK(fs::path::root_name("/usr") == "");
  CHECK(fs::path::root_name("foo") == "");
  CHECK(fs::path::root_name("") == "");
  CHECK(fs::path::root_name("1:/x") == "");
  static_assert(fs::path::root_name("C:/x") == "C:");
}

TEST_CASE("fs::path: is_within respects path component boundaries") {
  CHECK(fs::path::is_within("C:/assets", "C:/assets"));
  CHECK(fs::path::is_within("C:/assets/avatar/body.webp", "C:/assets"));
  CHECK(fs::path::is_within("/assets/avatar", "/assets"));
  CHECK_FALSE(fs::path::is_within("C:/assets-old/body.webp", "C:/assets"));
  CHECK_FALSE(fs::path::is_within("C:/asset", "C:/assets"));
  CHECK_FALSE(fs::path::is_within("C:/assets", ""));
  static_assert(fs::path::is_within("/a/b", "/a"));
  static_assert(!fs::path::is_within("/ab", "/a"));
}

TEST_CASE("fs::path: join concatenates and respects absolute tails") {
  CHECK(fs::path::join("a", "b") == "a/b");
  CHECK(fs::path::join("a/", "b") == "a/b");
  CHECK(fs::path::join("a\\", "b") == "a\\b");
  CHECK(fs::path::join("a/b", "c/d") == "a/b/c/d");

  CHECK(fs::path::join("a", "/b") == "/b");
  CHECK(fs::path::join("a", "C:/b") == "C:/b");

  CHECK(fs::path::join("a", "") == "a");
  CHECK(fs::path::join("", "b") == "b");
  CHECK(fs::path::join("", "") == "");
}

TEST_CASE("fs::path: normalize collapses separators and '.'") {
  CHECK(fs::path::normalize("/a/b") == "/a/b");
  CHECK(fs::path::normalize("a//b/./c") == "a/b/c");
  CHECK(fs::path::normalize("/a/b/") == "/a/b");
  CHECK(fs::path::normalize("\\a\\b") == "/a/b");
  CHECK(fs::path::normalize("/") == "/");
  CHECK(fs::path::normalize("a") == "a");
}

TEST_CASE("fs::path: normalize resolves '..' lexically") {
  CHECK(fs::path::normalize("/a/../b") == "/b");
  CHECK(fs::path::normalize("/a/b/../../c") == "/c");
  CHECK(fs::path::normalize("/a/./b/../c") == "/a/c");
  CHECK(fs::path::normalize("a/b/../c") == "a/c");
}

TEST_CASE("fs::path: normalize keeps a relative path relative") {
  CHECK(fs::path::normalize("../a") == "../a");
  CHECK(fs::path::normalize("../../a") == "../../a");
  CHECK(fs::path::normalize("a/../../b") == "../b");
  CHECK(fs::path::normalize("a/..") == ".");
  CHECK(fs::path::normalize(".") == ".");
}

TEST_CASE("fs::path: normalize refuses to escape an absolute root") {
  CHECK(fs::path::normalize("/../x").empty());
  CHECK(fs::path::normalize("/a/../../x").empty());
  CHECK(fs::path::normalize("").empty());
}

TEST_CASE("fs::path: normalize preserves a drive root name") {
  CHECK(fs::path::normalize("C:/a/../b") == "C:/b");
  CHECK(fs::path::normalize("C:\\a\\b") == "C:/a/b");
  CHECK(fs::path::normalize("C:/") == "C:/");
  CHECK(fs::path::normalize("C:a/b") == "C:a/b");
  CHECK(fs::path::normalize("C:/../x").empty());
}

TEST_CASE("fs::path_view: wraps a view and forwards the lexical queries") {
  constexpr fs::path_view empty;
  CHECK(empty.empty());
  CHECK(empty.native() == "");

  const fs::path_view p("/home/user/file.tar.gz");
  CHECK_FALSE(p.empty());
  CHECK(p.native() == "/home/user/file.tar.gz");
  CHECK(p.is_absolute());
  CHECK_FALSE(p.is_relative());
  CHECK(p.filename().native() == "file.tar.gz");
  CHECK(p.stem().native() == "file.tar");
  CHECK(p.extension() == ".gz");
  CHECK(p.parent_path().native() == "/home/user");
  CHECK(p.root_name() == "");
  CHECK(p.has_extension());

  const fs::path_view rel("docs/readme");
  CHECK(rel.is_relative());
  CHECK_FALSE(rel.has_extension());
  CHECK(rel.stem().native() == "readme");

  const fs::path_view drive("D:\\games\\x.exe");
  CHECK(drive.is_absolute());
  CHECK(drive.root_name() == "D:");
  CHECK(drive.filename().native() == "x.exe");

  const auto bytes = p.as_bytes();
  CHECK(bytes.size() == p.native().size());
  CHECK(reinterpret_cast<const char *>(bytes.data()) == p.native().data());
}

TEST_CASE("fs::path_view: constructs from string_view, char* and nx::string") {
  const nx::string owned("dir/file.txt");
  const fs::path_view from_string(owned);
  CHECK(from_string.native() == "dir/file.txt");

  const fs::path_view from_view(nx::string_view("dir/file.txt"));
  CHECK(from_view.filename().native() == "file.txt");

  const fs::path_view from_cstr("dir/file.txt");
  CHECK(from_cstr.stem().native() == "file");
}

TEST_CASE("fs::path_view: join and operator/ build new paths") {
  const fs::path_view base("root/dir");
  CHECK(base.join("file.txt") == "root/dir/file.txt");
  CHECK(base / "file.txt" == "root/dir/file.txt");
  CHECK(base / "/abs" == "/abs");
  CHECK(base / "" == "root/dir");

  const nx::string once = base / "a";
  CHECK(fs::path_view(once) / "b" == "root/dir/a/b");
}

TEST_CASE("fs::path_view: formats as its UTF-8 text") {
  const fs::path_view p("some/where.txt");
  CHECK(std::format("{}", p) == "some/where.txt");
  CHECK(std::format("[{:>16}]", p) == "[  some/where.txt]");
}

TEST_CASE("fs::to_string names every io_error") {
  CHECK(fs::to_string(fs::io_error::NotFound) == "file not found");
  CHECK(fs::to_string(fs::io_error::PermissionDenied) == "permission denied");
  CHECK(fs::to_string(fs::io_error::AlreadyExists) == "already exists");
  CHECK(fs::to_string(fs::io_error::IsADirectory) == "is a directory");
  CHECK(fs::to_string(fs::io_error::NotADirectory) == "not a directory");
  CHECK(fs::to_string(fs::io_error::NoSpace) == "no space left on device");
  CHECK(fs::to_string(fs::io_error::FileTooLarge) == "file too large");
  CHECK(fs::to_string(fs::io_error::InvalidArgument) == "invalid argument");
  CHECK(fs::to_string(fs::io_error::Interrupted) == "interrupted");
  CHECK(fs::to_string(fs::io_error::OpenFailed) == "open failed");
  CHECK(fs::to_string(fs::io_error::ReadFailed) == "read failed");
  CHECK(fs::to_string(fs::io_error::WriteFailed) == "write failed");
  CHECK(fs::to_string(fs::io_error::Busy) == "held by another");
  CHECK(fs::to_string(fs::io_error::Unknown) == "unknown error");

  for (u32 i = 0; i < static_cast<u32>(fs::io_error::Count); ++i)
    CHECK_FALSE(fs::to_string(static_cast<fs::io_error>(i)).empty());

  static_assert(fs::to_string(fs::io_error::NotFound) == "file not found");
}

TEST_CASE("fs: write then read round-trips binary content") {
  const temp_dir dir("rw");
  const nx::string file = dir.child("data.bin");

  const char payload[] = "\x00\x01\x02hello\xFF\xFE";
  const auto span = bytes_of(payload, sizeof(payload) - 1);

  const auto w = fs::file_write(fs::path_view(file), span);
  REQUIRE(w.has_value());
  CHECK(fs::exists(fs::path_view(file)));
  CHECK(fs::status(fs::path_view(file)) == fs::file_type::regular);

  const auto r = fs::file_read(fs::path_view(file));
  REQUIRE(r.has_value());
  REQUIRE(r->size() == sizeof(payload) - 1);
  for (usize i = 0; i < r->size(); ++i)
    REQUIRE((*r)[i] == static_cast<u8>(payload[i]));
}

TEST_CASE("fs: an empty file round-trips as an empty blob") {
  const temp_dir dir("empty");
  const nx::string file = dir.child("zero.bin");

  REQUIRE(
      fs::file_write(fs::path_view(file), std::span<const u8>{}).has_value());
  CHECK(fs::exists(fs::path_view(file)));

  const auto r = fs::file_read(fs::path_view(file));
  REQUIRE(r.has_value());
  CHECK(r->empty());

  const auto t = fs::file_read_text(fs::path_view(file));
  REQUIRE(t.has_value());
  CHECK(t->empty());
}

TEST_CASE("fs: text write/read round-trips, including UTF-8 and newlines") {
  const temp_dir dir("text");
  const nx::string file = dir.child("notes.txt");

  const nx::string_view text = "line one\nline two\ncaf\xC3\xA9 \xE2\x82\xAC\n";
  REQUIRE(fs::file_write_text(fs::path_view(file), text).has_value());

  const auto r = fs::file_read_text(fs::path_view(file));
  REQUIRE(r.has_value());
  CHECK(r->view() == text);
  CHECK(r->is_valid_utf8());
}

TEST_CASE("fs: stat_file reports type, size and a mtime that moves") {
  const temp_dir dir("stat");
  const nx::string file = dir.child("watched.txt");
  REQUIRE(fs::file_write_text(fs::path_view(file), "one").has_value());

  const fs::file_stat first = fs::stat_file(fs::path_view(file));
  CHECK(first.type == fs::file_type::regular);
  CHECK(first.size == 3u);
  CHECK(first.mtime_ns != 0u);

  nx::this_thread::sleep_for(std::chrono::milliseconds(20));
  REQUIRE(
      fs::file_write_text(fs::path_view(file), "one two three").has_value());
  const fs::file_stat second = fs::stat_file(fs::path_view(file));
  CHECK(second.size == 13u);
  CHECK(second.mtime_ns >= first.mtime_ns);
  CHECK(second.mtime_ns != first.mtime_ns);

  const fs::file_stat folder = fs::stat_file(dir.view());
  CHECK(folder.type == fs::file_type::directory);

  const fs::file_stat missing = fs::stat_file(fs::path_view(dir.child("no")));
  CHECK(missing.type == fs::file_type::not_found);
  CHECK(missing.mtime_ns == 0u);
}

TEST_CASE("fs: a large file round-trips exactly") {
  const temp_dir dir("large");
  const nx::string file = dir.child("big.bin");

  constexpr usize n = 1u << 20;
  std::vector<u8> data(n);
  for (usize i = 0; i < n; ++i)
    data[i] = static_cast<u8>(i * 31 + (i >> 8));

  REQUIRE(fs::file_write(fs::path_view(file),
                         std::span<const u8>(data.data(), data.size()))
              .has_value());

  const auto r = fs::file_read(fs::path_view(file));
  REQUIRE(r.has_value());
  REQUIRE(r->size() == n);
  CHECK(std::equal(r->begin(), r->end(), data.begin()));
}

TEST_CASE("fs: writing an existing file replaces its contents") {
  const temp_dir dir("overwrite");
  const nx::string file = dir.child("f.txt");

  REQUIRE(fs::file_write_text(fs::path_view(file), "a long original line")
              .has_value());
  REQUIRE(fs::file_write_text(fs::path_view(file), "short").has_value());

  const auto r = fs::file_read_text(fs::path_view(file));
  REQUIRE(r.has_value());
  CHECK(*r == "short");
}

TEST_CASE("fs: reading a missing file reports NotFound") {
  const temp_dir dir("missing");
  const nx::string file = dir.child("does_not_exist.bin");

  CHECK_FALSE(fs::exists(fs::path_view(file)));
  CHECK(fs::status(fs::path_view(file)) == fs::file_type::not_found);

  const auto r = fs::file_read(fs::path_view(file));
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == fs::io_error::NotFound);

  const auto t = fs::file_read_text(fs::path_view(file));
  REQUIRE_FALSE(t.has_value());
  CHECK(t.error().kind == fs::io_error::NotFound);
}

TEST_CASE("fs: reading a directory as a file fails rather than succeeding") {
  const temp_dir dir("readdir");
  const auto r = fs::file_read(dir.view());
  CHECK_FALSE(r.has_value());
}

TEST_CASE("fs: writing into a missing directory fails") {
  const temp_dir dir("nodir");
  const nx::string file =
      fs::path::join(nx::string_view(dir.child("absent")), "f.txt");
  const auto w = fs::file_write_text(fs::path_view(file), "x");
  CHECK_FALSE(w.has_value());
}

TEST_CASE("fs: create_directories builds the whole chain and is idempotent") {
  const temp_dir dir("mkdir");
  const nx::string deep = fs::path::join(nx::string_view(dir.str()), "a/b/c/d");

  REQUIRE(fs::create_directories(fs::path_view(deep)).has_value());
  CHECK(fs::exists(fs::path_view(deep)));
  CHECK(fs::status(fs::path_view(deep)) == fs::file_type::directory);

  CHECK(fs::status(fs::path_view(fs::path::join(
            nx::string_view(dir.str()), "a"))) == fs::file_type::directory);
  CHECK(fs::status(fs::path_view(fs::path::join(
            nx::string_view(dir.str()), "a/b"))) == fs::file_type::directory);

  CHECK(fs::create_directories(fs::path_view(deep)).has_value());
}

TEST_CASE("fs: status and symlink_status classify entries") {
  const temp_dir dir("status");
  const nx::string file = dir.child("f.txt");
  const nx::string sub = dir.child("sub");
  REQUIRE(fs::file_write_text(fs::path_view(file), "x").has_value());
  REQUIRE(fs::create_directories(fs::path_view(sub)).has_value());

  CHECK(fs::status(fs::path_view(file)) == fs::file_type::regular);
  CHECK(fs::status(fs::path_view(sub)) == fs::file_type::directory);
  CHECK(fs::status(dir.view()) == fs::file_type::directory);
  CHECK(fs::status(fs::path_view(dir.child("nope"))) ==
        fs::file_type::not_found);

  CHECK(fs::symlink_status(fs::path_view(file)) == fs::file_type::regular);
  CHECK(fs::symlink_status(fs::path_view(sub)) == fs::file_type::directory);
  CHECK(fs::symlink_status(fs::path_view(dir.child("nope"))) ==
        fs::file_type::not_found);

  CHECK(fs::exists(fs::path_view(file)));
  CHECK(fs::exists(fs::path_view(sub)));
  CHECK_FALSE(fs::exists(fs::path_view(dir.child("nope"))));
  CHECK_FALSE(fs::exists(fs::path_view("")));
}

TEST_CASE("fs: directory_entries lists one level with types") {
  const temp_dir dir("list");
  REQUIRE(
      fs::file_write_text(fs::path_view(dir.child("a.txt")), "a").has_value());
  REQUIRE(
      fs::file_write_text(fs::path_view(dir.child("b.txt")), "b").has_value());
  REQUIRE(fs::create_directories(fs::path_view(dir.child("sub"))).has_value());
  REQUIRE(fs::file_write_text(fs::path_view(dir.child("sub/deep.txt")), "d")
              .has_value());

  const auto r = fs::directory_entries(dir.view());
  REQUIRE(r.has_value());
  CHECK(entry_names(*r) == std::vector<std::string>{"a.txt", "b.txt", "sub"});

  for (const fs::directory_entry &e : *r) {
    const nx::string_view name = fs::path::filename(nx::string_view(e.path));
    if (name == "sub")
      CHECK(e.type == fs::file_type::directory);
    else
      CHECK(e.type == fs::file_type::regular);
    CHECK(fs::exists(fs::path_view(e.path)));
  }

  const temp_dir empty("list_empty");
  const auto e = fs::directory_entries(empty.view());
  REQUIRE(e.has_value());
  CHECK(e->empty());

  CHECK_FALSE(
      fs::directory_entries(fs::path_view(dir.child("nope"))).has_value());
}

TEST_CASE("fs: recursive_directory_entries descends the whole tree") {
  const temp_dir dir("rlist");
  REQUIRE(fs::create_directories(fs::path_view(dir.child("a/b"))).has_value());
  REQUIRE(fs::file_write_text(fs::path_view(dir.child("top.txt")), "1")
              .has_value());
  REQUIRE(fs::file_write_text(fs::path_view(dir.child("a/mid.txt")), "2")
              .has_value());
  REQUIRE(fs::file_write_text(fs::path_view(dir.child("a/b/leaf.txt")), "3")
              .has_value());

  const auto r = fs::recursive_directory_entries(dir.view());
  REQUIRE(r.has_value());
  CHECK(entry_names(*r) ==
        std::vector<std::string>{"a", "b", "leaf.txt", "mid.txt", "top.txt"});

  int files = 0, dirs = 0;
  for (const fs::directory_entry &e : *r) {
    REQUIRE(fs::exists(fs::path_view(e.path)));
    if (e.type == fs::file_type::directory)
      ++dirs;
    else if (e.type == fs::file_type::regular)
      ++files;
  }
  CHECK(dirs == 2);
  CHECK(files == 3);

  const auto shallow = fs::directory_entries(dir.view());
  REQUIRE(shallow.has_value());
  CHECK(shallow->size() < r->size());
}

TEST_CASE("fs: copy_file duplicates content and honours overwrite") {
  const temp_dir dir("copyfile");
  const nx::string src = dir.child("src.txt");
  const nx::string dst = dir.child("dst.txt");
  REQUIRE(fs::file_write_text(fs::path_view(src), "payload").has_value());

  REQUIRE(fs::copy_file(fs::path_view(src), fs::path_view(dst)).has_value());
  CHECK(fs::exists(fs::path_view(dst)));
  CHECK(*fs::file_read_text(fs::path_view(dst)) == "payload");
  CHECK(*fs::file_read_text(fs::path_view(src)) == "payload");

  REQUIRE(fs::file_write_text(fs::path_view(dst), "original").has_value());
  CHECK_FALSE(
      fs::copy_file(fs::path_view(src), fs::path_view(dst)).has_value());
  CHECK(*fs::file_read_text(fs::path_view(dst)) == "original");

  REQUIRE(
      fs::copy_file(fs::path_view(src), fs::path_view(dst), true).has_value());
  CHECK(*fs::file_read_text(fs::path_view(dst)) == "payload");

  CHECK_FALSE(fs::copy_file(fs::path_view(dir.child("nope")),
                            fs::path_view(dir.child("out")))
                  .has_value());
}

TEST_CASE("fs: staged files publish completely or preserve the destination") {
  const temp_dir dir("staged_file");
  const nx::string destination = dir.child("output.txt");
  REQUIRE(
      fs::file_write_text(fs::path_view(destination), "original").has_value());

  nx::string abandoned_path;
  {
    fs::staged_file staged{fs::path_view(destination)};
    abandoned_path = nx::string(staged.temporary_path().native());
    REQUIRE(fs::file_write_text(staged.temporary_path(), "unpublished")
                .has_value());
    CHECK(*fs::file_read_text(fs::path_view(destination)) == "original");
  }
  CHECK_FALSE(fs::exists(fs::path_view(abandoned_path)));
  CHECK(*fs::file_read_text(fs::path_view(destination)) == "original");

  REQUIRE(fs::file_write_text_atomic(fs::path_view(destination), "replacement")
              .has_value());
  CHECK(*fs::file_read_text(fs::path_view(destination)) == "replacement");

  fs::staged_file missing{fs::path_view(destination)};
  CHECK_FALSE(missing.commit().has_value());
  CHECK(*fs::file_read_text(fs::path_view(destination)) == "replacement");
}

TEST_CASE("fs: atomic copy replaces only after the source is readable") {
  const temp_dir dir("atomic_copy");
  const nx::string source = dir.child("source.bin");
  const nx::string destination = dir.child("destination.bin");
  REQUIRE(fs::file_write_text(fs::path_view(source), "new").has_value());
  REQUIRE(fs::file_write_text(fs::path_view(destination), "old").has_value());

  REQUIRE(
      fs::copy_file_atomic(fs::path_view(source), fs::path_view(destination))
          .has_value());
  CHECK(*fs::file_read_text(fs::path_view(destination)) == "new");

  CHECK_FALSE(fs::copy_file_atomic(fs::path_view(dir.child("missing")),
                                   fs::path_view(destination))
                  .has_value());
  CHECK(*fs::file_read_text(fs::path_view(destination)) == "new");
}

TEST_CASE("fs: copy duplicates a directory tree") {
  const temp_dir dir("copytree");
  const nx::string src = dir.child("src");
  const nx::string dst = dir.child("dst");

  REQUIRE(fs::create_directories(
              fs::path_view(fs::path::join(nx::string_view(src), "a/b")))
              .has_value());
  REQUIRE(
      fs::file_write_text(
          fs::path_view(fs::path::join(nx::string_view(src), "top.txt")), "t")
          .has_value());
  REQUIRE(fs::file_write_text(fs::path_view(fs::path::join(nx::string_view(src),
                                                           "a/b/leaf.txt")),
                              "l")
              .has_value());

  REQUIRE(fs::copy(fs::path_view(src), fs::path_view(dst)).has_value());

  CHECK(fs::status(fs::path_view(dst)) == fs::file_type::directory);
  CHECK(*fs::file_read_text(fs::path_view(
            fs::path::join(nx::string_view(dst), "top.txt"))) == "t");
  CHECK(*fs::file_read_text(fs::path_view(
            fs::path::join(nx::string_view(dst), "a/b/leaf.txt"))) == "l");

  const auto copied = fs::recursive_directory_entries(fs::path_view(dst));
  const auto original = fs::recursive_directory_entries(fs::path_view(src));
  REQUIRE(copied.has_value());
  REQUIRE(original.has_value());
  CHECK(entry_names(*copied) == entry_names(*original));

  const nx::string one = dir.child("one.txt");
  const nx::string two = dir.child("two.txt");
  REQUIRE(fs::file_write_text(fs::path_view(one), "x").has_value());
  REQUIRE(fs::copy(fs::path_view(one), fs::path_view(two)).has_value());
  CHECK(*fs::file_read_text(fs::path_view(two)) == "x");
}

TEST_CASE("fs: rename moves a file and a directory") {
  const temp_dir dir("rename");
  const nx::string from = dir.child("from.txt");
  const nx::string to = dir.child("to.txt");
  REQUIRE(fs::file_write_text(fs::path_view(from), "moved").has_value());

  REQUIRE(fs::rename(fs::path_view(from), fs::path_view(to)).has_value());
  CHECK_FALSE(fs::exists(fs::path_view(from)));
  CHECK(fs::exists(fs::path_view(to)));
  CHECK(*fs::file_read_text(fs::path_view(to)) == "moved");

  const nx::string dsrc = dir.child("dsrc");
  const nx::string ddst = dir.child("ddst");
  REQUIRE(fs::create_directories(fs::path_view(dsrc)).has_value());
  REQUIRE(fs::file_write_text(
              fs::path_view(fs::path::join(nx::string_view(dsrc), "inner.txt")),
              "i")
              .has_value());
  REQUIRE(fs::rename(fs::path_view(dsrc), fs::path_view(ddst)).has_value());
  CHECK_FALSE(fs::exists(fs::path_view(dsrc)));
  CHECK(*fs::file_read_text(fs::path_view(
            fs::path::join(nx::string_view(ddst), "inner.txt"))) == "i");

  CHECK_FALSE(fs::rename(fs::path_view(dir.child("nope")),
                         fs::path_view(dir.child("other")))
                  .has_value());
}

TEST_CASE("fs: remove deletes a file or an empty directory") {
  const temp_dir dir("remove");
  const nx::string file = dir.child("f.txt");
  REQUIRE(fs::file_write_text(fs::path_view(file), "x").has_value());

  REQUIRE(fs::remove(fs::path_view(file)).has_value());
  CHECK_FALSE(fs::exists(fs::path_view(file)));

  const nx::string empty = dir.child("empty");
  REQUIRE(fs::create_directories(fs::path_view(empty)).has_value());
  REQUIRE(fs::remove(fs::path_view(empty)).has_value());
  CHECK_FALSE(fs::exists(fs::path_view(empty)));

  CHECK_FALSE(fs::remove(fs::path_view(dir.child("nope"))).has_value());

  const nx::string full = dir.child("full");
  REQUIRE(fs::create_directories(fs::path_view(full)).has_value());
  REQUIRE(
      fs::file_write_text(
          fs::path_view(fs::path::join(nx::string_view(full), "c.txt")), "c")
          .has_value());
  CHECK_FALSE(fs::remove(fs::path_view(full)).has_value());
  CHECK(fs::exists(fs::path_view(full)));
}

TEST_CASE("fs: remove_all deletes a whole tree") {
  const temp_dir dir("removeall");
  const nx::string root = dir.child("tree");
  REQUIRE(fs::create_directories(
              fs::path_view(fs::path::join(nx::string_view(root), "a/b/c")))
              .has_value());
  for (const char *rel : {"f1.txt", "a/f2.txt", "a/b/f3.txt", "a/b/c/f4.txt"}) {
    REQUIRE(
        fs::file_write_text(fs::path_view(fs::path::join(nx::string_view(root),
                                                         nx::string_view(rel))),
                            "x")
            .has_value());
  }
  CHECK(fs::exists(fs::path_view(root)));

  REQUIRE(fs::remove_all(fs::path_view(root)).has_value());
  CHECK_FALSE(fs::exists(fs::path_view(root)));

  const nx::string file = dir.child("solo.txt");
  REQUIRE(fs::file_write_text(fs::path_view(file), "x").has_value());
  REQUIRE(fs::remove_all(fs::path_view(file)).has_value());
  CHECK_FALSE(fs::exists(fs::path_view(file)));
}

TEST_CASE("fs: executable_dir points at a real directory") {
  const auto r = fs::executable_dir();
  REQUIRE(r.has_value());
  CHECK_FALSE(r->empty());
  CHECK(fs::status(fs::path_view(*r)) == fs::file_type::directory);
  CHECK(fs::path_view(*r).is_absolute());
  CHECK_FALSE(fs::path::is_separator(r->back()));
}

TEST_CASE("fs: application_data_dir is per-application and rejects paths") {
  const auto r = fs::application_data_dir("foundation_path_test");
  REQUIRE(r.has_value());
  CHECK(fs::path_view(*r).is_absolute());
  const bool expected_suffix = r->ends_with("nx2d/foundation_path_test") ||
                               r->ends_with("nx2d\\foundation_path_test");
  CHECK(expected_suffix);

  CHECK_FALSE(fs::application_data_dir("").has_value());
  CHECK_FALSE(fs::application_data_dir("..").has_value());
  CHECK_FALSE(fs::application_data_dir("nested/name").has_value());
  CHECK_FALSE(fs::application_data_dir("nested\\name").has_value());
}

TEST_CASE("fs::stream_ptr: default is invalid and move transfers ownership") {
  fs::stream_ptr empty;
  CHECK_FALSE(empty.valid());
  CHECK_FALSE(static_cast<bool>(empty));
  CHECK(empty.get() == nullptr);
  empty.reset();

  const char data[] = "abcdef";
  auto a = fs::open_memory(data, sizeof(data) - 1);
  REQUIRE(a.has_value());
  CHECK(a->valid());
  fs::stream *raw = a->get();

  fs::stream_ptr b = std::move(*a);
  CHECK(b.valid());
  CHECK(b.get() == raw);
  CHECK_FALSE(a->valid());

  fs::stream_ptr c;
  c = std::move(b);
  CHECK(c.valid());
  CHECK_FALSE(b.valid());

  fs::stream_ptr *alias = &c;
  c = std::move(*alias);
  CHECK(c.valid());

  c.reset();
  CHECK_FALSE(c.valid());

  static_assert(!std::is_copy_constructible_v<fs::stream_ptr>);
  static_assert(!std::is_copy_assignable_v<fs::stream_ptr>);
}

TEST_CASE("fs::open_memory: reads, seeks and reports size") {
  const char data[] = "0123456789";
  constexpr usize n = sizeof(data) - 1;

  auto s = fs::open_memory(data, n);
  REQUIRE(s.has_value());
  fs::stream &st = **s;

  CHECK(st.size() == static_cast<i64>(n));
  CHECK(st.tell() == 0);
  CHECK_FALSE(st.eof());

  char buf[4] = {};
  CHECK(st.read(buf, 4) == 4);
  CHECK(std::string(buf, 4) == "0123");
  CHECK(st.tell() == 4);

  CHECK(st.seek(n - 2));
  CHECK(st.tell() == static_cast<i64>(n) - 2);
  CHECK(st.read(buf, 4) == 2);
  CHECK(std::string(buf, 2) == "89");
  CHECK(st.eof());
  CHECK(st.read(buf, 4) == 0);

  CHECK(st.seek(0));
  CHECK_FALSE(st.eof());
  CHECK(st.tell() == 0);
  CHECK(st.read(buf, 1) == 1);
  CHECK(buf[0] == '0');

  CHECK(st.seek(2, fs::seek_origin::begin));
  CHECK(st.tell() == 2);
  CHECK(st.seek(3, fs::seek_origin::current));
  CHECK(st.tell() == 5);
  CHECK(st.seek(-1, fs::seek_origin::current));
  CHECK(st.tell() == 4);
  CHECK(st.seek(0, fs::seek_origin::end));
  CHECK(st.tell() == static_cast<i64>(n));
  CHECK(st.seek(-3, fs::seek_origin::end));
  CHECK(st.tell() == static_cast<i64>(n) - 3);

  const i64 before = st.tell();
  CHECK(st.read(buf, 0) == 0);
  CHECK(st.tell() == before);

  auto e = fs::open_memory(data, 0);
  REQUIRE(e.has_value());
  CHECK((*e)->size() == 0);
  CHECK((*e)->read(buf, 4) == 0);
  CHECK((*e)->eof());
}

TEST_CASE("fs::open_file: reads a file through the stream interface") {
  const temp_dir dir("stream");
  const nx::string file = dir.child("s.bin");

  std::vector<u8> data(5000);
  for (usize i = 0; i < data.size(); ++i)
    data[i] = static_cast<u8>(i);
  REQUIRE(fs::file_write(fs::path_view(file),
                         std::span<const u8>(data.data(), data.size()))
              .has_value());

  auto s = fs::open_file(fs::path_view(file));
  REQUIRE(s.has_value());
  fs::stream &st = **s;

  CHECK(st.size() == static_cast<i64>(data.size()));
  CHECK(st.tell() == 0);
  CHECK_FALSE(st.eof());

  std::vector<u8> read_back;
  u8 buf[512];
  for (;;) {
    const usize got = st.read(buf, sizeof(buf));
    if (got == 0)
      break;
    read_back.insert(read_back.end(), buf, buf + got);
  }
  CHECK(read_back == data);
  CHECK(st.eof());
  CHECK(st.tell() == static_cast<i64>(data.size()));

  CHECK(st.seek(100));
  CHECK(st.tell() == 100);
  CHECK(st.read(buf, 4) == 4);
  for (int i = 0; i < 4; ++i)
    CHECK(buf[i] == data[100 + static_cast<usize>(i)]);

  CHECK(st.seek(0, fs::seek_origin::end));
  CHECK(st.tell() == static_cast<i64>(data.size()));
  CHECK(st.seek(-10, fs::seek_origin::end));
  CHECK(st.read(buf, 32) == 10);
}

TEST_CASE("fs::open_file: a missing file reports an error") {
  const temp_dir dir("streammissing");
  const auto s = fs::open_file(fs::path_view(dir.child("absent.bin")));
  REQUIRE_FALSE(s.has_value());
  CHECK(s.error().kind == fs::io_error::NotFound);
}

TEST_CASE("fs::open_file: an empty file yields an immediately-eof stream") {
  const temp_dir dir("streamempty");
  const nx::string file = dir.child("empty.bin");
  REQUIRE(
      fs::file_write(fs::path_view(file), std::span<const u8>{}).has_value());

  auto s = fs::open_file(fs::path_view(file));
  REQUIRE(s.has_value());
  CHECK((*s)->size() == 0);
  CHECK((*s)->tell() == 0);
  char buf[8];
  CHECK((*s)->read(buf, sizeof(buf)) == 0);
  CHECK((*s)->eof());
}

TEST_CASE("fs::open_file_views: views keep cursors of their own") {
  const temp_dir dir("views");
  const nx::string file = dir.child("data.bin");
  REQUIRE(fs::file_write(fs::path_view(file), bytes_of("0123456789", 10))
              .has_value());

  auto views = fs::open_file_views(fs::path_view(file));
  REQUIRE(views.has_value());
  auto first = (*views)();
  auto second = (*views)();
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  CHECK((*first)->size() == 10);

  char a[4]{};
  char b[3]{};
  CHECK((*first)->read(a, 4) == 4);
  CHECK((*second)->seek(7));
  CHECK((*second)->read(b, 3) == 3);
  CHECK(std::string(a, 4) == "0123");
  CHECK(std::string(b, 3) == "789");
  CHECK((*second)->eof());
  CHECK((*first)->tell() == 4);
  CHECK((*first)->read(a, 4) == 4);
  CHECK(std::string(a, 4) == "4567");

  CHECK_FALSE((*first)->seek(11));
  CHECK_FALSE((*first)->seek(-1));
  CHECK((*first)->seek(-2, fs::seek_origin::end));
  CHECK((*first)->read(a, 4) == 2);
}

TEST_CASE("fs::open_file_views: the file can go from disk while it is open") {
  const temp_dir dir("viewsgone");
  const nx::string file = dir.child("data.bin");
  REQUIRE(
      fs::file_write(fs::path_view(file), bytes_of("before", 6)).has_value());
  auto views = fs::open_file_views(fs::path_view(file));
  REQUIRE(views.has_value());
  auto early = (*views)();
  REQUIRE(early.has_value());

  REQUIRE(fs::remove(fs::path_view(file)).has_value());
  REQUIRE(
      fs::file_write(fs::path_view(file), bytes_of("after!!", 7)).has_value());

  auto late = (*views)();
  REQUIRE(late.has_value());
  char text[6]{};
  CHECK((*early)->read(text, 6) == 6);
  CHECK(std::string(text, 6) == "before");
  CHECK((*late)->size() == 6);
  CHECK((*late)->read(text, 6) == 6);
  CHECK(std::string(text, 6) == "before");

  const auto now = fs::file_read(fs::path_view(file));
  REQUIRE(now.has_value());
  CHECK(now->size() == 7);
}

TEST_CASE("fs::open_file_views: a missing file reports an error") {
  const temp_dir dir("viewsmissing");
  const auto views = fs::open_file_views(fs::path_view(dir.child("absent")));
  REQUIRE_FALSE(views.has_value());
  CHECK(views.error().kind == fs::io_error::NotFound);
}

TEST_CASE("fs: a path past Windows' MAX_PATH works for every operation") {
  const temp_dir dir("long");
  // Six folders of sixty characters: well past 260 in all, each name within
  // every filesystem's limit for one.
  nx::string deep = dir.str();
  for (u32 i = 0; i < 6; ++i)
    deep = fs::path::join(deep.view(),
                          nx::format("{}{}", i, nx::string(59, 'd')).view());
  const nx::string file = fs::path::join(deep.view(), "payload.bin");
  REQUIRE(file.size() > 360u);

  REQUIRE(fs::create_directories(fs::path_view(deep)).has_value());
  CHECK(fs::status(fs::path_view(deep)) == fs::file_type::directory);
  const char payload[] = "long path";
  REQUIRE(fs::file_write_atomic(fs::path_view(file),
                                bytes_of(payload, sizeof(payload) - 1))
              .has_value());
  CHECK(fs::exists(fs::path_view(file)));
  CHECK(fs::stat_file(fs::path_view(file)).size == sizeof(payload) - 1);
  const auto read = fs::file_read(fs::path_view(file));
  REQUIRE(read.has_value());
  CHECK(read->size() == sizeof(payload) - 1);
  REQUIRE(fs::file_write_text(fs::path_view(file), "replaced").has_value());
  CHECK(fs::file_read_text(fs::path_view(file)).value_or("") == "replaced");
  CHECK(fs::open_file(fs::path_view(file)).has_value());

  const nx::string copied = fs::path::join(deep.view(), "copied.bin");
  REQUIRE(
      fs::copy_file(fs::path_view(file), fs::path_view(copied)).has_value());
  const nx::string moved = fs::path::join(deep.view(), "moved.bin");
  REQUIRE(fs::rename(fs::path_view(copied), fs::path_view(moved)).has_value());
  const auto listed = fs::directory_entries(fs::path_view(deep));
  REQUIRE(listed.has_value());
  CHECK(entry_names(*listed) ==
        std::vector<std::string>{"moved.bin", "payload.bin"});
  const auto all = fs::recursive_directory_entries(dir.view());
  REQUIRE(all.has_value());
  CHECK(all->size() == 8u);

  REQUIRE(fs::remove(fs::path_view(moved)).has_value());
  CHECK_FALSE(fs::exists(fs::path_view(moved)));
  REQUIRE(fs::remove_all(dir.view()).has_value());
  CHECK_FALSE(fs::exists(fs::path_view(file)));
}

#if defined(NX_PLATFORM_WINDOWS)

namespace {

/// What Win32 is handed for @p path, as UTF-8 for comparing.
[[nodiscard]] std::string win32(const nx::string_view path) {
  const nx::vector<wchar_t> wide = fs::detail::win32_path(path);
  std::string out;
  for (const wchar_t c : wide)
    if (c != L'\0')
      out.push_back(static_cast<char>(c));
  return out;
}

} // namespace

TEST_CASE("fs: Win32 gets a long path in its extended form, and a short one "
          "as it is") {
  const std::string tail(300, 'x');
  // Resolved first: separators, "." and "..", as the plain form would.
  CHECK(win32(nx::format("C:/a/./b/../{}", tail.c_str()).view()) ==
        "\\\\?\\C:\\a\\" + tail);
  CHECK(win32(nx::format("\\\\server\\share\\{}", tail.c_str()).view()) ==
        "\\\\?\\UNC\\server\\share\\" + tail);
  // Already extended, or short enough: left alone.
  const std::string extended = "\\\\?\\C:\\" + tail;
  CHECK(win32(nx::string_view(extended.c_str())) == extended);
  CHECK(win32("C:/short/path.txt") == "C:/short/path.txt");
  CHECK(win32("relative/short.txt") == "relative/short.txt");
  // A relative one resolves against the current directory.
  const std::string relative = win32(nx::string_view(tail.c_str()));
  CHECK(relative.rfind("\\\\?\\", 0) == 0);
  CHECK(relative.size() > tail.size() + 4);
  CHECK(relative.compare(relative.size() - tail.size(), tail.size(), tail) ==
        0);
  // The limit is the tightest plain-form one, CreateDirectoryW's.
  const std::string at =
      "C:\\" + std::string(fs::detail::WIN32_LONG_PATH - 3, 'y');
  CHECK(win32(nx::string_view(at.c_str())).rfind("\\\\?\\", 0) == 0);
  const std::string below = at.substr(0, at.size() - 1);
  CHECK(win32(nx::string_view(below.c_str())) == below);
  // Not UTF-8, or nothing.
  CHECK(fs::detail::win32_path("\xff").empty());
  CHECK(fs::detail::win32_path("").empty());
}

#endif

TEST_CASE("fs::file_lock: one holder at a time, until it lets go") {
  const temp_dir dir("lock");
  const nx::string file = dir.child("cook.manifest.lock");
  auto first = fs::file_lock::try_acquire(fs::path_view(file));
  REQUIRE(first.has_value());
  CHECK(first->held());
  CHECK(fs::exists(fs::path_view(file)));

  // Refused while held, without waiting.
  const auto second = fs::file_lock::try_acquire(fs::path_view(file));
  REQUIRE_FALSE(second.has_value());
  CHECK(second.error().kind == fs::io_error::Busy);

  // Moved, it is still held; released, it is free.
  fs::file_lock moved = std::move(*first);
  CHECK_FALSE(first->held());
  CHECK(moved.held());
  CHECK_FALSE(fs::file_lock::try_acquire(fs::path_view(file)).has_value());
  moved.release();
  CHECK_FALSE(moved.held());
  {
    auto third = fs::file_lock::try_acquire(fs::path_view(file));
    REQUIRE(third.has_value());
    CHECK(third->held());
  }
  // Destroyed, it let go; the file stays.
  CHECK(fs::file_lock::try_acquire(fs::path_view(file)).has_value());
  CHECK(fs::exists(fs::path_view(file)));
}

TEST_CASE("fs::file_lock: a missing folder reports why") {
  const temp_dir dir("lockmissing");
  const auto lock = fs::file_lock::try_acquire(
      fs::path_view(dir.child("nowhere/cook.manifest.lock")));
  REQUIRE_FALSE(lock.has_value());
  CHECK(lock.error().kind == fs::io_error::NotFound);
}
