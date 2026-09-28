#include "core/foundation/platform/process.h"

#include "core/foundation/core/scope_guard.h"
#include "core/foundation/platform/os.h"

#include <algorithm>

#if NX_HAS_WINDOWS_API
#include <windows.h>
#elif NX_PLATFORM_LINUX || NX_PLATFORM_MACOS
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace nx::process {
namespace {

/// Keeps what fits under the limit and notes what did not.
void keep(Outcome &out, const char *const bytes, const usize count,
          const usize limit) {
  const usize room = out.output.size() < limit ? limit - out.output.size() : 0;
  out.output.append(string_view(bytes, std::min(count, room)));
  if (count > room)
    out.truncated = true;
}

#if NX_HAS_WINDOWS_API
[[nodiscard]] vector<wchar_t> wide(const string_view text) {
  vector<wchar_t> out;
  const int count =
      text.empty() ? 0
                   : MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         cast<int>(text.size()), nullptr, 0);
  out.resize(cast<usize>(count) + 1, L'\0');
  if (count > 0)
    MultiByteToWideChar(CP_UTF8, 0, text.data(), cast<int>(text.size()),
                        out.data(), count);
  return out;
}

[[nodiscard]] string narrow(const wchar_t *const text, const usize count) {
  if (count == 0)
    return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, cast<int>(count),
                                        nullptr, 0, nullptr, nullptr);
  string out;
  out.resize(cast<usize>(bytes));
  WideCharToMultiByte(CP_UTF8, 0, text, cast<int>(count), out.data(), bytes,
                      nullptr, nullptr);
  return out;
}
#endif

} // namespace

string_view to_string(const Failure failure) noexcept {
  switch (failure) {
  case Failure::Unsupported:
    return "unsupported";
  case Failure::NotFound:
    return "not found";
  case Failure::StartFailed:
    return "start failed";
  case Failure::Io:
    return "i/o failed";
  }
  return "unknown";
}

string quote_argument(const string_view argument) {
  if (!argument.empty() &&
      argument.find_first_of(" \t\n\v\"") == string_view::npos)
    return string(argument);
  string out("\"");
  usize slashes = 0;
  for (const char c : argument) {
    if (c == '\\') {
      ++slashes;
      continue;
    }
    // Backslashes before a quote escape themselves and the quote.
    const usize repeat = c == '"' ? slashes * 2 + 1 : slashes;
    for (usize i = 0; i < repeat; ++i)
      out.push_back('\\');
    out.push_back(c);
    slashes = 0;
  }
  // And before the closing quote.
  for (usize i = 0; i < slashes * 2; ++i)
    out.push_back('\\');
  out.push_back('"');
  return out;
}

#if NX_HAS_WINDOWS_API

bool supported() noexcept { return true; }

vector<string> environment() {
  vector<string> out;
  wchar_t *const block = GetEnvironmentStringsW();
  if (block == nullptr)
    return out;
  for (const wchar_t *at = block; *at != L'\0';) {
    const usize length = wcslen(at);
    // "=C:=C:\\..." entries are the shell's per-drive folders, not variables.
    if (at[0] != L'=')
      out.push_back(narrow(at, length));
    at += length + 1;
  }
  FreeEnvironmentStringsW(block);
  return out;
}

