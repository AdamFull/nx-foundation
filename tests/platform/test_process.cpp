#include "framework/nxtest.h"

#include "core/foundation/platform/os.h"
#include "core/foundation/platform/process.h"

namespace process = nx::process;

namespace {

/// @p command run by the system's shell.
[[nodiscard]] nx::result<process::Outcome, process::Failure>
shell(const nx::string_view command, const u32 timeout_ms = 10'000,
      const usize output_limit = 64 * 1024,
      const nx::vector<nx::string> *const environment = nullptr) {
#if NX_PLATFORM_WINDOWS
  const nx::string arguments[] = {nx::string("/d"), nx::string("/c"),
                                  nx::string(command)};
  const nx::string_view program = "cmd.exe";
#else
  const nx::string arguments[] = {nx::string("-c"), nx::string(command)};
  const nx::string_view program = "/bin/sh";
#endif
  return process::run({.program = program,
                       .arguments = arguments,
                       .environment = environment,
                       .timeout_ms = timeout_ms,
                       .output_limit = output_limit});
}

[[nodiscard]] bool has(const nx::string &text, const nx::string_view part) {
  return text.view().find(part) != nx::string_view::npos;
}

} // namespace

TEST_CASE("process: a child's output and exit code come back") {
  if (!process::supported())
    return SKIP("no child processes on this platform");
  const auto hello = shell("echo hello");
  REQUIRE(hello);
  CHECK(hello->exit_code == 0);
  CHECK(hello->output.view().starts_with("hello"));
  CHECK_FALSE(hello->timed_out);
  CHECK_FALSE(hello->truncated);
  const auto code = shell("exit 3");
  REQUIRE(code);
  CHECK(code->exit_code == 3);
  const auto error = shell("echo oops 1>&2");
  REQUIRE(error);
  CHECK(has(error->output, "oops"));
}

TEST_CASE("process: one that runs too long is killed") {
  if (!process::supported())
    return SKIP("no child processes on this platform");
  const u64 start = nx::os::ticks_ms();
#if NX_PLATFORM_WINDOWS
  const auto slow = shell("ping -n 30 127.0.0.1 >nul", 300);
#else
  const auto slow = shell("sleep 30", 300);
#endif
  REQUIRE(slow);
  CHECK(slow->timed_out);
  CHECK(slow->exit_code == -1);
  CHECK(nx::os::ticks_ms() - start < 5000);
}

TEST_CASE("process: output past the limit is dropped, and the child finishes") {
  if (!process::supported())
    return SKIP("no child processes on this platform");
#if NX_PLATFORM_WINDOWS
  const auto lots =
      shell("for /l %i in (1,1,3000) do @echo 0123456789", 20'000, 1000);
#else
  const auto lots =
      shell("i=0; while [ $i -lt 3000 ]; do echo 0123456789; i=$((i+1)); done",
            20'000, 1000);
#endif
  REQUIRE(lots);
  CHECK_FALSE(lots->timed_out);
  CHECK(lots->exit_code == 0);
  CHECK(lots->truncated);
  CHECK(lots->output.size() == 1000u);
}

TEST_CASE("process: the child gets the environment it is given") {
  if (!process::supported())
    return SKIP("no child processes on this platform");
  nx::vector<nx::string> without;
  for (const nx::string &entry : process::environment())
    if (!entry.view().starts_with("NX_PROCESS_TEST="))
      without.push_back(entry);
  nx::vector<nx::string> with = without;
  with.push_back(nx::string("NX_PROCESS_TEST=given"));
#if NX_PLATFORM_WINDOWS
  constexpr nx::string_view ECHO = "echo [%NX_PROCESS_TEST%]";
  constexpr nx::string_view UNSET = "[%NX_PROCESS_TEST%]";
#else
  constexpr nx::string_view ECHO = "echo [$NX_PROCESS_TEST]";
  constexpr nx::string_view UNSET = "[]";
#endif
  const auto given = shell(ECHO, 10'000, 1024, &with);
  REQUIRE(given);
  CHECK(has(given->output, "[given]"));
  const auto absent = shell(ECHO, 10'000, 1024, &without);
  REQUIRE(absent);
  CHECK(has(absent->output, UNSET));
}

TEST_CASE("process: a child has no input, and a missing program says so") {
  if (!process::supported())
    return SKIP("no child processes on this platform");
#if NX_PLATFORM_WINDOWS
  const auto reads = shell("findstr x & echo done", 5000);
#else
  const auto reads = shell("cat; echo done", 5000);
#endif
  REQUIRE(reads);
  CHECK_FALSE(reads->timed_out);
  CHECK(has(reads->output, "done"));
  const auto missing = process::run({.program = "nx_no_such_program_4711"});
  CHECK_FALSE(missing);
}

TEST_CASE("process: arguments are quoted as Windows reads them") {
  CHECK(process::quote_argument("plain") == "plain");
  CHECK(process::quote_argument("") == "\"\"");
  CHECK(process::quote_argument("a b") == "\"a b\"");
  CHECK(process::quote_argument("say \"hi\"") == "\"say \\\"hi\\\"\"");
  CHECK(process::quote_argument("C:\\path\\") == "C:\\path\\");
  CHECK(process::quote_argument("C:\\my path\\") == "\"C:\\my path\\\\\"");
  CHECK(process::quote_argument("a\\\"b") == "\"a\\\\\\\"b\"");
}
