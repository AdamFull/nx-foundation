#pragma once

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
  [[nodiscard]] static Endpoint loopback(Family family, u16 port) noexcept;
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
  NotFound,
  TryAgain,
  Failed,
};

/// Blocking - it may hit the network - so a caller that cannot block hands it to
/// thread_pool::async.
[[nodiscard]] nx::result<nx::small_vector<Endpoint, 4>, ResolveError>
resolve(nx::string_view host, u16 port);

class Connection {
public:
  Connection() = default;
  ~Connection();

  Connection(Connection &&other) noexcept;
  Connection &operator=(Connection &&other) noexcept;
  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;

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

  u64 m_handle = ~u64{0};
};

class Listener {
public:
  Listener() = default;
  ~Listener();

  Listener(Listener &&other) noexcept;
  Listener &operator=(Listener &&other) noexcept;
  Listener(const Listener &) = delete;
  Listener &operator=(const Listener &) = delete;

  [[nodiscard]] bool listen(u16 port);
  [[nodiscard]] bool listen(const Endpoint &at);
  [[nodiscard]] bool listening() const noexcept;
  [[nodiscard]] u16 port() const noexcept { return m_port; }
  void close();

  [[nodiscard]] Connection accept();

private:
  u64 m_handle = ~u64{0};
  u16 m_port = 0;
};

class UdpSocket {
public:
  UdpSocket() = default;
  ~UdpSocket();

  UdpSocket(UdpSocket &&other) noexcept;
  UdpSocket &operator=(UdpSocket &&other) noexcept;
  UdpSocket(const UdpSocket &) = delete;
  UdpSocket &operator=(const UdpSocket &) = delete;

  [[nodiscard]] bool bind(const Endpoint &at);
  [[nodiscard]] bool open() const noexcept;
  void close();

  [[nodiscard]] const Endpoint &local() const noexcept { return m_local; }

  /// Bytes sent, or -1 on error. A datagram is all-or-nothing: unlike a stream
  /// write there is no short send to loop on.
  [[nodiscard]] isize send_to(std::span<const char> from, const Endpoint &to);
  /// Bytes read, and in @p from who sent them. 0 when nothing was waiting - the
  /// normal idle case - and -1 on error. A zero-length datagram is a real 0,
  /// not a closed peer: UDP has no close. A datagram longer than @p into is
  /// cut to fit on every platform, and a peer's closed port is not an error.
  [[nodiscard]] isize recv_from(std::span<char> into, Endpoint &from);

private:
  u64 m_handle = ~u64{0};
  Endpoint m_local;
};

}