result<Outcome, Failure> run(const Options &options) {
  string line = quote_argument(options.program);
  for (const string &argument : options.arguments) {
    line.push_back(' ');
    line.append(quote_argument(argument.view()).view());
  }
  vector<wchar_t> command = wide(line.view());

  vector<wchar_t> block;
  if (options.environment != nullptr) {
    vector<string> sorted(*options.environment);
    std::sort(sorted.begin(), sorted.end());
    for (const string &entry : sorted) {
      const vector<wchar_t> one = wide(entry.view());
      block.insert(block.end(), one.begin(), one.end());
    }
    if (block.empty())
      block.push_back(L'\0');
    block.push_back(L'\0');
  }

  SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE reader = nullptr;
  HANDLE writer = nullptr;
  if (!CreatePipe(&reader, &writer, &inherit, 0))
    return err(Failure::Io);
  const scope_guard close_pipe([&] {
    CloseHandle(reader);
    if (writer != nullptr)
      CloseHandle(writer);
  });
  SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
  const HANDLE nothing =
      CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  &inherit, OPEN_EXISTING, 0, nullptr);
  if (nothing == INVALID_HANDLE_VALUE)
    return err(Failure::Io);
  const scope_guard close_nothing([&] { CloseHandle(nothing); });

  // Closing the job kills the child and whatever it started.
  const HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr)
    return err(Failure::Io);
  const scope_guard close_job([&] { CloseHandle(job); });
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                          sizeof(limits));

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = nothing;
  startup.hStdOutput = writer;
  startup.hStdError = writer;
  PROCESS_INFORMATION child{};
  const DWORD flags =
      CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, flags,
                      options.environment != nullptr ? block.data() : nullptr,
                      nullptr, &startup, &child)) {
    const DWORD error = GetLastError();
    return err(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? Failure::NotFound
                   : Failure::StartFailed);
  }
  const scope_guard close_child([&] { CloseHandle(child.hProcess); });
  AssignProcessToJobObject(job, child.hProcess);
  ResumeThread(child.hThread);
  CloseHandle(child.hThread);
  // The pipe breaks once the child and everything it started are done.
  CloseHandle(writer);
  writer = nullptr;

  Outcome out;
  const u64 deadline = os::ticks_ms() + options.timeout_ms;
  char buffer[4096];
  bool exited = false;
  for (;;) {
    DWORD available = 0;
    const bool broken =
        !PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr);
    while (available > 0) {
      DWORD got = 0;
      if (!ReadFile(reader, buffer, std::min<DWORD>(available, sizeof(buffer)),
                    &got, nullptr) ||
          got == 0)
        break;
      keep(out, buffer, got, options.output_limit);
      available -= got;
    }
    exited = exited || WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0;
    if (exited && broken)
      break;
    if (os::ticks_ms() >= deadline) {
      TerminateJobObject(job, 1);
      WaitForSingleObject(child.hProcess, 2000);
      out.timed_out = true;
      return out;
    }
    if (exited || broken)
      Sleep(10);
    else
      WaitForSingleObject(child.hProcess, 10);
  }
  DWORD code = 0;
  GetExitCodeProcess(child.hProcess, &code);
  out.exit_code = cast<i32>(code);
  return out;
}

#elif NX_PLATFORM_LINUX || NX_PLATFORM_MACOS

bool supported() noexcept { return true; }

vector<string> environment() {
  vector<string> out;
  for (char **at = environ; at != nullptr && *at != nullptr; ++at)
    out.push_back(string(*at));
  return out;
}

result<Outcome, Failure> run(const Options &options) {
  int pipe_ends[2];
  if (pipe(pipe_ends) != 0)
    return err(Failure::Io);
  for (const int end : pipe_ends)
    fcntl(end, F_SETFD, FD_CLOEXEC);
  const int reader = pipe_ends[0];
  const scope_guard close_reader([&] { close(reader); });

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], 1);
  posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], 2);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  // A group of its own, so a timeout kills whatever it started as well.
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attributes, 0);

  vector<string> words;
  words.push_back(string(options.program));
  for (const string &argument : options.arguments)
    words.push_back(argument);
  vector<char *> argv;
  for (string &word : words)
    argv.push_back(word.data());
  argv.push_back(nullptr);
  vector<string> entries;
  vector<char *> envp;
  if (options.environment != nullptr) {
    entries = *options.environment;
    for (string &entry : entries)
      envp.push_back(entry.data());
    envp.push_back(nullptr);
  }

  pid_t pid = 0;
  const int started =
      posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(),
                   options.environment != nullptr ? envp.data() : environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  close(pipe_ends[1]);
  if (started != 0)
    return err(started == ENOENT ? Failure::NotFound : Failure::StartFailed);

  Outcome out;
  const u64 deadline = os::ticks_ms() + options.timeout_ms;
  char buffer[4096];
  bool finished = false;
  bool exited = false;
  int status = 0;
  for (;;) {
    if (!finished) {
      pollfd wait{reader, POLLIN, 0};
      if (poll(&wait, 1, 10) > 0) {
        const ssize_t got = read(reader, buffer, sizeof(buffer));
        if (got > 0)
          keep(out, buffer, cast<usize>(got), options.output_limit);
        else if (got == 0 || errno != EINTR)
          finished = true;
      }
    } else {
      usleep(10'000);
    }
    exited = exited || waitpid(pid, &status, WNOHANG) == pid;
    if (exited && finished)
      break;
    if (os::ticks_ms() >= deadline) {
      kill(-pid, SIGKILL);
      if (!exited)
        waitpid(pid, &status, 0);
      out.timed_out = true;
      return out;
    }
  }
  out.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return out;
}

#else

bool supported() noexcept { return false; }

vector<string> environment() { return {}; }

result<Outcome, Failure> run(const Options &) {
  return err(Failure::Unsupported);
}

#endif

} // namespace nx::process
