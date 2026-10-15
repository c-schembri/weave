#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "async_echo_peer.hpp"

using asio::ip::tcp;

TEST_CASE("Async peer rejects zero workers and stops without clients")
{
  support::AsyncEchoPeer invalid(0);
  CHECK(invalid.error() == asio::error::invalid_argument);
  support::AsyncEchoPeer peer(4);
  REQUIRE_FALSE(peer.error());
  peer.stop();
  peer.stop();
  CHECK_FALSE(peer.error());
  CHECK(peer.wait_idle());
}

TEST_CASE("Async peer preserves fragmented payloads on persistent connections and drains EOF")
{
  support::AsyncEchoPeer peer(4);
  REQUIRE_FALSE(peer.error());
  asio::io_context context;
  std::vector<tcp::socket> clients;
  asio::error_code error;
  for (std::size_t i = 0; i < 16; ++i) {
    clients.emplace_back(context);
    clients.back().connect(tcp::endpoint(asio::ip::address_v4::loopback(), peer.port(i)), error);
    REQUIRE_FALSE(error);
    clients.back().set_option(tcp::no_delay(true), error);
    REQUIRE_FALSE(error);
  }
  REQUIRE(peer.wait_connected(clients.size()));
  std::array<char, 12325> tx{}, rx{};
  for (int round = 0; round < 3; ++round) {
    for (std::size_t i = 0; i < clients.size(); ++i) {
      for (std::size_t j = 0; j < tx.size(); ++j)
        tx[j] = static_cast<char>((j + i + round) % 127);
      REQUIRE(asio::write(clients[i], asio::buffer(tx.data(), 7), error) == 7);
      REQUIRE_FALSE(error);
      REQUIRE(asio::write(clients[i], asio::buffer(tx.data() + 7, tx.size() - 7), error) == tx.size() - 7);
      REQUIRE_FALSE(error);
      REQUIRE(asio::read(clients[i], asio::buffer(rx), error) == rx.size());
      REQUIRE_FALSE(error);
      CHECK(tx == rx);
    }
  }
  for (auto &client : clients) {
    client.shutdown(tcp::socket::shutdown_send, error);
    REQUIRE_FALSE(error);
    CHECK(client.read_some(asio::buffer(rx), error) == 0);
    CHECK(error == asio::error::eof);
    client.close(error);
    CHECK_FALSE(error);
  }
  CHECK(peer.wait_idle());
  CHECK_FALSE(peer.error());
}

TEST_CASE("Async peer cancels pending accepts and reads before destroying sessions")
{
  support::AsyncEchoPeer peer(4);
  REQUIRE_FALSE(peer.error());
  asio::io_context context;
  std::vector<tcp::socket> clients;
  asio::error_code error;
  for (std::size_t i = 0; i < 16; ++i) {
    clients.emplace_back(context);
    clients.back().connect(tcp::endpoint(asio::ip::address_v4::loopback(), peer.port(i)), error);
    REQUIRE_FALSE(error);
  }
  REQUIRE(peer.wait_connected(clients.size()));
  peer.stop();
  CHECK(peer.wait_idle());
  CHECK_FALSE(peer.error());
  std::array<char, 1> rx{};
  for (auto &client : clients) {
    CHECK(client.read_some(asio::buffer(rx), error) == 0);
    // Forced cancellation is not a graceful half-close on Windows.
    CHECK((error == asio::error::eof || error == asio::error::connection_reset));
  }
}

TEST_CASE("Async peer reports an unexpected connection reset as a fixture error")
{
  support::AsyncEchoPeer peer(1);
  REQUIRE_FALSE(peer.error());
  asio::io_context context;
  tcp::socket client(context);
  asio::error_code error;
  client.connect(tcp::endpoint(asio::ip::address_v4::loopback(), peer.port(0)), error);
  REQUIRE_FALSE(error);
  REQUIRE(peer.wait_connected(1));
  client.set_option(asio::socket_base::linger(true, 0), error);
  REQUIRE_FALSE(error);
  client.close(error);
  REQUIRE_FALSE(error);
  CHECK_FALSE(peer.wait_idle());
  CHECK(peer.error() == asio::error::connection_reset);
}

TEST_CASE("Async peer supports repeated server-first teardown after completed exchanges")
{
  for (int repetition = 0; repetition < 12; ++repetition) {
    CAPTURE(repetition);
    support::AsyncEchoPeer peer(4);
    REQUIRE_FALSE(peer.error());
    asio::io_context context;
    std::vector<tcp::socket> clients;
    asio::error_code error;
    for (std::size_t i = 0; i < 32; ++i) {
      clients.emplace_back(context);
      clients.back().connect(tcp::endpoint(asio::ip::address_v4::loopback(), peer.port(i)), error);
      REQUIRE_FALSE(error);
    }
    REQUIRE(peer.wait_connected(clients.size()));
    std::array<char, 31> tx{}, rx{};
    for (std::size_t i = 0; i < clients.size(); ++i) {
      tx.fill(static_cast<char>(i + repetition));
      REQUIRE(asio::write(clients[i], asio::buffer(tx), error) == tx.size());
      REQUIRE_FALSE(error);
      REQUIRE(asio::read(clients[i], asio::buffer(rx), error) == rx.size());
      REQUIRE_FALSE(error);
      REQUIRE(tx == rx);
    }
    peer.stop();
    CHECK(peer.wait_idle());
    CHECK_FALSE(peer.error());
    for (auto &client : clients) {
      client.close(error);
      CHECK_FALSE(error);
    }
  }
}
