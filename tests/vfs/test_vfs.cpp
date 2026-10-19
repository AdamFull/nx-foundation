
#include "framework/nxtest.h"

#include "core/foundation/vfs/vfs_internal.h"
#include "vfs_fixture.h"

#include "core/foundation/platform/filesystem.h"

#include <cstring>

namespace vfs = nx::vfs;
using vfs_fixture::blob_of;
using vfs_fixture::bytes_of;
using vfs_fixture::has_name;
using vfs_fixture::read_eq;
using vfs_fixture::scoped_vfs;
using vfs_fixture::temp_dir;

TEST_CASE("vfs treats a bare path as rooted") {
  scoped_vfs guard;
  vfs::MemoryDevice *mem = vfs::make_memory_device();
  mem->add("/shaders/x.spv", blob_of("SPV"));
  vfs::mount("/", mem, 0);

  CHECK(read_eq("/shaders/x.spv", "SPV"));
  CHECK(read_eq("shaders/x.spv", "SPV"));
  CHECK(read_eq("/shaders/./x.spv", "SPV"));
  CHECK(read_eq("//shaders//x.spv", "SPV"));
  CHECK(read_eq("\\shaders\\x.spv", "SPV"));
  CHECK(read_eq("/shaders/sub/../x.spv", "SPV"));
}

TEST_CASE("vfs rejects traversal at the read boundary") {
  scoped_vfs guard;
  vfs::MemoryDevice *mem = vfs::make_memory_device();
  mem->add("/secret.txt", blob_of("s"));
  vfs::mount("/sub", mem, 0);

  CHECK_FALSE(vfs::read("/sub/../secret.txt").has_value());
  CHECK_FALSE(vfs::read("/../secret.txt").has_value());
  CHECK_FALSE(vfs::exists("/sub/../secret.txt"));
}

TEST_CASE("vfs memory device round-trips reads, stats and streams") {
  scoped_vfs guard;
  vfs::MemoryDevice *mem = vfs::make_memory_device();
  mem->add("/foo.txt", blob_of("hello"));
  mem->add("/dir/bar.txt", blob_of("bar"));
  vfs::mount("/", mem, 0);

  CHECK(read_eq("/foo.txt", "hello"));
  CHECK(vfs::exists("/foo.txt"));
  CHECK(vfs::stat("/foo.txt").size == 5);
  CHECK_FALSE(vfs::stat("/foo.txt").is_directory);
  CHECK(vfs::stat("/dir").is_directory);
  CHECK_FALSE(vfs::exists("/nope.txt"));

  SUBCASE("open gives a seekable stream") {
    auto s = vfs::open("/foo.txt");
    REQUIRE(s.has_value());
    CHECK((*s)->size() == 5);
    char buf[5] = {};
    CHECK((*s)->read(buf, 5) == 5);
    CHECK(std::memcmp(buf, "hello", 5) == 0);
    CHECK((*s)->eof());
    CHECK((*s)->seek(0, nx::fs::seek_origin::begin));
    CHECK((*s)->tell() == 0);
  }

  SUBCASE("reading a missing file reports NotFound") {
    auto r = vfs::read("/nope.txt");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == nx::fs::io_error::NotFound);
  }
}

TEST_CASE("vfs reports provider capabilities without host backing paths") {
  scoped_vfs guard;
  vfs::MemoryDevice *mem = vfs::make_memory_device();
  mem->add("asset.txt", blob_of("x"));
  REQUIRE(vfs::mount("/assets", mem, 17).valid());

  const nx::vector<vfs::MountInfo> mounted = vfs::mounts();
  REQUIRE(mounted.size() == 1u);
  CHECK(mounted[0].prefix == "/assets");
  CHECK(mounted[0].provider == "memory");
  CHECK(mounted[0].priority == 17);
  CHECK(mounted[0].writable);
  CHECK(mounted[0].watchable);
}

TEST_CASE("vfs memory device stamps a file every time it is written") {
  scoped_vfs guard;
  vfs::MemoryDevice *mem = vfs::make_memory_device();
  mem->add("/foo.txt", blob_of("hello"));
  vfs::mount("/", mem, 0);

  const u64 first = vfs::stat("/foo.txt").mtime_ns;
  CHECK(first != 0u);

  mem->add("/foo.txt", blob_of("hello again"));
  const u64 second = vfs::stat("/foo.txt").mtime_ns;
  CHECK(second > first);

  CHECK(vfs::write("/foo.txt", bytes_of("third")).has_value());
  const auto listed = vfs::list("/");
  REQUIRE(listed.has_value());
  u64 from_listing = 0;
  for (const vfs::DirEntry &entry : *listed)
    if (entry.name == "foo.txt")
      from_listing = entry.info.mtime_ns;
  CHECK(from_listing > second);
  CHECK(from_listing == vfs::stat("/foo.txt").mtime_ns);
}

