#include "core/foundation/platform/socket.h"

#include "core/foundation/diagnostics/log.h"

#if NX_HAS_WINDOWS_API
#include <winsock2.h>
// Do not reorder!
#include <ws2tcpip.h>
#elif NX_PLATFORM_POSIX
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace nx::net {
namespace {

constexpr u64 NONE = ~u64{0};

#if NX_HAS_WINDOWS_API

using native = SOCKET;

[[nodiscard]] native cast(const u64 handle) {
  return static_cast<native>(handle);
}
[[nodiscard]] u64 widen(const native s) { return static_cast<u64>(s); }
[[nodiscard]] bool bad(const native s) { return s == INVALID_SOCKET; }

/// Winsock needs starting once per process. A function-local static does it on
/// the first listen and unwinds at exit; nothing here runs before main.
struct Winsock {
  bool ok = false;
  Winsock() {
    WSADATA data;
    ok = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }
  ~Winsock() {
    if (ok)
      ::WSACleanup();
  }
};

[[nodiscard]] bool started() {
  static const Winsock instance;
  return instance.ok;
}

void shut(const u64 handle) {
  if (handle != NONE)
    ::closesocket(cast(handle));
}

[[nodiscard]] bool set_non_blocking(const native s) {
  u_long on = 1;
  return ::ioctlsocket(s, FIONBIO, &on) == 0;
}

/// True when the call failed only because there was nothing to do, which is
/// every idle frame and not an error.
[[nodiscard]] bool would_block() {
  const int e = ::WSAGetLastError();
  return e == WSAEWOULDBLOCK;
}

#elif NX_PLATFORM_POSIX

using native = int;

[[nodiscard]] native cast(const u64 handle) {
  return static_cast<native>(handle);
}
[[nodiscard]] u64 widen(const native s) {
  return static_cast<u64>(static_cast<unsigned>(s));
}
[[nodiscard]] bool bad(const native s) { return s < 0; }
[[nodiscard]] bool started() { return true; }

void shut(const u64 handle) {
  if (handle != NONE)
    ::close(cast(handle));
}

[[nodiscard]] bool set_non_blocking(const native s) {
  const int flags = ::fcntl(s, F_GETFL, 0);
  return flags >= 0 && ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}

[[nodiscard]] bool would_block() {
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

#else

using native = int;
[[nodiscard]] native cast(u64) { return -1; }
[[nodiscard]] u64 widen(native) { return NONE; }
[[nodiscard]] bool bad(native) { return true; }
[[nodiscard]] bool started() { return false; }
void shut(u64) {}
[[nodiscard]] bool set_non_blocking(native) { return false; }
[[nodiscard]] bool would_block() { return false; }

#endif

} // namespace

Connection::~Connection() { shut(m_handle); }

Connection::Connection(Connection &&other) noexcept : m_handle(other.m_handle) {
  other.m_handle = NONE;
}

Connection &Connection::operator=(Connection &&other) noexcept {
  if (this != &other) {
    shut(m_handle);
    m_handle = other.m_handle;
    other.m_handle = NONE;
  }
  return *this;
}

Connection Connection::connect(const u16 port) {
  if (!started())
    return {};

  const native s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (bad(s))
    return {};

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = ::htons(port);
  address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

  // Blocking for the connect, then not: a loopback connect completes or fails
  // at once, and asking the caller to spin on EINPROGRESS buys nothing.
  if (::connect(s, reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) != 0 ||
      !set_non_blocking(s)) {
    shut(widen(s));
    return {};
  }
  return Connection(widen(s));
}

bool Connection::open() const noexcept { return m_handle != NONE; }

void Connection::close() {
  shut(m_handle);
  m_handle = NONE;
}

isize Connection::read(const std::span<char> into) {
  if (m_handle == NONE || into.empty())
    return 0;
#if NX_HAS_WINDOWS_API
  const int got =
      ::recv(cast(m_handle), into.data(), static_cast<int>(into.size()), 0);
#elif NX_PLATFORM_POSIX
  const isize got = ::recv(cast(m_handle), into.data(), into.size(), 0);
#else
  const isize got = -1;
#endif
  if (got > 0)
    return static_cast<isize>(got);
  // Zero from recv is the peer closing, which is not "nothing waiting".
  if (got == 0)
    return -1;
  return would_block() ? 0 : -1;
}

isize Connection::write(const std::span<const char> from) {
  if (m_handle == NONE || from.empty())
    return 0;
#if NX_HAS_WINDOWS_API
  const int sent =
      ::send(cast(m_handle), from.data(), static_cast<int>(from.size()), 0);
#elif NX_PLATFORM_POSIX
  // MSG_NOSIGNAL: a write to a socket the peer has closed raises SIGPIPE
  // otherwise, and a debugger disconnecting would kill the game.
  const isize sent = ::send(cast(m_handle), from.data(), from.size(),
#if defined(MSG_NOSIGNAL)
                            MSG_NOSIGNAL
#else
                            0
#endif
  );
#else
  const isize sent = -1;
#endif
  if (sent >= 0)
    return static_cast<isize>(sent);
  return would_block() ? 0 : -1;
}

Listener::~Listener() { shut(m_handle); }

Listener::Listener(Listener &&other) noexcept
    : m_handle(other.m_handle), m_port(other.m_port) {
  other.m_handle = NONE;
  other.m_port = 0;
}

Listener &Listener::operator=(Listener &&other) noexcept {
  if (this != &other) {
    shut(m_handle);
    m_handle = other.m_handle;
    m_port = other.m_port;
    other.m_handle = NONE;
    other.m_port = 0;
  }
  return *this;
}

bool Listener::listening() const noexcept { return m_handle != NONE; }

void Listener::close() {
  shut(m_handle);
  m_handle = NONE;
  m_port = 0;
}

bool Listener::listen(const u16 port) {
  close();
  if (!started()) {
    nx::loge("net: no socket layer on this platform");
    return false;
  }

  const native s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (bad(s)) {
    nx::loge("net: could not open a socket");
    return false;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = ::htons(port);
  // Loopback, never INADDR_ANY. See the file comment: this port runs code.
  address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

  if (::bind(s, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0 ||
      ::listen(s, 1) != 0 || !set_non_blocking(s)) {
    nx::loge("net: could not listen on 127.0.0.1:{}", port);
    shut(widen(s));
    return false;
  }

  // What the OS actually gave us, which is the point of asking for port 0.
  sockaddr_in bound{};
#if NX_HAS_WINDOWS_API
  int size = sizeof(bound);
#else
  socklen_t size = sizeof(bound);
#endif
  m_port = ::getsockname(s, reinterpret_cast<sockaddr *>(&bound), &size) == 0
               ? ::ntohs(bound.sin_port)
               : port;
  m_handle = widen(s);
  return true;
}

Connection Listener::accept() {
  if (m_handle == NONE)
    return {};

  const native taken = ::accept(cast(m_handle), nullptr, nullptr);
  if (bad(taken))
    return {};
  if (!set_non_blocking(taken)) {
    shut(widen(taken));
    return {};
  }

  // A debugger's messages are small and latency matters more than packing
  // them, so Nagle is off.
  const int on = 1;
  (void)::setsockopt(taken, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char *>(&on), sizeof(on));
  return Connection(widen(taken));
}

} // namespace nx::net
