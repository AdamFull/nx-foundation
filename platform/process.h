#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/core/result.h"
#include "core/foundation/strings/utf8_string.h"

#include <span>

namespace nx::process {

/// How a child process runs.
struct Options {
  /// An absolute path, or a name the system finds on PATH.
  string_view program;
  std::span<const string> arguments;
  /// NAME=value entries: the whole environment the child gets. Null gives
  /// it the parent's.
  const vector<string> *environment = nullptr;
  /// Past this it is killed, with every process it started.
  u32 timeout_ms = 30'000;
  /// Output past this is read and dropped, so the child never blocks on a
  /// full pipe.
  usize output_limit = 64 * 1024;
};

struct Outcome {
  /// -1 when it was killed.
  i32 exit_code = -1;
  /// Standard output and error together, in the order they came.
  string output;
  bool timed_out = false;
  bool truncated = false;
};

enum class Failure : u8 { Unsupported, NotFound, StartFailed, Io };

[[nodiscard]] string_view to_string(Failure failure) noexcept;

/// Whether this platform runs child processes: Windows, Linux and macOS.
[[nodiscard]] bool supported() noexcept;

/// Runs the program with no input and waits for it, up to its timeout. It
/// blocks: call it from nx::fiber::run_blocking or a thread of its own.
[[nodiscard]] result<Outcome, Failure> run(const Options &options);

/// The parent's environment, as NAME=value entries.
[[nodiscard]] vector<string> environment();

/// @p argument quoted for a Windows command line, so CommandLineToArgvW and
/// the C runtime read it back unchanged.
[[nodiscard]] string quote_argument(string_view argument);

} // namespace nx::process