TEST_CASE("vfs file generations track path and metadata") {
  scoped_vfs guard;
  vfs::MemoryDevice *mem = vfs::make_memory_device();
  mem->add("/a.txt", blob_of("one"));
  mem->add("/b.txt", blob_of("one"));
  REQUIRE(vfs::mount("/", mem).valid());

  const u64 first = vfs::file_generation("/a.txt");
  CHECK(first != vfs::file_generation("/b.txt"));
  REQUIRE(vfs::write_text("/a.txt", "two").has_value());
  CHECK(first != vfs::file_generation("/a.txt"));

  const nx::vector<nx::string> paths = {"/a.txt", "/b.txt"};
  CHECK(vfs::files_generation(paths) != 0);
}

TEST_CASE("vfs memory device serves the writable surface") {
  scoped_vfs guard;
  vfs::mount("/", vfs::make_memory_device(), 0);

  CHECK(vfs::write("/w.txt", bytes_of("written")).has_value());
  CHECK(read_eq("/w.txt", "written"));
  CHECK(vfs::write_text("/t.txt", "text").has_value());
  CHECK(read_eq("/t.txt", "text"));
  CHECK(vfs::remove("/w.txt").has_value());
  CHECK_FALSE(vfs::exists("/w.txt"));
  CHECK_FALSE(vfs::remove("/w.txt").has_value());
}

TEST_CASE("vfs read_text returns file contents as a string") {
  scoped_vfs guard;
  vfs::MemoryDevice *mem = vfs::make_memory_device();
  mem->add("/text.txt", blob_of("line one"));
  mem->add("/empty.txt", nx::blob<u8>{});
  vfs::mount("/", mem, 0);

  auto t = vfs::read_text("/text.txt");
  REQUIRE(t.has_value());
  CHECK(*t == "line one");

  auto e = vfs::read_text("/empty.txt");
  REQUIRE(e.has_value());
  CHECK(e->empty());

  CHECK_FALSE(vfs::read_text("/missing.txt").has_value());
}

TEST_CASE("vfs write fails when no writable mount serves the path") {
  scoped_vfs guard;
  CHECK_FALSE(vfs::write("/ro/x.txt", bytes_of("x")).has_value());
}

TEST_CASE("vfs host device writes atomically and reads back") {
  temp_dir dir("host");
  scoped_vfs guard;
  vfs::mount("/data", vfs::make_host_device(dir.str()), 0);

  REQUIRE(vfs::write("/data/w.txt", bytes_of("world")).has_value());
  CHECK(read_eq("/data/w.txt", "world"));
  const auto entries = nx::fs::directory_entries(dir.view());
  REQUIRE(entries.has_value());
  for (const nx::fs::directory_entry &e : entries.value())
    CHECK(nx::fs::path::extension(e.path.view()) != ".nxtmp");

  SUBCASE("nested paths create their parent directories") {
    REQUIRE(vfs::write("/data/a/b/deep.txt", bytes_of("deep")).has_value());
    CHECK(read_eq("/data/a/b/deep.txt", "deep"));
    CHECK(vfs::stat("/data/a").is_directory);
  }

  SUBCASE("overwriting replaces the contents") {
    REQUIRE(vfs::write("/data/w.txt", bytes_of("second")).has_value());
    CHECK(read_eq("/data/w.txt", "second"));
  }
}

TEST_CASE("vfs read-only host device serves files without accepting writes") {
  temp_dir dir("host_ro");
  REQUIRE(nx::fs::file_write_text(dir.file("legacy.txt"), "legacy")
              .has_value());
  scoped_vfs guard;
  vfs::mount("/legacy", vfs::make_host_device(
                            dir.str(), vfs::HostDeviceAccess::ReadOnly));

  CHECK(read_eq("/legacy/legacy.txt", "legacy"));
  CHECK_FALSE(vfs::write_text("/legacy/new.txt", "new").has_value());
  CHECK_FALSE(vfs::remove("/legacy/legacy.txt").has_value());
  CHECK(nx::fs::exists(dir.file("legacy.txt")));
}

TEST_CASE("vfs resolves mounts by priority then prefix length") {
  scoped_vfs guard;

  vfs::MemoryDevice *low = vfs::make_memory_device();
  low->add("a.txt", blob_of("LOW"));
  low->add("only_low.txt", blob_of("ONLY"));
  vfs::mount("/", low, 0);

  vfs::MemoryDevice *high = vfs::make_memory_device();
  high->add("a.txt", blob_of("HIGH"));
  vfs::mount("/", high, 100);

  SUBCASE("higher priority wins") { CHECK(read_eq("/a.txt", "HIGH")); }
  SUBCASE("misses fall through to the lower mount") {
    CHECK(read_eq("/only_low.txt", "ONLY"));
  }
  SUBCASE("a longer prefix wins at equal priority") {
    vfs::MemoryDevice *sub = vfs::make_memory_device();
    sub->add("a.txt", blob_of("SUB"));
    vfs::mount("/sub", sub, 0);
    CHECK(read_eq("/sub/a.txt", "SUB"));
  }
}

