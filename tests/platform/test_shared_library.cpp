
#include "core/foundation/platform/filesystem.h"
#include "framework/nxtest.h"

#include "core/foundation/platform/shared_library.h"

namespace {

constexpr nx::string_view LIB = NX_TESTLIB_PATH;

using answer_fn = int (*)();
using add_fn = int (*)(int, int);

}

TEST_CASE("shared library: a fresh one holds nothing") {
  const nx::os::shared_library lib;
  CHECK_FALSE(lib.is_open());
  CHECK(lib.symbol("nx_testlib_answer") == nullptr);
  CHECK(lib.last_error().empty());
}

TEST_CASE("shared library: opening one finds its symbols") {
  if (!nx::os::shared_library::supported())
    SKIP("this platform does not load code at run time");

  nx::os::shared_library lib;
  REQUIRE(lib.open(LIB));
  CHECK(lib.is_open());
  CHECK(lib.last_error().empty());

  const auto answer =
      reinterpret_cast<answer_fn>(lib.symbol("nx_testlib_answer"));
  REQUIRE(answer != nullptr);
  CHECK(answer() == 42);

  const auto add = reinterpret_cast<add_fn>(lib.symbol("nx_testlib_add"));
  REQUIRE(add != nullptr);
  CHECK(add(2, 3) == 5);
}

TEST_CASE("shared library: a symbol that is not there is null, not a crash") {
  if (!nx::os::shared_library::supported())
    SKIP("this platform does not load code at run time");

  nx::os::shared_library lib;
  REQUIRE(lib.open(LIB));
  CHECK(lib.symbol("nx_testlib_absent") == nullptr);
  CHECK(lib.symbol("nx_testlib_answer") != nullptr);
}

TEST_CASE("shared library: a path that is not there fails and says so") {
  if (!nx::os::shared_library::supported())
    SKIP("this platform does not load code at run time");

  nx::os::shared_library lib;
  CHECK_FALSE(lib.open("no_such_library_anywhere"));
  CHECK_FALSE(lib.is_open());
  CHECK_FALSE(lib.last_error().empty());
}

TEST_CASE("shared library: closing drops the symbols") {
  if (!nx::os::shared_library::supported())
    SKIP("this platform does not load code at run time");

  nx::os::shared_library lib;
  REQUIRE(lib.open(LIB));
  lib.close();
  CHECK_FALSE(lib.is_open());
  CHECK(lib.symbol("nx_testlib_answer") == nullptr);

  lib.close();
  CHECK_FALSE(lib.is_open());
}

TEST_CASE("shared library: opening again replaces what was held") {
  if (!nx::os::shared_library::supported())
    SKIP("this platform does not load code at run time");

  nx::os::shared_library lib;
  REQUIRE(lib.open(LIB));
  REQUIRE(lib.open(LIB));
  CHECK(lib.is_open());
  CHECK(lib.symbol("nx_testlib_answer") != nullptr);
}

TEST_CASE("shared library: a failed open leaves it empty rather than stale") {
  if (!nx::os::shared_library::supported())
    SKIP("this platform does not load code at run time");

  nx::os::shared_library lib;
  REQUIRE(lib.open(LIB));
  CHECK_FALSE(lib.open("no_such_library_anywhere"));
  CHECK_FALSE(lib.is_open());
  CHECK(lib.symbol("nx_testlib_answer") == nullptr);
}

TEST_CASE("shared library: moving carries the handle, once") {
  if (!nx::os::shared_library::supported())
    SKIP("this platform does not load code at run time");

  nx::os::shared_library from;
  REQUIRE(from.open(LIB));

  nx::os::shared_library to = std::move(from);
  CHECK(to.is_open());
  CHECK(to.symbol("nx_testlib_answer") != nullptr);
  CHECK_FALSE(from.is_open());
}

TEST_CASE("shared library: decorate spells the name this platform uses") {
  const nx::string name = nx::os::shared_library::decorate("game");
  CHECK(name.find("game") != nx::string::npos);
  const bool known =
      name == "game.dll" || name == "libgame.so" || name == "libgame.dylib";
  CHECK(known);
}

TEST_CASE("shared library: one under a non-ASCII path opens") {
  if (!nx::os::shared_library::supported())
    SKIP("this platform does not load code at run time");
  const nx::string dir = nx::fs::path::join(nx::fs::path::parent_path(LIB),
                                            nx::string_view("библиотека_日本"));
  (void)nx::fs::remove_all(nx::fs::path_view(dir));
  REQUIRE(nx::fs::create_directories(nx::fs::path_view(dir)).has_value());
  const nx::string copy =
      nx::fs::path::join(dir.view(), nx::fs::path::filename(LIB));
  REQUIRE(
      nx::fs::copy_file(nx::fs::path_view(LIB), nx::fs::path_view(copy), true)
          .has_value());
  {
    nx::os::shared_library lib;
    REQUIRE(lib.open(copy));
    const auto answer =
        reinterpret_cast<answer_fn>(lib.symbol("nx_testlib_answer"));
    REQUIRE(answer != nullptr);
    CHECK(answer() == 42);
  }
  (void)nx::fs::remove_all(nx::fs::path_view(dir));
}
