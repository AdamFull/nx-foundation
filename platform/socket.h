#pragma once

/**
 * @file socket.h
 * @brief Raw sockets: a TCP listener/connection and a UDP socket, plus the
 * address types they speak (namespace nx::net).
 */

#include "core/foundation/containers/small_vector.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/core/result.h"
#include "core/foundation/strings/utf8_string.h"

#include <optional>
#include <span>

namespace nx::net {

enum class Family : u8 { V4, V6 };

/// A resolved IP address and port. Plain data - no socket, no platform type -
/// so it copies freely and crosses threads. Address bytes are in network order
/// (1.2.3.4 is {1, 2, 3, 4}); the port is host order.
class Endpoint {
public:
  Endpoint() = default;

  [[nodiscard]] static Endpoint v4(const u8 (&addr)[4], u16 port) noexcept;
  [[nodiscard]] static Endpoint v6(const u8 (&addr)[16], u16 port) noexcept;
  /// 127.0.0.1 or ::1, the dial-yourself address.
  [[nodiscard]] static Endpoint loopback(Family family, u16 port) noexcept;
  /// 0.0.0.0 or ::, the "every interface" bind address. Not a dial target.
  [[nodiscard]] static Endpoint any(Family family, u16 port) noexcept;

  /// A numeric address and port: "1.2.3.4:80" or "[::1]:80". Not a resolver - a
  /// hostname yields nullopt, which is what resolve() is for. A bare IPv6
  /// address must be bracketed so its colons are not read as the port's.
  [[nodiscard]] static std::optional<Endpoint> parse(nx::string_view text);

  [[nodiscard]] bool valid() const noexcept { return m_valid; }
  [[nodiscard]] Family family() const noexcept { return m_family; }
  [[nodiscard]] u16 port() const noexcept { return m_port; }
  void set_port(const u16 port) noexcept { m_port = port; }
  /// 4 bytes for V4, 16 for V6; empty when invalid.
  [[nodiscard]] std::span<const u8> address() const noexcept;
  [[nodiscard]] nx::string to_string() const;

  friend bool operator==(const Endpoint &a, const Endpoint &b) noexcept;

private:
  u8 m_bytes[16] = {};
  u16 m_port = 0;
  Family m_family = Family::V4;
  bool m_valid = false;
};

enum class ResolveError : u8 {
  /// The host string is empty or otherwise malformed.
  Invalid,
  /// The name resolved to nothing.
  NotFound,
  /// A temporary failure; the same call may succeed on a retry.
  TryAgain,
  /// Anything else, including no socket layer on this platform.
  Failed,
};

/// Every address a host has, in the order the resolver returned them, each
/// carrying @p port. Blocking - it may hit the network - so a caller that
/// cannot block hands it to thread_pool::async. A numeric literal resolves
/// without touching the network.
[[nodiscard]] nx::result<nx::small_vector<Endpoint, 4>, ResolveError>
resolve(nx::string_view host, u16 port);

/// One accepted connection. Moves, never copies: the handle is owned.
class Connection {
public:
  Connection() = default;
  ~Connection();

  Connection(Connection &&other) noexcept;
  Connection &operator=(Connection &&other) noexcept;
  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;

  /// Dials a loopback listener. Present so a listener can be driven from
  /// inside one process, which is what its suite does.
  [[nodiscard]] static Connection connect(u16 port);

  /// Dials a remote endpoint. Blocking for the connect itself - a caller that
  /// cannot wait for an unreachable host hands it to thread_pool::async - then
  /// non-blocking for reads and writes like every other connection here.
  [[nodiscard]] static Connection connect(const Endpoint &to);

  [[nodiscard]] bool open() const noexcept;
  void close();

  /// Bytes read; 0 when nothing was waiting, -1 once the peer has gone. The
  /// difference matters: nothing waiting is the normal case every frame.
  [[nodiscard]] isize read(std::span<char> into);
  /// Bytes written, which may be fewer than offered. -1 once the peer has
  /// gone.
  [[nodiscard]] isize write(std::span<const char> from);

private:
  friend class Listener;
  explicit Connection(u64 handle) noexcept : m_handle(handle) {}

  /// The platform's socket, widened. ~0 is "none" rather than 0, because a
  /// POSIX descriptor of 0 is valid.
  u64 m_handle = ~u64{0};
};

/// A listening socket on loopback.
class Listener {
public:
  Listener() = default;
  ~Listener();

  Listener(Listener &&other) noexcept;
  Listener &operator=(Listener &&other) noexcept;
  Listener(const Listener &) = delete;
  Listener &operator=(const Listener &) = delete;

  /// @p port 0 asks the OS for a free one, which port() then reports. Loopback
  /// only, never a public interface - the safe default. Use the Endpoint
  /// overload to bind anywhere else.
  [[nodiscard]] bool listen(u16 port);
  /// Binds @p at, which may be any() to accept on every interface, or a v6
  /// address. Same port(0)-picks-one rule.
  [[nodiscard]] bool listen(const Endpoint &at);
  [[nodiscard]] bool listening() const noexcept;
  [[nodiscard]] u16 port() const noexcept { return m_port; }
  void close();

  /// A caller, or a closed Connection when nobody is waiting.
  [[nodiscard]] Connection accept();

private:
  u64 m_handle = ~u64{0};
  u16 m_port = 0;
};

/// A bound, non-blocking UDP socket. Moves, never copies. Datagrams, so there
/// is no connection and no stream: each send is one packet to one endpoint and
/// each receive tells you who sent it.
class UdpSocket {
public:
  UdpSocket() = default;
  ~UdpSocket();

  UdpSocket(UdpSocket &&other) noexcept;
  UdpSocket &operator=(UdpSocket &&other) noexcept;
  UdpSocket(const UdpSocket &) = delete;
  UdpSocket &operator=(const UdpSocket &) = delete;

  /// Binds a local address. any(family, 0) takes every interface and an
  /// OS-assigned port, which local() then reports.
  [[nodiscard]] bool bind(const Endpoint &at);
  [[nodiscard]] bool open() const noexcept;
  void close();

  /// The address actually bound, with the port filled in. Invalid before bind.
  [[nodiscard]] const Endpoint &local() const noexcept { return m_local; }

  /// Bytes sent, or -1 on error. A datagram is all-or-nothing: unlike a stream
  /// write there is no short send to loop on.
  [[nodiscard]] isize send_to(std::span<const char> from, const Endpoint &to);
  /// Bytes read, and in @p from who sent them. 0 when nothing was waiting - the
  /// normal idle case - and -1 on error. A zero-length datagram is a real 0,
  /// not a closed peer: UDP has no close.
  [[nodiscard]] isize recv_from(std::span<char> into, Endpoint &from);

private:
  u64 m_handle = ~u64{0};
  Endpoint m_local;
};

} // namespace nx::net