TEST_CASE("vfs unmount removes a mount and its device") {
  scoped_vfs guard;
  vfs::MemoryDevice *mem = vfs::make_memory_device();
  mem->add("x.txt", blob_of("X"));
  const vfs::MountId id = vfs::mount("/", mem, 0);
  REQUIRE(id.valid());
  CHECK(vfs::exists("/x.txt"));

  vfs::unmount(id);
  CHECK_FALSE(vfs::exists("/x.txt"));
  vfs::unmount(id);
  vfs::unmount(vfs::MountId{});
}

TEST_CASE("vfs a stale mount handle never aliases a later mount") {
  scoped_vfs guard;
  vfs::MemoryDevice *first = vfs::make_memory_device();
  first->add("a.txt", blob_of("FIRST"));
  const vfs::MountId stale = vfs::mount("/", first, 0);
  vfs::unmount(stale);

  vfs::MemoryDevice *second = vfs::make_memory_device();
  second->add("a.txt", blob_of("SECOND"));
  const vfs::MountId live = vfs::mount("/", second, 0);
  REQUIRE(read_eq("/a.txt", "SECOND"));

  vfs::unmount(stale);
  CHECK(read_eq("/a.txt", "SECOND"));

  vfs::unmount(live);
  CHECK_FALSE(vfs::exists("/a.txt"));
}

TEST_CASE("vfs unmount_all clears every mount") {
  scoped_vfs guard;
  vfs::MemoryDevice *a = vfs::make_memory_device();
  a->add("a.txt", blob_of("A"));
  vfs::MemoryDevice *b = vfs::make_memory_device();
  b->add("b.txt", blob_of("B"));
  vfs::mount("/", a, 0);
  vfs::mount("/other", b, 0);
  CHECK(vfs::exists("/a.txt"));

  vfs::unmount_all();
  CHECK_FALSE(vfs::exists("/a.txt"));
  CHECK_FALSE(vfs::exists("/other/b.txt"));
  vfs::unmount_all();
}

TEST_CASE("vfs mount rejects a null device and an illegal prefix") {
  scoped_vfs guard;
  CHECK_FALSE(vfs::mount("/", nullptr, 0).valid());
  CHECK_FALSE(vfs::mount("/../evil", vfs::make_memory_device(), 0).valid());
}

TEST_CASE("vfs list merges mounts and reports each name once") {
  scoped_vfs guard;

  vfs::MemoryDevice *low = vfs::make_memory_device();
  low->add("shared.txt", blob_of("LOW"));
  low->add("low_only.txt", blob_of("L"));
  low->add("sub/deep.txt", blob_of("D"));
  vfs::mount("/", low, 0);

  vfs::MemoryDevice *high = vfs::make_memory_device();
  high->add("shared.txt", blob_of("HIGH"));
  high->add("high_only.txt", blob_of("H"));
  vfs::mount("/", high, 100);

  auto ls = vfs::list("/");
  REQUIRE(ls.has_value());
  CHECK(has_name(*ls, "shared.txt"));
  CHECK(has_name(*ls, "low_only.txt"));
  CHECK(has_name(*ls, "high_only.txt"));
  CHECK(has_name(*ls, "sub"));

  usize shared_count = 0;
  for (const vfs::DirEntry &e : *ls)
    if (e.name == "shared.txt")
      ++shared_count;
  CHECK(shared_count == 1);

  SUBCASE("subdirectories list their own children") {
    auto sub = vfs::list("/sub");
    REQUIRE(sub.has_value());
    CHECK(has_name(*sub, "deep.txt"));
    CHECK(sub->size() == 1);
  }

  SUBCASE("listing a missing directory fails") {
    CHECK_FALSE(vfs::list("/does_not_exist").has_value());
  }
}

TEST_CASE("vfs list reports directory entries as directories") {
  scoped_vfs guard;
  vfs::MemoryDevice *mem = vfs::make_memory_device();
  mem->add("dir/a.txt", blob_of("a"));
  mem->add("file.txt", blob_of("f"));
  vfs::mount("/", mem, 0);

  auto ls = vfs::list("/");
  REQUIRE(ls.has_value());
  for (const vfs::DirEntry &e : *ls) {
    if (e.name == "dir")
      CHECK(e.info.is_directory);
    if (e.name == "file.txt") {
      CHECK_FALSE(e.info.is_directory);
      CHECK(e.info.size == 1);
    }
  }
}

