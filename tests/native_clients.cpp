#include <weave/tcp.hpp>
#include <doctest/doctest.h>
#include "asio_config.hpp"
#include "async_echo_peer.hpp"
#include "callback_pool.hpp"
#include "echo_peer.hpp"
#include <atomic>

namespace test_native_clients {

using support::LibuvClient;
using support::UsocketsClient;

struct Outcome {
  support::CallbackLoop &loop;
  bool ok = false;
  int calls = 0;

  support::Callback callback()
  {
    return {this, [](void *data, bool ok) {
              auto &outcome = *static_cast<Outcome *>(data);
              outcome.ok = ok;
              ++outcome.calls;
              outcome.loop.stop();
            }};
  }
};

template <class Client>
static Outcome exchange(support::CallbackGroup<Client> &group, std::size_t rounds, bool cancel = false)
{
  Outcome outcome{group.loop};

  struct Start {
    Client &client;
    Outcome &outcome;
    std::size_t rounds;
    bool cancel;
  } start{*group.clients[0], outcome, rounds, cancel};

  support::CallbackLoop::Command command{&start, [](void *data) {
                                           auto &start = *static_cast<Start *>(data);
                                           start.client.exchange(start.rounds, true, start.outcome.callback());
                                           if (start.cancel)
                                             start.client.close();
                                         }};
  group.loop.post(command);
  group.loop.run();
  return outcome;
}

// A peer that either ends without an echo or sends a deliberately wrong echo.
class FaultPeer {
  asio::io_context context_;
  asio::ip::tcp::acceptor acceptor_{context_};
  std::thread thread_;
  std::uint16_t port_ = 0;

public:
  explicit FaultPeer(bool corrupt)
  {
    asio::error_code error;
    acceptor_.open(asio::ip::tcp::v4(), error);
    support::check(!error);
    acceptor_.bind({asio::ip::address_v4::loopback(), 0}, error);
    support::check(!error);
    acceptor_.listen(1, error);
    support::check(!error);
    port_ = acceptor_.local_endpoint(error).port();
    support::check(!error);
    thread_ = std::thread([this, corrupt] {
      asio::ip::tcp::socket socket(context_);
      asio::error_code error;
      acceptor_.accept(socket, error);
      if (error)
        return;
      std::array<char, 1024> buffer{};
      asio::read(socket, asio::buffer(buffer), error);
      if (corrupt && !error) {
        buffer[0] ^= 1;
        asio::write(socket, asio::buffer(buffer), error);
      }
      socket.shutdown(asio::ip::tcp::socket::shutdown_send, error);
    });
  }

  ~FaultPeer()
  {
    asio::error_code ignored;
    acceptor_.close(ignored);
    thread_.join();
  }

