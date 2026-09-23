#include "core/foundation/diagnostics/crash.h"

#include "core/foundation/core/time.h"
#include "core/foundation/diagnostics/log.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/platform/os.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/threading/sync.h"
#include "core/foundation/threading/thread.h"

#include <atomic>
#include <cerrno>
#include <chrono>

#if NX_HAS_WINDOWS_API
#include <windows.h>
// Do not reorder!
#include <dbghelp.h>
#include <signal.h>
#elif NX_PLATFORM_POSIX
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace nx::crash {
namespace {

constexpr usize PATH_CAPACITY = 1024;
constexpr usize PREAMBLE_CAPACITY = 8192;
constexpr usize TAIL_MESSAGE_CAPACITY = 256;

// A handler cannot format with anything that allocates or takes a lock, so it
// gets this: a fixed buffer flushed through one raw write per fill.
class Out {
public:
#if NX_HAS_WINDOWS_API
  using handle_type = HANDLE;
#else
  using handle_type = int;
#endif

  // A function rather than a constexpr member: INVALID_HANDLE_VALUE is a cast
  // from -1, which clang-cl rejects as an initialiser for one.
  [[nodiscard]] static handle_type no_handle() noexcept {
#if NX_HAS_WINDOWS_API
    return INVALID_HANDLE_VALUE;
#else
    return -1;
#endif
  }

  explicit Out(const handle_type handle) noexcept : m_handle(handle) {}
  ~Out() { flush(); }

  Out(const Out &) = delete;
  Out &operator=(const Out &) = delete;

  void text(const char *const chars, const usize length) noexcept {
    for (usize i = 0; i < length; ++i) {
      if (m_used == sizeof(m_buffer))
        flush();
      m_buffer[m_used++] = chars[i];
    }
  }
  void text(const string_view chars) noexcept {
    text(chars.data(), chars.size());
  }
  void text(const char *const chars) noexcept {
    usize length = 0;
    while (chars != nullptr && chars[length] != '\0')
      ++length;
    text(chars, length);
  }

  void dec(u64 value) noexcept {
    char digits[24];
    usize count = 0;
    do {
      digits[count++] = nx::cast<char>('0' + (value % 10));
      value /= 10;
    } while (value != 0);
    while (count > 0)
      text(&digits[--count], 1);
  }

  void hex(const u64 value, const usize width = 16) noexcept {
    static constexpr char alphabet[] = "0123456789abcdef";
    char digits[16];
    for (usize i = 0; i < 16; ++i)
      digits[15 - i] = alphabet[(value >> (i * 4)) & 0xfull];
    const usize skip = 16 - (width > 16 ? 16 : width);
    text("0x", 2);
    text(&digits[skip], 16 - skip);
  }

  void line(const char *const label, const string_view value) noexcept {
    if (value.empty())
      return;
    text(label);
    text(value);
    text("\n", 1);
  }

  void nl() noexcept { text("\n", 1); }

  void flush() noexcept {
    if (m_used == 0 || m_handle == no_handle())
      return;
#if NX_HAS_WINDOWS_API
    DWORD wrote = 0;
    ::WriteFile(m_handle, m_buffer, nx::cast<DWORD>(m_used), &wrote, nullptr);
#else
    const ssize_t wrote = ::write(m_handle, m_buffer, m_used);
    (void)wrote;
#endif
    m_used = 0;
  }

private:
  handle_type m_handle;
  usize m_used = 0;
  char m_buffer[4096] = {};
};

struct Note {
  bool occupied = false;
  char key[NOTE_KEY_CAPACITY] = {};
  char value[NOTE_VALUE_CAPACITY] = {};
  u16 length = 0;
};

struct TailRecord {
  u64 timestamp_ns = 0;
  u64 thread_id = 0;
  const log::CategoryState *category = nullptr;
  log::Level level = log::Level::Info;
  u16 length = 0;
  char message[TAIL_MESSAGE_CAPACITY] = {};
};

struct State {
  std::atomic<bool> installed{false};
  // A second fault inside the handler must not restart it; the first report is
  // the one worth having and a loop here writes nothing at all.
  std::atomic<bool> handling{false};

  char report_path[PATH_CAPACITY] = {};
  char dump_path[PATH_CAPACITY] = {};
  char directory[PATH_CAPACITY] = {};
  usize directory_length = 0;

  char preamble[PREAMBLE_CAPACITY] = {};
  usize preamble_length = 0;

  mutex notes_mutex;
  Note notes[NOTE_CAPACITY];

  mutex tail_mutex;
  TailRecord tail[LOG_TAIL_CAPACITY];
  u64 tail_written = 0;
  std::atomic<bool> tail_enabled{false};

  u64 started_ns = 0;
  u64 started_wall_ns = 0;
  u32 pid = 0;
  bool minidump = false;
  bool minidump_full = false;

  char last_report[PATH_CAPACITY] = {};
  std::atomic<u32> fatal_count{0};
  log::Sink *tail_sink = nullptr;
};

State g_state;

State &state() noexcept { return g_state; }

const char *thread_label() noexcept {
  const char *const named = nx::this_thread::name();
  return named[0] != 0 ? named : "(unnamed)";
}

usize copy_into(char *const out, const usize capacity,
                const string_view text) noexcept {
  if (capacity == 0)
    return 0;
  const usize kept = text.size() < capacity - 1 ? text.size() : capacity - 1;
  for (usize i = 0; i < kept; ++i)
    out[i] = text[i];
  out[kept] = '\0';
  return kept;
}

class TailSink final : public log::Sink {
public:
  void write(const log::Record &record) override {
    State &s = state();
    const scoped_lock lock(s.tail_mutex);
    const u64 slot = s.tail_written;
    TailRecord &into = s.tail[slot % LOG_TAIL_CAPACITY];
    into.timestamp_ns = record.timestamp_ns;
    into.thread_id = record.thread_id;
    into.category = record.category;
    into.level = record.level;
    into.length =
        nx::cast<u16>(copy_into(into.message, TAIL_MESSAGE_CAPACITY,
                                string_view(record.message, record.length)));
    s.tail_written = slot + 1;
  }
};

void gather_preamble(const Config &config) {
  State &s = state();
  char *const buffer = s.preamble;
  usize used = 0;

  const auto put = [&](const string_view text) {
    const usize room = PREAMBLE_CAPACITY - 1 - used;
    const usize kept = text.size() < room ? text.size() : room;
    for (usize i = 0; i < kept; ++i)
      buffer[used + i] = text[i];
    used += kept;
  };
  char scratch[1024];
  const auto say = [&](const char *const label, const string_view value) {
    if (value.empty())
      return;
    put(label);
    put(value);
    put("\n");
  };

  put("application  : ");
  put(config.application);
  put("\n");
  say("version      : ", config.version);
  say("build        : ", NX_BUILD_CONFIG);
  say("platform     : ", NX_PLATFORM_NAME);
  say("architecture : ", NX_ARCH_NAME);
  put(nx::format_to(scratch, "compiler     : {} {}\n", NX_COMPILER_NAME,
                    NX_COMPILER_VERSION));
  put(nx::format_to(scratch, "compiled     : {} {}\n", __DATE__, __TIME__));

  char text[512];
  os::os_version(text, sizeof(text));
  say("os           : ", text);
  os::device_model(text, sizeof(text));
  say("device       : ", text);
  os::cpu_brand(text, sizeof(text));
  say("cpu          : ", text);

  const nx::cpu::topology_info &topology = nx::cpu::topology();
  put(nx::format_to(
      scratch, "cores        : {} logical, {} physical, {} package(s)\n",
      topology.logical_count, topology.physical_count, topology.package_count));
  if (topology.efficiency_class_count > 1)
    put(nx::format_to(scratch, "hybrid       : {} performance, {} efficiency\n",
                      nx::cpu::performance_core_count(),
                      nx::cpu::efficiency_core_count()));
  put(nx::format_to(scratch, "cache line   : {} bytes\n",
                    topology.cache_line_size));
  for (const nx::cpu::cache_info &cache : topology.caches) {
    const char *const kind = cache.type == nx::cpu::cache_type::Data ? "d"
                             : cache.type == nx::cpu::cache_type::Instruction
                                 ? "i"
                                 : "u";
    put(nx::format_to(scratch, "cache L{}{}    : {} KiB x{}\n", cache.level,
                      kind, cache.size / 1024, cache.instance_count));
  }

  put(nx::format_to(scratch, "ram          : {} MiB\n", os::system_ram_mb()));
  put(nx::format_to(scratch, "page size    : {} bytes\n",
                    nx::vmem::get_page_size()));

  os::executable_path(text, sizeof(text));
  say("executable   : ", text);
  put(nx::format_to(scratch, "pid          : {}\n", s.pid));

  buffer[used] = '\0';
  s.preamble_length = used;
}

void write_notes(Out &out, const bool best_effort = false) noexcept {
  State &s = state();
  Note snapshot[NOTE_CAPACITY];
  if (best_effort) {
    if (!s.notes_mutex.try_lock())
      return;
  } else {
    s.notes_mutex.lock();
  }
  for (usize i = 0; i < NOTE_CAPACITY; ++i)
    snapshot[i] = s.notes[i];
  s.notes_mutex.unlock();

  bool any = false;
  for (const Note &note : snapshot) {
    if (!note.occupied)
      continue;
    if (!any) {
      out.text("\n--- notes ---\n");
      any = true;
    }
    out.text(note.key);
    out.text(" : ");
    out.text(note.value, note.length);
    out.nl();
  }
}

void write_tail(Out &out, const bool best_effort = false) noexcept {
  State &s = state();
  if (!s.tail_enabled.load(std::memory_order_acquire))
    return;

  TailRecord snapshot[LOG_TAIL_CAPACITY];
  if (best_effort) {
    if (!s.tail_mutex.try_lock())
      return;
  } else {
    s.tail_mutex.lock();
  }
  const u64 written = s.tail_written;
  if (written == 0) {
    s.tail_mutex.unlock();
    return;
  }

  const u64 count = written < LOG_TAIL_CAPACITY ? written : LOG_TAIL_CAPACITY;
  for (u64 i = 0; i < count; ++i)
    snapshot[i] = s.tail[(written - count + i) % LOG_TAIL_CAPACITY];
  s.tail_mutex.unlock();

  out.text("\n--- log (most recent ");
  out.dec(count);
  out.text(") ---\n");

  for (u64 i = 0; i < count; ++i) {
    const TailRecord &record = snapshot[i];
    // Milliseconds since the process started, because turning a wall-clock
    // epoch into a date needs libc calls a handler must not make.
    const u64 since = record.timestamp_ns > s.started_wall_ns
                          ? (record.timestamp_ns - s.started_wall_ns) / 1000000u
                          : 0;
    out.text("[+");
    out.dec(since);
    out.text("ms ");
    out.text(log::level_name(record.level));
    out.text(" ");
    out.text(record.category != nullptr ? record.category->name : "?");
    out.text("] ");
    out.text(record.message, record.length);
    out.nl();
  }
}

void write_runtime(Out &out) noexcept {
  State &s = state();
  const os::MemStats memory = os::mem_get_stats();
  out.text("uptime       : ");
  out.dec((os::ticks_ns() - s.started_ns) / 1000000u);
  out.text(" ms\nrss          : ");
  out.dec(memory.rss / 1024u);
  out.text(" KiB\npeak rss     : ");
  out.dec(memory.peak_rss / 1024u);
  out.text(" KiB\nheap         : ");
  out.dec(memory.heap_committed / 1024u);
  out.text(" KiB\n");
}

Out::handle_type open_report(const char *const path) noexcept {
#if NX_HAS_WINDOWS_API
  return ::CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
  return ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif
}

void close_report(const Out::handle_type handle) noexcept {
  if (handle == Out::no_handle())
    return;
#if NX_HAS_WINDOWS_API
  ::CloseHandle(handle);
#else
  ::close(handle);
#endif
}

#if NX_PLATFORM_POSIX

const char *signal_name(const int number) noexcept {
  switch (number) {
  case SIGSEGV:
    return "SIGSEGV (invalid memory reference)";
  case SIGBUS:
    return "SIGBUS (bad memory access)";
  case SIGFPE:
    return "SIGFPE (arithmetic fault)";
  case SIGILL:
    return "SIGILL (illegal instruction)";
  case SIGABRT:
    return "SIGABRT (abort)";
  case SIGTRAP:
    return "SIGTRAP (trap)";
  default:
    return "signal";
  }
}

constexpr int HANDLED_SIGNALS[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
struct sigaction g_previous[nx::array_size(HANDLED_SIGNALS)];
std::atomic_flag g_handling_signal = ATOMIC_FLAG_INIT;
char g_alt_stack[65536];

void handle_signal(const int number, siginfo_t *const info,
                   void *const context) {
  const int saved_errno = errno;
  State &s = state();
  if (!g_handling_signal.test_and_set(std::memory_order_relaxed)) {
    const Out::handle_type handle = open_report(s.report_path);
    {
      Out out(handle);
      out.text("=== nx2d crash report ===\n\n");
      out.text("signal       : ");
      out.text(signal_name(number));
      out.nl();
      out.text("code         : ");
      out.dec(nx::cast<u64>(info != nullptr ? info->si_code : 0));
      out.text("\nfault address: ");
      out.hex(reinterpret_cast<uintptr_t>(info != nullptr ? info->si_addr
                                                          : nullptr));
      out.nl();
      out.text(s.preamble, s.preamble_length);
      out.text("\nUse the OS tombstone/core dump for the crashing thread's "
               "registers and backtrace.\n");
    }
    close_report(handle);
  }

  for (usize i = 0; i < nx::array_size(HANDLED_SIGNALS); ++i)
    if (HANDLED_SIGNALS[i] == number)
      ::sigaction(number, &g_previous[i], nullptr);
  (void)context;
  errno = saved_errno;
  if (::raise(number) != 0)
    ::_exit(128 + number);
}

void install_handlers() {
  stack_t alt = {};
  alt.ss_sp = g_alt_stack;
  alt.ss_size = sizeof(g_alt_stack);
  alt.ss_flags = 0;
  // Without this a stack overflow has no stack left to report itself on.
  ::sigaltstack(&alt, nullptr);

  struct sigaction action = {};
  action.sa_sigaction = &handle_signal;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
  ::sigemptyset(&action.sa_mask);
  for (usize i = 0; i < nx::array_size(HANDLED_SIGNALS); ++i)
    ::sigaction(HANDLED_SIGNALS[i], &action, &g_previous[i]);
}

void remove_handlers() {
  for (usize i = 0; i < nx::array_size(HANDLED_SIGNALS); ++i)
    ::sigaction(HANDLED_SIGNALS[i], &g_previous[i], nullptr);
}

#elif NX_HAS_WINDOWS_API

constexpr usize BACKTRACE_CAPACITY = 64;

using MiniDumpWriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                           PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION,
                                           PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFn g_write_dump = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

const char *exception_name(const DWORD code) noexcept {
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION:
    return "EXCEPTION_ACCESS_VIOLATION";
  case EXCEPTION_STACK_OVERFLOW:
    return "EXCEPTION_STACK_OVERFLOW";
  case EXCEPTION_ILLEGAL_INSTRUCTION:
    return "EXCEPTION_ILLEGAL_INSTRUCTION";
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
    return "EXCEPTION_INT_DIVIDE_BY_ZERO";
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
  case EXCEPTION_PRIV_INSTRUCTION:
    return "EXCEPTION_PRIV_INSTRUCTION";
  case EXCEPTION_IN_PAGE_ERROR:
    return "EXCEPTION_IN_PAGE_ERROR";
  case EXCEPTION_DATATYPE_MISALIGNMENT:
    return "EXCEPTION_DATATYPE_MISALIGNMENT";
  case EXCEPTION_BREAKPOINT:
    return "EXCEPTION_BREAKPOINT";
  case 0xE06D7363u:
    return "C++ exception (unhandled)";
  default:
    return "exception";
  }
}

void write_dump(EXCEPTION_POINTERS *const pointers,
                const DWORD thread_id) noexcept {
  State &s = state();
  if (!s.minidump || g_write_dump == nullptr)
    return;

  const HANDLE file =
      ::CreateFileA(s.dump_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;

  MINIDUMP_EXCEPTION_INFORMATION info = {};
  info.ThreadId = thread_id;
  info.ExceptionPointers = pointers;
  info.ClientPointers = FALSE;

  DWORD type = MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs |
               MiniDumpWithHandleData | MiniDumpWithThreadInfo |
               MiniDumpWithUnloadedModules;
  if (s.minidump_full)
    type |= MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo;

  g_write_dump(::GetCurrentProcess(), ::GetCurrentProcessId(), file,
               nx::cast<MINIDUMP_TYPE>(type),
               pointers != nullptr ? &info : nullptr, nullptr, nullptr);
  ::CloseHandle(file);
}

void write_stack(Out &out, CONTEXT *const context) noexcept {
  if (context == nullptr)
    return;
  out.text("\n--- backtrace ---\n");

  STACKFRAME64 frame = {};
  CONTEXT walking = *context;
  DWORD machine = 0;
#if defined(_M_X64)
  machine = IMAGE_FILE_MACHINE_AMD64;
  frame.AddrPC.Offset = walking.Rip;
  frame.AddrFrame.Offset = walking.Rbp;
  frame.AddrStack.Offset = walking.Rsp;
#elif defined(_M_ARM64)
  machine = IMAGE_FILE_MACHINE_ARM64;
  frame.AddrPC.Offset = walking.Pc;
  frame.AddrFrame.Offset = walking.Fp;
  frame.AddrStack.Offset = walking.Sp;
#else
  return;
#endif
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Mode = AddrModeFlat;

  const HANDLE process = ::GetCurrentProcess();
  const HANDLE thread = ::GetCurrentThread();
  for (usize i = 0; i < BACKTRACE_CAPACITY; ++i) {
    if (!::StackWalk64(machine, process, thread, &frame, &walking, nullptr,
                       ::SymFunctionTableAccess64, ::SymGetModuleBase64,
                       nullptr))
      break;
    if (frame.AddrPC.Offset == 0)
      break;

    out.text("#");
    out.dec(i);
    out.text(i < 10 ? "  " : " ");
    out.hex(frame.AddrPC.Offset);

    const DWORD64 base = ::SymGetModuleBase64(process, frame.AddrPC.Offset);
    if (base != 0) {
      char module[MAX_PATH] = {};
      if (::GetModuleFileNameA(reinterpret_cast<HMODULE>(base), module,
                               sizeof(module)) != 0) {
        out.text(" ");
        out.text(module);
        out.text("+");
        out.hex(frame.AddrPC.Offset - base, 8);
      }
    }
    out.nl();
  }
}

void write_registers(Out &out, CONTEXT *const context) noexcept {
  if (context == nullptr)
    return;
  out.text("\n--- registers ---\n");
#if defined(_M_X64)
  const struct {
    const char *name;
    DWORD64 value;
  } named[] = {
      {"rax", context->Rax}, {"rbx", context->Rbx}, {"rcx", context->Rcx},
      {"rdx", context->Rdx}, {"rsi", context->Rsi}, {"rdi", context->Rdi},
      {"rbp", context->Rbp}, {"rsp", context->Rsp}, {"r8 ", context->R8},
      {"r9 ", context->R9},  {"r10", context->R10}, {"r11", context->R11},
      {"r12", context->R12}, {"r13", context->R13}, {"r14", context->R14},
      {"r15", context->R15}, {"rip", context->Rip}};
  for (usize i = 0; i < nx::array_size(named); ++i) {
    out.text(named[i].name);
    out.text(" ");
    out.hex(named[i].value);
    out.text((i % 4) == 3 ? "\n" : "  ");
  }
  out.nl();
#elif defined(_M_ARM64)
  for (usize i = 0; i < 31; ++i) {
    out.text("x");
    out.dec(i);
    out.text(" ");
    out.hex(context->X[i]);
    out.text((i % 4) == 3 ? "\n" : "  ");
  }
  out.text("\npc  ");
  out.hex(context->Pc);
  out.text("  sp  ");
  out.hex(context->Sp);
  out.nl();
#endif
}

struct Request {
  EXCEPTION_POINTERS *pointers = nullptr;
  DWORD thread_id = 0;
  const char *thread_name = "";
  const char *reason = nullptr;
};

void write_report(const Request &request) noexcept {
  State &s = state();
  write_dump(request.pointers, request.thread_id);

  const Out::handle_type handle = open_report(s.report_path);
  {
    Out out(handle);
    out.text("=== nx2d crash report ===\n\n");
    const EXCEPTION_RECORD *const record =
        request.pointers != nullptr ? request.pointers->ExceptionRecord
                                    : nullptr;
    out.text("exception    : ");
    if (request.reason != nullptr) {
      out.text(request.reason);
    } else {
      out.text(exception_name(record != nullptr ? record->ExceptionCode : 0));
      out.text("\ncode         : ");
      out.hex(record != nullptr ? record->ExceptionCode : 0, 8);
      out.text("\naddress      : ");
      out.hex(reinterpret_cast<uintptr_t>(
          record != nullptr ? record->ExceptionAddress : nullptr));
    }
    if (request.reason == nullptr && record != nullptr &&
        record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        record->NumberParameters >= 2) {
      out.text("\naccess       : ");
      out.text(record->ExceptionInformation[0] == 0 ? "read " : "write ");
      out.hex(record->ExceptionInformation[1]);
    }
    out.text("\nthread       : ");
    out.text(request.thread_name);
    out.nl();
    write_runtime(out);
    out.nl();
    out.text(s.preamble, s.preamble_length);
    if (s.minidump && g_write_dump != nullptr) {
      out.text("minidump     : ");
      out.text(s.dump_path);
      out.nl();
    }
    CONTEXT *const context =
        request.pointers != nullptr ? request.pointers->ContextRecord : nullptr;
    out.flush();
    write_registers(out, context);
    write_stack(out, context);
    write_notes(out, true);
    write_tail(out, true);
  }
  close_report(handle);
}

DWORD WINAPI report_on_worker(LPVOID const arg) {
  write_report(*static_cast<const Request *>(arg));
  return 0;
}

LONG WINAPI handle_exception(EXCEPTION_POINTERS *const pointers) {
  State &s = state();
  bool expected = false;
  if (!s.handling.compare_exchange_strong(expected, true))
    return EXCEPTION_CONTINUE_SEARCH;

  static Request request;
  request.pointers = pointers;
  request.thread_id = ::GetCurrentThreadId();
  request.thread_name = thread_label();

  const HANDLE worker = ::CreateThread(nullptr, 512 * 1024, &report_on_worker,
                                       &request, 0, nullptr);
  if (worker != nullptr) {
    ::WaitForSingleObject(worker, 60000);
    ::CloseHandle(worker);
  } else {
    write_report(request);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void report_and_exit(const char *const reason) {
  State &s = state();
  bool expected = false;
  if (s.handling.compare_exchange_strong(expected, true)) {
    static CONTEXT context;
    static EXCEPTION_RECORD record;
    static EXCEPTION_POINTERS pointers;
    ::RtlCaptureContext(&context);
    pointers.ContextRecord = &context;
    pointers.ExceptionRecord = &record;

    static Request request;
    request.pointers = &pointers;
    request.thread_id = ::GetCurrentThreadId();
    request.thread_name = thread_label();
    request.reason = reason;
    write_report(request);
  }
  ::_exit(3);
}

void handle_abort(int) { report_and_exit("abort()"); }
void handle_terminate() { report_and_exit("std::terminate()"); }
void handle_purecall() { report_and_exit("pure virtual call"); }
void handle_invalid_parameter(const wchar_t *, const wchar_t *, const wchar_t *,
                              unsigned int, uintptr_t) {
  report_and_exit("invalid CRT parameter");
}

void install_handlers() {
  const HMODULE dbghelp = ::LoadLibraryA("dbghelp.dll");
  if (dbghelp != nullptr) {
    g_write_dump =
        reinterpret_cast<MiniDumpWriteDumpFn>(reinterpret_cast<void *>(
            ::GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    // Loaded now rather than in the handler: LoadLibrary takes the loader lock,
    // and a crash that happens to hold it would hang instead of reporting.
    ::SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    ::SymInitialize(::GetCurrentProcess(), nullptr, TRUE);
  }

  g_previous_filter = ::SetUnhandledExceptionFilter(&handle_exception);
  ::SetErrorMode(::GetErrorMode() | SEM_FAILCRITICALERRORS |
                 SEM_NOGPFAULTERRORBOX);
  ::signal(SIGABRT, &handle_abort);
  std::set_terminate(&handle_terminate);
  ::_set_purecall_handler(&handle_purecall);
  ::_set_invalid_parameter_handler(&handle_invalid_parameter);
}

void remove_handlers() {
  ::SetUnhandledExceptionFilter(g_previous_filter);
  ::signal(SIGABRT, SIG_DFL);
}

#else

void install_handlers() {}
void remove_handlers() {}

#endif

usize stamp_now(char *const out, const usize capacity) {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm parts = {};
#if NX_HAS_WINDOWS_API
  ::localtime_s(&parts, &time);
#else
  ::localtime_r(&time, &parts);
#endif
  char text[64];
  return copy_into(out, capacity,
                   nx::format_to(text, "{:04}{:02}{:02}-{:02}{:02}{:02}",
                                 parts.tm_year + 1900, parts.tm_mon + 1,
                                 parts.tm_mday, parts.tm_hour, parts.tm_min,
                                 parts.tm_sec));
}

void prune(const string_view where, const u32 keep) {
  const auto listed = fs::directory_entries(where);
  if (!listed)
    return;
  vector<string> reports;
  for (const fs::directory_entry &entry : listed.value())
    if (entry.path.ends_with(".txt") || entry.path.ends_with(".dmp"))
      reports.push_back(entry.path);
  if (reports.size() <= keep)
    return;

  std::sort(reports.begin(), reports.end());
  const usize drop = reports.size() - keep;
  for (usize i = 0; i < drop; ++i) {
    if (reports[i].ends_with(".txt")) {
      string attachment_prefix(
          reports[i].view().substr(0, reports[i].size() - 4));
      attachment_prefix.push_back('-');
      for (const fs::directory_entry &entry : listed.value())
        if (entry.path.starts_with(attachment_prefix))
          (void)fs::remove(entry.path);
    }
    (void)fs::remove(reports[i]);
  }
}

} // namespace

bool install(const Config &config) {
  State &s = state();
  if (s.installed.load(std::memory_order_acquire))
    return true;

  string where(config.directory);
  if (where.empty()) {
    const auto base = fs::executable_dir();
    if (!base)
      return false;
    where = fs::path::join(base.value(), "crashes");
  }
  if (!fs::create_directories(where))
    return false;

  s.pid = os::process_id();
  s.started_ns = os::ticks_ns();
  s.started_wall_ns =
      nx::cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
  s.minidump = config.minidump;
  s.minidump_full = config.minidump_full_memory;

  s.directory_length = copy_into(s.directory, PATH_CAPACITY, where);
  prune(where, config.keep);

  char stamp[64] = {};
  stamp_now(stamp, sizeof(stamp));
  char name[256];
  copy_into(s.report_path, PATH_CAPACITY,
            fs::path::join(where, nx::format_to(name, "crash-{}-{}.txt",
                                                string_view(stamp), s.pid)));
  copy_into(s.dump_path, PATH_CAPACITY,
            fs::path::join(where, nx::format_to(name, "crash-{}-{}.dmp",
                                                string_view(stamp), s.pid)));

  gather_preamble(config);

  if (config.log_tail) {
    s.tail_enabled.store(true, std::memory_order_release);
    s.tail_sink = nx::allocate<TailSink>();
    if (s.tail_sink != nullptr)
      log::add_sink(s.tail_sink);
  }

  install_handlers();
  s.installed.store(true, std::memory_order_release);
  nx::logi(nx::log::system, "crash: reports go to {}", where);
  return true;
}

void shutdown() {
  State &s = state();
  if (!s.installed.exchange(false, std::memory_order_acq_rel))
    return;
  remove_handlers();
  if (s.tail_sink != nullptr) {
    log::remove_sink(s.tail_sink);
    s.tail_sink = nullptr;
  }
  s.tail_enabled.store(false, std::memory_order_release);
}

bool installed() noexcept {
  return state().installed.load(std::memory_order_acquire);
}

string_view directory() noexcept {
  const State &s = state();
  return string_view(s.directory, s.directory_length);
}

void note(const string_view key, const string_view value) {
  State &s = state();
  const scoped_lock lock(s.notes_mutex);
  Note *free_slot = nullptr;
  for (Note &slot : s.notes) {
    if (!slot.occupied) {
      if (free_slot == nullptr)
        free_slot = &slot;
      continue;
    }
    if (string_view(slot.key) == key) {
      slot.length =
          nx::cast<u16>(copy_into(slot.value, NOTE_VALUE_CAPACITY, value));
      return;
    }
  }
  if (free_slot == nullptr)
    return;
  copy_into(free_slot->key, NOTE_KEY_CAPACITY, key);
  free_slot->length =
      nx::cast<u16>(copy_into(free_slot->value, NOTE_VALUE_CAPACITY, value));
  free_slot->occupied = true;
}

void clear_note(const string_view key) {
  State &s = state();
  const scoped_lock lock(s.notes_mutex);
  for (Note &slot : s.notes)
    if (slot.occupied && string_view(slot.key) == key) {
      slot.occupied = false;
      return;
    }
}

bool valid_attachment_name(const string_view name) noexcept {
  if (name.empty() || name.size() >= ATTACHMENT_NAME_CAPACITY || name == "." ||
      name == "..")
    return false;
  for (const char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
      return false;
  return true;
}

bool report(const Fatal &fatal) {
  State &s = state();
  if (s.directory_length == 0)
    return false;
  // The log tail only holds records a sink has seen.
  log::flush();

  char stamp[64] = {};
  stamp_now(stamp, sizeof(stamp));
  char name[256];
  const string path = fs::path::join(
      directory(),
      nx::format_to(name, "{}-{}-{}-{}.txt", fatal.kind, string_view(stamp),
                    s.pid, s.fatal_count.fetch_add(1) + 1));

  const Out::handle_type handle = open_report(path.c_str());
  if (handle == Out::no_handle())
    return false;
  {
    Out out(handle);
    out.text("=== nx2d fatal report ===\n\n");
    out.line("reason       : ", fatal.reason);
    out.text("thread       : ");
    out.text(thread_label());
    out.nl();
    write_runtime(out);
    out.nl();
    out.text(s.preamble, s.preamble_length);
    out.flush();
    if (!fatal.detail.empty()) {
      out.text("\n--- detail ---\n");
      out.text(fatal.detail);
      out.nl();
    }
    if (!fatal.attachments.empty()) {
      out.text("\n--- attachments ---\n");
      const string_view report_stem =
          path.ends_with(".txt") ? path.view().substr(0, path.size() - 4)
                                 : path.view();
      for (const Attachment &attachment : fatal.attachments) {
        if (!valid_attachment_name(attachment.name)) {
          out.text("rejected invalid attachment name\n");
          continue;
        }
        if (attachment.bytes.size() > ATTACHMENT_SIZE_CAPACITY) {
          out.text(attachment.name);
          out.text(" : rejected (size limit)\n");
          continue;
        }
        nx::string attachment_path(report_stem);
        attachment_path.push_back('-');
        attachment_path.append(attachment.name);
        const auto wrote =
            fs::file_write_atomic(attachment_path, attachment.bytes);
        out.text(attachment.name);
        out.text(wrote ? " : " : " : write failed: ");
        out.text(wrote ? attachment_path.view()
                       : fs::to_string(wrote.error().kind));
        out.nl();
      }
    }
    write_notes(out);
    write_tail(out);
  }
  close_report(handle);

  copy_into(s.last_report, PATH_CAPACITY, path);
  nx::loge(nx::log::system, "crash: wrote {}", path);
  return true;
}

string_view last_report() noexcept { return string_view(state().last_report); }

vector<string> pending_reports() {
  vector<string> found;
  if (state().directory_length == 0)
    return found;
  const auto listed = fs::directory_entries(directory());
  if (!listed)
    return found;
  for (const fs::directory_entry &entry : listed.value())
    if (entry.path.ends_with(".txt"))
      found.push_back(entry.path);
  std::sort(found.begin(), found.end(),
            [](const string &a, const string &b) { return a > b; });
  return found;
}

} // namespace nx::crash
