
#include "framework/nxtest.h"

#include "core/foundation/platform/socket.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/threading/thread.h"

#include <chrono>

namespace {

namespace net = nx::net;

[[nodiscard]] net::Connection accept_soon(net::Listener &listener) {
  for (u32 i = 0; i < 200; ++i) {
    net::Connection taken = listener.accept();
    if (taken.open())
      return taken;
    nx::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return {};
}

[[nodiscard]] nx::string read_soon(net::Connection &from, const usize count) {
  nx::string out;
  char buffer[256];
  for (u32 i = 0; i < 200 && out.size() < count; ++i) {
    const isize got = from.read({buffer, sizeof(buffer)});
    if (got < 0)
      break;
    if (got == 0) {
      nx::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    out.append(buffer, nx::cast<usize>(got));
  }
  return out;
}

}

TEST_CASE("net: a listener takes the port the OS gave it") {
  net::Listener listener;
  REQUIRE(listener.listen(0));
  CHECK(listener.listening());
  CHECK(listener.port() != 0u);

  listener.close();
  CHECK_FALSE(listener.listening());
  CHECK(listener.port() == 0u);
}

TEST_CASE("net: accept does not block when nobody is calling") {
  net::Listener listener;
  REQUIRE(listener.listen(0));

  for (u32 i = 0; i < 8; ++i)
    CHECK_FALSE(listener.accept().open());
}

TEST_CASE("net: bytes cross in both directions") {
  net::Listener listener;
  REQUIRE(listener.listen(0));

  net::Connection client = net::Connection::connect(listener.port());
  REQUIRE(client.open());
  net::Connection server = accept_soon(listener);
  REQUIRE(server.open());

  const nx::string_view up = "Content-Length: 2\r\n\r\n{}";
  CHECK(client.write({up.data(), up.size()}) == nx::cast<isize>(up.size()));
  CHECK(read_soon(server, up.size()) == up);

  const nx::string_view down = "pong";
  CHECK(server.write({down.data(), down.size()}) ==
        nx::cast<isize>(down.size()));
  CHECK(read_soon(client, down.size()) == down);
}

TEST_CASE("net: a read with nothing waiting is 0, and a dead peer is -1") {
  net::Listener listener;
  REQUIRE(listener.listen(0));

  net::Connection client = net::Connection::connect(listener.port());
  REQUIRE(client.open());
  net::Connection server = accept_soon(listener);
  REQUIRE(server.open());

  char buffer[16];
  CHECK(server.read({buffer, sizeof(buffer)}) == 0);

  client.close();

  isize last = 0;
  for (u32 i = 0; i < 200 && last == 0; ++i) {
    last = server.read({buffer, sizeof(buffer)});
    if (last == 0)
      nx::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(last == -1);
}

TEST_CASE("net: writing to a peer that has gone reports, and does not signal") {
  net::Listener listener;
  REQUIRE(listener.listen(0));

  net::Connection client = net::Connection::connect(listener.port());
  REQUIRE(client.open());
  net::Connection server = accept_soon(listener);
  REQUIRE(server.open());

  client.close();

  const nx::string_view text = "still here?";
  isize last = 0;
  for (u32 i = 0; i < 200; ++i) {
    last = server.write({text.data(), text.size()});
    if (last < 0)
      break;
    nx::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(last < 0);
}

TEST_CASE("net: a connection moves and the handle goes with it") {
  net::Listener listener;
  REQUIRE(listener.listen(0));

  net::Connection client = net::Connection::connect(listener.port());
  REQUIRE(client.open());
  net::Connection server = accept_soon(listener);
  REQUIRE(server.open());

  net::Connection moved = std::move(server);
  CHECK(moved.open());
  CHECK_FALSE(server.open()); // NOLINT(bugprone-use-after-move)

  const nx::string_view text = "hello";
  CHECK(moved.write({text.data(), text.size()}) ==
        nx::cast<isize>(text.size()));
  CHECK(read_soon(client, text.size()) == text);
}

TEST_CASE("net: connecting to a port nothing listens on fails") {
  net::Listener listener;
  REQUIRE(listener.listen(0));
  const u16 port = listener.port();
  listener.close();

  CHECK_FALSE(net::Connection::connect(port).open());
}

TEST_CASE("net: an endpoint parses a v4 literal and prints it back") {
  const std::optional<net::Endpoint> e = net::Endpoint::parse("127.0.0.1:8080");
  REQUIRE(e.has_value());
  CHECK(e->valid());
  CHECK(e->family() == net::Family::V4);
  CHECK(e->port() == 8080u);
  const nx::string text = e->to_string();
  CHECK(text.view() == "127.0.0.1:8080");
}

TEST_CASE("net: an endpoint parses a bracketed v6 literal and prints it back") {
  const std::optional<net::Endpoint> e = net::Endpoint::parse("[::1]:9000");
  REQUIRE(e.has_value());
  CHECK(e->family() == net::Family::V6);
  CHECK(e->port() == 9000u);
  const nx::string text = e->to_string();
  CHECK(text.view() == "[::1]:9000");
}

TEST_CASE("net: endpoint parse refuses what is not a numeric host and port") {
  CHECK_FALSE(net::Endpoint::parse("").has_value());
  CHECK_FALSE(net::Endpoint::parse("127.0.0.1").has_value());
  CHECK_FALSE(net::Endpoint::parse("127.0.0.1:70000").has_value());
  CHECK_FALSE(net::Endpoint::parse("127.0.0.1:-1").has_value());
  CHECK_FALSE(net::Endpoint::parse("127.0.0.1:8x").has_value());
  CHECK_FALSE(net::Endpoint::parse("localhost:80").has_value());
  CHECK_FALSE(net::Endpoint::parse("[::1]").has_value());
  CHECK(net::Endpoint::parse("0.0.0.0:0").has_value());
  CHECK(net::Endpoint::parse("255.255.255.255:65535").has_value());
}

TEST_CASE("net: loopback and any endpoints hold the addresses they claim") {
  const net::Endpoint lo = net::Endpoint::loopback(net::Family::V4, 53);
  CHECK(lo.valid());
  CHECK(lo.port() == 53u);
  const nx::string lo_text = lo.to_string();
  CHECK(lo_text.view() == "127.0.0.1:53");

  const net::Endpoint any = net::Endpoint::any(net::Family::V4, 0);
  const nx::string any_text = any.to_string();
  CHECK(any_text.view() == "0.0.0.0:0");

  CHECK(lo == net::Endpoint::loopback(net::Family::V4, 53));
  CHECK_FALSE(lo == net::Endpoint::loopback(net::Family::V4, 54));
  CHECK_FALSE(lo == any);
}

TEST_CASE("net: resolve turns a numeric address into an endpoint with the port") {
  nx::result<nx::small_vector<net::Endpoint, 4>, net::ResolveError> r =
      net::resolve("127.0.0.1", 443);
  REQUIRE(r.has_value());
  REQUIRE_FALSE(r->empty());

  bool found_v4 = false;
  for (const net::Endpoint &e : *r) {
    CHECK(e.port() == 443u);
    if (e.family() == net::Family::V4)
      found_v4 = true;
  }
  CHECK(found_v4);
}

TEST_CASE("net: resolve refuses an empty host") {
  nx::result<nx::small_vector<net::Endpoint, 4>, net::ResolveError> r =
      net::resolve("", 80);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == net::ResolveError::Invalid);
}

TEST_CASE("net: a listener bound by endpoint takes a connection dialled by one") {
  net::Listener listener;
  REQUIRE(listener.listen(net::Endpoint::loopback(net::Family::V4, 0)));
  const u16 port = listener.port();
  CHECK(port != 0u);

  net::Connection client =
      net::Connection::connect(net::Endpoint::loopback(net::Family::V4, port));
  REQUIRE(client.open());
  net::Connection server = accept_soon(listener);
  REQUIRE(server.open());

  const nx::string_view msg = "over ip";
  CHECK(client.write({msg.data(), msg.size()}) == nx::cast<isize>(msg.size()));
  CHECK(read_soon(server, msg.size()) == msg);
}

TEST_CASE("net: dialling an endpoint nothing listens on fails") {
  net::Listener listener;
  REQUIRE(listener.listen(net::Endpoint::loopback(net::Family::V4, 0)));
  const u16 port = listener.port();
  listener.close();

  CHECK_FALSE(
      net::Connection::connect(net::Endpoint::loopback(net::Family::V4, port))
          .open());
}

TEST_CASE("net: a udp datagram crosses and names its sender") {
  net::UdpSocket a;
  net::UdpSocket b;
  REQUIRE(a.bind(net::Endpoint::loopback(net::Family::V4, 0)));
  REQUIRE(b.bind(net::Endpoint::loopback(net::Family::V4, 0)));
  CHECK(a.local().port() != 0u);
  CHECK(b.local().port() != 0u);

  const nx::string_view payload = "ping";
  REQUIRE(a.send_to({payload.data(), payload.size()}, b.local()) ==
          nx::cast<isize>(payload.size()));

  char buffer[64];
  net::Endpoint from;
  isize got = 0;
  for (u32 i = 0; i < 200 && got == 0; ++i) {
    got = b.recv_from({buffer, sizeof(buffer)}, from);
    if (got == 0)
      nx::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(got == nx::cast<isize>(payload.size()));
  CHECK(nx::string_view(buffer, nx::cast<usize>(got)) == payload);
  CHECK(from == a.local());
}

TEST_CASE("net: a udp receive with nothing waiting is 0") {
  net::UdpSocket s;
  REQUIRE(s.bind(net::Endpoint::loopback(net::Family::V4, 0)));

  char buffer[16];
  net::Endpoint from;
  CHECK(s.recv_from({buffer, sizeof(buffer)}, from) == 0);
}
