#include "core/foundation/platform/socket.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/strings/format.h"

#include <charconv>
#include <cstring>

#if NX_HAS_WINDOWS_API
#include <winsock2.h>
// Do not reorder!
#include <ws2tcpip.h>
#elif NX_PLATFORM_POSIX
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
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

}

namespace {

[[nodiscard]] int af_of(const Family family) {
  return family == Family::V4 ? AF_INET : AF_INET6;
}

void fill_sockaddr(const Endpoint &e, sockaddr_storage &ss, socklen_t &len) {
  std::memset(&ss, 0, sizeof(ss));
  if (e.family() == Family::V4) {
    auto *a = reinterpret_cast<sockaddr_in *>(&ss);
    a->sin_family = AF_INET;
    a->sin_port = ::htons(e.port());
    std::memcpy(&a->sin_addr, e.address().data(), 4);
    len = static_cast<socklen_t>(sizeof(sockaddr_in));
  } else {
    auto *a = reinterpret_cast<sockaddr_in6 *>(&ss);
    a->sin6_family = AF_INET6;
    a->sin6_port = ::htons(e.port());
    std::memcpy(&a->sin6_addr, e.address().data(), 16);
    len = static_cast<socklen_t>(sizeof(sockaddr_in6));
  }
}

[[nodiscard]] Endpoint endpoint_from(const sockaddr *sa) {
  if (sa->sa_family == AF_INET) {
    const auto *a = reinterpret_cast<const sockaddr_in *>(sa);
    u8 b[4];
    std::memcpy(b, &a->sin_addr, 4);
    return Endpoint::v4(b, ::ntohs(a->sin_port));
  }
  if (sa->sa_family == AF_INET6) {
    const auto *a = reinterpret_cast<const sockaddr_in6 *>(sa);
    u8 b[16];
    std::memcpy(b, &a->sin6_addr, 16);
    return Endpoint::v6(b, ::ntohs(a->sin6_port));
  }
  return {};
}

[[nodiscard]] std::optional<u16> parse_port(const nx::string_view text) {
  if (text.empty())
    return std::nullopt;
  u32 value = 0;
  const char *first = text.data();
  const char *last = first + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc{} || ptr != last || value > 0xFFFFu)
    return std::nullopt;
  return static_cast<u16>(value);
}

}

Endpoint Endpoint::v4(const u8 (&addr)[4], const u16 port) noexcept {
  Endpoint e;
  std::memcpy(e.m_bytes, addr, 4);
  e.m_port = port;
  e.m_family = Family::V4;
  e.m_valid = true;
  return e;
}

Endpoint Endpoint::v6(const u8 (&addr)[16], const u16 port) noexcept {
  Endpoint e;
  std::memcpy(e.m_bytes, addr, 16);
  e.m_port = port;
  e.m_family = Family::V6;
  e.m_valid = true;
  return e;
}

Endpoint Endpoint::loopback(const Family family, const u16 port) noexcept {
  if (family == Family::V4) {
    const u8 addr[4] = {127, 0, 0, 1};
    return v4(addr, port);
  }
  u8 addr[16] = {};
  addr[15] = 1;
  return v6(addr, port);
}

Endpoint Endpoint::any(const Family family, const u16 port) noexcept {
  if (family == Family::V4) {
    const u8 addr[4] = {0, 0, 0, 0};
    return v4(addr, port);
  }
  const u8 addr[16] = {};
  return v6(addr, port);
}

std::span<const u8> Endpoint::address() const noexcept {
  if (!m_valid)
    return {};
  return {m_bytes, m_family == Family::V4 ? usize{4} : usize{16}};
}

std::optional<Endpoint> Endpoint::parse(const nx::string_view text) {
  if (text.empty())
    return std::nullopt;

  if (text.front() == '[') {
    const usize rb = text.find(']');
    if (rb == nx::string_view::npos)
      return std::nullopt;
    const nx::string_view host = text.substr(1, rb - 1);
    const nx::string_view rest = text.substr(rb + 1);
    if (rest.empty() || rest.front() != ':')
      return std::nullopt;
    const std::optional<u16> port = parse_port(rest.substr(1));
    if (!port)
      return std::nullopt;
    char buffer[64];
    if (host.size() >= sizeof(buffer))
      return std::nullopt;
    std::memcpy(buffer, host.data(), host.size());
    buffer[host.size()] = '\0';
    u8 addr[16];
    if (::inet_pton(AF_INET6, buffer, addr) != 1)
      return std::nullopt;
    return v6(addr, *port);
  }

  const usize colon = text.rfind(':');
  if (colon == nx::string_view::npos)
    return std::nullopt;
  const nx::string_view host = text.substr(0, colon);
  const std::optional<u16> port = parse_port(text.substr(colon + 1));
  if (!port)
    return std::nullopt;
  char buffer[64];
  if (host.size() >= sizeof(buffer))
    return std::nullopt;
  std::memcpy(buffer, host.data(), host.size());
  buffer[host.size()] = '\0';
  u8 addr[4];
  if (::inet_pton(AF_INET, buffer, addr) != 1)
    return std::nullopt;
  return v4(addr, *port);
}