TEST_CASE("vfs initialize and shutdown are idempotent") {
  vfs::shutdown();
  CHECK_FALSE(vfs::initialized());

  CHECK(vfs::initialize());
  CHECK(vfs::initialized());
  CHECK(vfs::initialize());
  CHECK(vfs::initialized());

  vfs::shutdown();
  CHECK_FALSE(vfs::initialized());
  vfs::shutdown();
  CHECK_FALSE(vfs::initialized());
}

TEST_CASE("vfs calls before initialize are safe") {
  vfs::shutdown();
  REQUIRE_FALSE(vfs::initialized());

  CHECK_FALSE(vfs::mount("/", vfs::make_memory_device(), 0).valid());
  CHECK_FALSE(vfs::read("/x").has_value());
  CHECK_FALSE(vfs::exists("/x"));
  CHECK_FALSE(vfs::write("/x", bytes_of("x")).has_value());
  CHECK_FALSE(vfs::list("/").has_value());
}

TEST_CASE("vfs resolves a path a file names beside that file") {
  CHECK(vfs::resolve_beside("/mods/k/1/her/doll.nxpdoll", "her.nxatlaspack") ==
        "/mods/k/1/her/her.nxatlaspack");
  CHECK(vfs::resolve_beside("/mods/k/1/her/doll.nxpdoll", "parts/a.png") ==
        "/mods/k/1/her/parts/a.png");
  CHECK(vfs::resolve_beside("/doll.nxpdoll", "a.png") == "/a.png");
  CHECK(vfs::resolve_beside("/her/doll.nxpdoll", "/shared/a.png") ==
        "/shared/a.png");
  CHECK(vfs::resolve_beside("/her/doll.nxpdoll", "") == "");
  // Only checked: stays relative.
  CHECK(vfs::resolve_beside("", "parts/a.png") == "parts/a.png");
  for (const nx::string_view bad :
       {"../a.png", "./a.png", "parts/../a.png", "parts//a.png", "parts/",
        "parts/.", ".."}) {
    CHECK_FALSE(vfs::resolve_beside("/her/doll.nxpdoll", bad).has_value());
    CHECK_FALSE(vfs::resolve_beside("", bad).has_value());
  }
}

TEST_CASE("vfs canonical paths refuse what could leave a mount") {
  using nx::vfs::detail::canon_vpath;
  CHECK(canon_vpath("") == nx::string_view("/"));
  CHECK(canon_vpath("a\\b/./c//") == nx::string_view("/a/b/c"));
  CHECK(canon_vpath("/a/../b") == nx::string_view("/b"));
  for (const nx::string_view bad :
       {"..", "/../a", "a\\..\\..\\b", "/C:/Windows/win.ini", "C:\\x",
        "/a.txt:hidden", "/a/b:c/d"})
    CHECK(canon_vpath(bad).empty());
}

TEST_CASE("vfs a host mount cannot be left through a drive or a parent") {
  temp_dir jail("jail");
  temp_dir outside("outside");
  REQUIRE(nx::fs::file_write_text(outside.file("secret.txt"), "secret")
              .has_value());
  scoped_vfs guard;
  vfs::Device *const host = vfs::make_host_device(jail.str());
  vfs::mount("/", host, 0);

  const nx::string secret = outside.file("secret.txt");
  const nx::string through_root = nx::string("/") + secret.view();
  CHECK_FALSE(vfs::read(through_root).has_value());
  CHECK_FALSE(vfs::exists(through_root));
  CHECK_FALSE(host->read(secret).has_value());
  CHECK_FALSE(host->stat(secret).exists);

  const nx::string planted_path = outside.file("planted.txt");
  CHECK_FALSE(vfs::write(nx::string("/") + planted_path.view(), bytes_of("x"))
                  .has_value());
  CHECK_FALSE(host->write(planted_path, bytes_of("x")).has_value());
  CHECK_FALSE(nx::fs::exists(planted_path));

  CHECK_FALSE(vfs::read("/..\\outside\\secret.txt").has_value());
  CHECK_FALSE(host->read("..\\outside\\secret.txt").has_value());
  CHECK_FALSE(vfs::write("/notes.txt:hidden", bytes_of("x")).has_value());

  REQUIRE(vfs::write("/inside.txt", bytes_of("in")).has_value());
  CHECK(read_eq("/inside.txt", "in"));
}

TEST_CASE("vfs resolve_beside refuses backslashes, drives and streams") {
  for (const nx::string_view bad :
       {"..\\..\\x", "parts\\..\\a.png", "parts\\.\\a.png", "C:/x",
        "a.png:stream"}) {
    CHECK_FALSE(vfs::resolve_beside("/her/doll.nxpdoll", bad).has_value());
    CHECK_FALSE(vfs::resolve_beside("", bad).has_value());
  }
  CHECK(vfs::resolve_beside("/her/doll.nxpdoll", "\\shared\\a.png") ==
        "\\shared\\a.png");
}