  std::uint16_t port() const
  {
    return port_;
  }
};

TEST_CASE_TEMPLATE(
  "Native clients preserve fragmented and backpressured persistent transfers",
  Client,
  LibuvClient,
  UsocketsClient)
{
  for (std::size_t size : {std::size_t{1}, std::size_t{1024}, std::size_t{65536}, std::size_t{1 << 20}}) {
    CAPTURE(size);
    support::EchoPeer peer(997);
    support::CallbackGroup<Client> group(1, size);
    for (std::size_t i = 0; i < size; ++i)
      group.clients[0]->tx[i] = static_cast<std::byte>(i % 251);
    REQUIRE(group.connect(0, peer.port()));
    REQUIRE(group.clients[0]->send_buffer(1024));
    auto result = exchange(group, 3);
    CHECK(result.ok);
    CHECK(result.calls == 1);
    CHECK(group.clients[0]->tx == group.clients[0]->rx);
    auto empty = exchange(group, 0);
    CHECK(empty.ok);
    CHECK(empty.calls == 1);
    group.close();
    peer.join();
    CHECK(peer.ok());
    auto closed = exchange(group, 1);
    CHECK_FALSE(closed.ok);
    CHECK(closed.calls == 1);
  }
}

TEST_CASE_TEMPLATE(
  "Native clients report refusal, EOF and corrupt responses without hanging",
  Client,
  LibuvClient,
  UsocketsClient)
{
  {
    weave::Context reserve;
    auto listener = weave::tcp::listen(reserve, "127.0.0.1", 0);
    REQUIRE(listener);
    auto port = listener->local_port();
    REQUIRE(port);
    REQUIRE(listener->close());
    support::CallbackGroup<Client> group(1, 1024);
    CHECK_FALSE(group.connect(0, *port));
    CHECK(group.clients[0]->is_closed());
  }
  for (bool corrupt : {false, true}) {
    CAPTURE(corrupt);
    FaultPeer peer(corrupt);
    support::CallbackGroup<Client> group(1, 1024);
    REQUIRE(group.connect(0, peer.port()));
    auto result = exchange(group, 1);
    CHECK_FALSE(result.ok);
    CHECK(result.calls == 1);
    CHECK(group.clients[0]->is_closed());
  }
}

TEST_CASE_TEMPLATE(
  "Native clients drain pending connect and write cancellation exactly once",
  Client,
  LibuvClient,
  UsocketsClient)
{
  {
    support::EchoPeer peer;
    support::CallbackGroup<Client> group(1, 8 << 20);
    REQUIRE(group.connect(0, peer.port()));
    REQUIRE(group.clients[0]->send_buffer(1024));
    auto result = exchange(group, 3, true);
    CHECK_FALSE(result.ok);
    CHECK(result.calls == 1);
    CHECK(group.clients[0]->is_closed());
    group.close();
  }
  {
    support::AsyncEchoPeer peer(1);
    REQUIRE_FALSE(peer.error());
    support::CallbackGroup<Client> group(1, 1024);
    Outcome outcome{group.loop};

    struct Start {
      Client &client;
      Outcome &outcome;
      std::uint16_t port;
    } start{*group.clients[0], outcome, peer.port(0)};

    support::CallbackLoop::Command command{&start, [](void *data) {
                                             auto &start = *static_cast<Start *>(data);
                                             start.client.connect(start.port, start.outcome.callback());
                                             start.client.close();
                                           }};
    group.loop.post(command);
    group.loop.run();
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.calls == 1);
    CHECK(group.clients[0]->is_closed());
    peer.stop();
  }
}

TEST_CASE_TEMPLATE(
  "Native affine pools preserve many concurrent sessions and reusable submissions",
  Client,
  LibuvClient,
  UsocketsClient)
{
  support::AsyncEchoPeer peer(4);
  REQUIRE_FALSE(peer.error());
  support::CallbackPool<Client> pool(4, 32, 1024);
  for (std::size_t i = 0; i < 32; ++i) {
    pool.client(i).tx.assign(1024, static_cast<std::byte>(i));
    REQUIRE(pool.connect(i, peer.port(i)).get());
  }
  REQUIRE(peer.wait_connected(32));
  for (int batch = 0; batch < 12; ++batch) {
    std::vector<std::future<bool>> jobs;
    for (std::size_t i = 0; i < 32; ++i)
      jobs.push_back(pool.exchange(i, 8, true));
    for (auto &job : jobs)
      REQUIRE(job.get());
  }
  peer.stop();
  pool.stop();
  CHECK(peer.wait_idle());
  CHECK_FALSE(peer.error());
}

TEST_CASE_TEMPLATE(
  "Native pool shutdown drains active jobs before releasing client storage",
  Client,
  LibuvClient,
  UsocketsClient)
{
  support::AsyncEchoPeer peer(2);
  REQUIRE_FALSE(peer.error());
  support::CallbackPool<Client> pool(2, 4, 1 << 20);
  std::vector<std::future<bool>> jobs;
  for (std::size_t i = 0; i < 4; ++i)
    REQUIRE(pool.connect(i, peer.port(i)).get());
  REQUIRE(peer.wait_connected(4));
  for (std::size_t i = 0; i < 4; ++i)
    jobs.push_back(pool.exchange(i, 100, true));
  pool.stop();
  for (auto &job : jobs)
    CHECK_FALSE(job.get());
  peer.stop();
}

} // namespace test_native_clients