nx::string Endpoint::to_string() const {
  if (!m_valid)
    return nx::string("[invalid]");
  char buffer[64] = {};
  ::inet_ntop(af_of(m_family), m_bytes, buffer, sizeof(buffer));
  return m_family == Family::V4 ? nx::format("{}:{}", buffer, m_port)
                                : nx::format("[{}]:{}", buffer, m_port);
}

bool operator==(const Endpoint &a, const Endpoint &b) noexcept {
  if (a.m_valid != b.m_valid)
    return false;
  if (!a.m_valid)
    return true;
  if (a.m_family != b.m_family || a.m_port != b.m_port)
    return false;
  const usize n = a.m_family == Family::V4 ? 4 : 16;
  return std::memcmp(a.m_bytes, b.m_bytes, n) == 0;
}

nx::result<nx::small_vector<Endpoint, 4>, ResolveError>
resolve(const nx::string_view host, const u16 port) {
  if (!started())
    return nx::err{ResolveError::Failed};
  if (host.empty())
    return nx::err{ResolveError::Invalid};

  char name[256];
  if (host.size() >= sizeof(name))
    return nx::err{ResolveError::Invalid};
  std::memcpy(name, host.data(), host.size());
  name[host.size()] = '\0';

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo *found = nullptr;
  const int rc = ::getaddrinfo(name, nullptr, &hints, &found);
  if (rc != 0) {
#ifdef EAI_NONAME
    if (rc == EAI_NONAME)
      return nx::err{ResolveError::NotFound};
#endif
#ifdef EAI_AGAIN
    if (rc == EAI_AGAIN)
      return nx::err{ResolveError::TryAgain};
#endif
    return nx::err{ResolveError::Failed};
  }

  nx::small_vector<Endpoint, 4> out;
  for (const addrinfo *it = found; it != nullptr; it = it->ai_next) {
    Endpoint e = endpoint_from(it->ai_addr);
    if (e.valid()) {
      e.set_port(port);
      out.push_back(e);
    }
  }
  ::freeaddrinfo(found);

  if (out.empty())
    return nx::err{ResolveError::NotFound};
  return out;
}

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

