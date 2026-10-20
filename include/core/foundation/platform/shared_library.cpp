#include "core/foundation/platform/shared_library.h"

#include "core/foundation/strings/format.h"

#if NX_HAS_WINDOWS_API
#include "core/foundation/platform/windows_path.h"
#endif

#if NX_HAS_WINDOWS_API
#include <windows.h>
#elif NX_PLATFORM_POSIX
#include <dlfcn.h>
#endif

namespace nx::os {
namespace {

#if NX_HAS_WINDOWS_API
[[nodiscard]] string windows_error() {
  const DWORD code = ::GetLastError();
  char *text = nullptr;
  const DWORD length = ::FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<char *>(&text), 0, nullptr);
  string out = length != 0 && text != nullptr
                   ? string(text, length)
                   : nx::format("error {}", cast<u32>(code));
  if (text != nullptr)
    ::LocalFree(text);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
    out.pop_back();
  return out;
}
#endif

}

shared_library::~shared_library() { close(); }

shared_library::shared_library(shared_library &&other) noexcept
    : m_handle(other.m_handle) {
  other.m_handle = nullptr;
}

shared_library &shared_library::operator=(shared_library &&other) noexcept {
  if (this != &other) {
    close();
    m_handle = other.m_handle;
    other.m_handle = nullptr;
  }
  return *this;
}

bool shared_library::supported() noexcept {
#if NX_PLATFORM_IOS
  return false;
#elif NX_HAS_WINDOWS_API || NX_PLATFORM_POSIX
  return true;
#else
  return false;
#endif
}

string shared_library::decorate(const string_view stem) {
#if NX_HAS_WINDOWS_API
  return nx::format("{}.dll", stem);
#elif defined(NX_PLATFORM_MACOS) || defined(NX_PLATFORM_IOS)
  return nx::format("lib{}.dylib", stem);
#else
  return nx::format("lib{}.so", stem);
#endif
}

result<void, string> shared_library::open(const string_view path) {
  // For the unload, not for the handle: the load below overwrites that either
  // way. Without this, opening twice leaves the first library loaded forever.
  close();

  const string terminated(path);
  string error;

#if NX_HAS_WINDOWS_API
  const vector<wchar_t> wide = fs::detail::win32_path(terminated.view());
  m_handle = wide.empty() ? nullptr : ::LoadLibraryW(wide.data());
  if (m_handle == nullptr)
    error = windows_error();
#elif NX_PLATFORM_POSIX
  ::dlerror();
  m_handle = ::dlopen(terminated.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (m_handle == nullptr)
    if (const char *const why = ::dlerror())
      error = why;
#else
  error = "this platform does not load code at run time";
#endif

  if (m_handle != nullptr)
    return {};
  if (error.empty())
    error = nx::format("could not load '{}'", path);
  return err(std::move(error));
}

void shared_library::close() noexcept {
  if (m_handle == nullptr)
    return;
#if NX_HAS_WINDOWS_API
  ::FreeLibrary(static_cast<HMODULE>(m_handle));
#elif NX_PLATFORM_POSIX
  ::dlclose(m_handle);
#endif
  m_handle = nullptr;
}

void *shared_library::symbol(const string_view name) const noexcept {
  if (m_handle == nullptr)
    return nullptr;
  const string terminated(name);
#if NX_HAS_WINDOWS_API
  return reinterpret_cast<void *>(
      ::GetProcAddress(static_cast<HMODULE>(m_handle), terminated.c_str()));
#elif NX_PLATFORM_POSIX
  return ::dlsym(m_handle, terminated.c_str());
#else
  (void)terminated;
  return nullptr;
#endif
}

}
