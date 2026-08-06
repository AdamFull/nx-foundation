#pragma once

/**
 * @file socket.h
 * @brief A non-blocking TCP listener and connection (namespace nx::net).
 */

#include "core/foundation/core/foundation.h"

#include <span>

namespace nx::net {

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
  /// inside one process, which is what its suite does; nothing in the engine
  /// dials out on its own.
  [[nodiscard]] static Connection connect(u16 port);

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

  /// @p port 0 asks the OS for a free one, which port() then reports.
  [[nodiscard]] bool listen(u16 port);
  [[nodiscard]] bool listening() const noexcept;
  [[nodiscard]] u16 port() const noexcept { return m_port; }
  void close();

  /// A caller, or a closed Connection when nobody is waiting.
  [[nodiscard]] Connection accept();

private:
  u64 m_handle = ~u64{0};
  u16 m_port = 0;
};

} // namespace nx::net