namespace {

// Sockets are never inherited: a child process that kept one alive held the
// port after the program that opened it had exited.
#if NX_HAS_WINDOWS_API
[[nodiscard]] native open_socket(const int af, const int type,
                                 const int protocol) {
  return ::WSASocketW(af, type, protocol, nullptr, 0,
                      WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}
[[nodiscard]] native accept_socket(const native listener) {
  const native s = ::accept(listener, nullptr, nullptr);
  if (!bad(s))
    ::SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
  return s;
}
#elif NX_PLATFORM_POSIX
[[nodiscard]] native open_socket(const int af, const int type,
                                 const int protocol) {
#if defined(SOCK_CLOEXEC)
  return ::socket(af, type | SOCK_CLOEXEC, protocol);
#else
  const native s = ::socket(af, type, protocol);
  if (!bad(s))
    ::fcntl(s, F_SETFD, FD_CLOEXEC);
  return s;
#endif
}
[[nodiscard]] native accept_socket(const native listener) {
#if defined(__linux__)
  return ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
#else
  const native s = ::accept(listener, nullptr, nullptr);
  if (!bad(s))
    ::fcntl(s, F_SETFD, FD_CLOEXEC);
  return s;
#endif
}
#else
[[nodiscard]] native open_socket(int, int, int) { return cast(NONE); }
[[nodiscard]] native accept_socket(native) { return cast(NONE); }
#endif

} // namespace

Connection Connection::connect(const u16 port) {
  if (!started())
    return {};

  const native s = open_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (bad(s))
    return {};

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = ::htons(port);
  address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

  if (::connect(s, reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) != 0 ||
      !set_non_blocking(s)) {
    shut(widen(s));
    return {};
  }
  return Connection(widen(s));
}

Connection Connection::connect(const Endpoint &to) {
  if (!started() || !to.valid())
    return {};

  const native s = open_socket(af_of(to.family()), SOCK_STREAM, IPPROTO_TCP);
  if (bad(s))
    return {};

  sockaddr_storage address;
  socklen_t size = 0;
  fill_sockaddr(to, address, size);

  if (::connect(s, reinterpret_cast<const sockaddr *>(&address), size) != 0 ||
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

  const native s = open_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (bad(s)) {
    nx::loge("net: could not open a socket");
    return false;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = ::htons(port);
  address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

  if (::bind(s, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0 ||
      ::listen(s, 1) != 0 || !set_non_blocking(s)) {
    nx::loge("net: could not listen on 127.0.0.1:{}", port);
    shut(widen(s));
    return false;
  }

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

bool Listener::listen(const Endpoint &at) {
  close();
  if (!started()) {
    nx::loge("net: no socket layer on this platform");
    return false;
  }
  if (!at.valid()) {
    nx::loge("net: cannot listen on an invalid address");
    return false;
  }

  const native s = open_socket(af_of(at.family()), SOCK_STREAM, IPPROTO_TCP);
  if (bad(s)) {
    nx::loge("net: could not open a socket");
    return false;
  }

  sockaddr_storage address;
  socklen_t size = 0;
  fill_sockaddr(at, address, size);

  if (::bind(s, reinterpret_cast<const sockaddr *>(&address), size) != 0 ||
      ::listen(s, 16) != 0 || !set_non_blocking(s)) {
    nx::loge("net: could not listen on {}", at.to_string().c_str());
    shut(widen(s));
    return false;
  }

  sockaddr_storage bound;
  socklen_t bound_size = sizeof(bound);
  m_port =
      ::getsockname(s, reinterpret_cast<sockaddr *>(&bound), &bound_size) == 0
          ? endpoint_from(reinterpret_cast<const sockaddr *>(&bound)).port()
          : at.port();
  m_handle = widen(s);
  return true;
}

Connection Listener::accept() {
  if (m_handle == NONE)
    return {};

  const native taken = accept_socket(cast(m_handle));
  if (bad(taken))
    return {};
  if (!set_non_blocking(taken)) {
    shut(widen(taken));
    return {};
  }

  const int on = 1;
  (void)::setsockopt(taken, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char *>(&on), sizeof(on));
  return Connection(widen(taken));
}

UdpSocket::~UdpSocket() { shut(m_handle); }

UdpSocket::UdpSocket(UdpSocket &&other) noexcept
    : m_handle(other.m_handle), m_local(other.m_local) {
  other.m_handle = NONE;
  other.m_local = {};
}

UdpSocket &UdpSocket::operator=(UdpSocket &&other) noexcept {
  if (this != &other) {
    shut(m_handle);
    m_handle = other.m_handle;
    m_local = other.m_local;
    other.m_handle = NONE;
    other.m_local = {};
  }
  return *this;
}

bool UdpSocket::open() const noexcept { return m_handle != NONE; }

void UdpSocket::close() {
  shut(m_handle);
  m_handle = NONE;
  m_local = {};
}

bool UdpSocket::bind(const Endpoint &at) {
  close();
  if (!started()) {
    nx::loge("net: no socket layer on this platform");
    return false;
  }
  if (!at.valid()) {
    nx::loge("net: cannot bind an invalid address");
    return false;
  }

  const native s = open_socket(af_of(at.family()), SOCK_DGRAM, IPPROTO_UDP);
  if (bad(s)) {
    nx::loge("net: could not open a udp socket");
    return false;
  }

  sockaddr_storage address;
  socklen_t size = 0;
  fill_sockaddr(at, address, size);

  if (::bind(s, reinterpret_cast<const sockaddr *>(&address), size) != 0 ||
      !set_non_blocking(s)) {
    nx::loge("net: could not bind {}", at.to_string().c_str());
    shut(widen(s));
    return false;
  }

  sockaddr_storage bound;
  socklen_t bound_size = sizeof(bound);
  m_local =
      ::getsockname(s, reinterpret_cast<sockaddr *>(&bound), &bound_size) == 0
          ? endpoint_from(reinterpret_cast<const sockaddr *>(&bound))
          : at;
  m_handle = widen(s);
  return true;
}

isize UdpSocket::send_to(const std::span<const char> from, const Endpoint &to) {
  if (m_handle == NONE || !to.valid())
    return -1;

  sockaddr_storage address;
  socklen_t size = 0;
  fill_sockaddr(to, address, size);

#if NX_HAS_WINDOWS_API
  const int sent =
      ::sendto(cast(m_handle), from.data(), static_cast<int>(from.size()), 0,
               reinterpret_cast<const sockaddr *>(&address), size);
#elif NX_PLATFORM_POSIX
  const isize sent = ::sendto(cast(m_handle), from.data(), from.size(),
#if defined(MSG_NOSIGNAL)
                              MSG_NOSIGNAL,
#else
                              0,
#endif
                              reinterpret_cast<const sockaddr *>(&address),
                              size);
#else
  const isize sent = -1;
#endif
  if (sent >= 0)
    return static_cast<isize>(sent);
  return would_block() ? 0 : -1;
}

isize UdpSocket::recv_from(const std::span<char> into, Endpoint &from) {
  if (m_handle == NONE)
    return 0;

  sockaddr_storage address;
  socklen_t size = sizeof(address);
#if NX_HAS_WINDOWS_API
  const int got =
      ::recvfrom(cast(m_handle), into.data(), static_cast<int>(into.size()), 0,
                 reinterpret_cast<sockaddr *>(&address), &size);
#elif NX_PLATFORM_POSIX
  const isize got =
      ::recvfrom(cast(m_handle), into.data(), into.size(), 0,
                 reinterpret_cast<sockaddr *>(&address), &size);
#else
  const isize got = -1;
#endif
  if (got >= 0) {
    from = endpoint_from(reinterpret_cast<const sockaddr *>(&address));
    return static_cast<isize>(got);
  }
  return would_block() ? 0 : -1;
}

}
