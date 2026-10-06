#include <doctest/doctest.h>
#include <weave/runtime.hpp>
#include <weave/tls.hpp>
#include <weave/timer.hpp>
#include "tls_certificates.hpp"
#include "runtime_fixture.hpp"

using namespace std::chrono_literals;

static weave::Task<void> echo(weave::TcpStream transport, const weave::TlsContext &credentials)
{
  auto peer = co_await weave::tls::server(std::move(transport), credentials);
  std::array<std::byte, 4096> buffer;

  while (auto received = co_await peer.read(buffer))
    co_await peer.write_all(std::span{buffer}.first(received));

  co_await peer.shutdown();
}

static weave::Task<void> clients(weave::u16 port, const weave::TlsContext &credentials)
{
  auto peer = co_await weave::tls::connect(credentials, "localhost", port);
  std::vector<std::byte> sent(1024 * 1024, std::byte{37});
  std::vector<std::byte> received(sent.size());

  co_await weave::scope([&](weave::TaskScope &children) -> weave::Task<void> {
    auto reading = children.spawn(peer.read_exactly(received));
    if (!reading)
      co_await weave::fail(reading.error());

    auto writing = children.spawn(peer.write_all(sent));
    if (!writing)
      co_await weave::fail(writing.error());

    co_await children.join();
  });
  CHECK(received == sent);

  co_await peer.shutdown();
}

static weave::Task<void> exchange(const weave::TlsContext &server, const weave::TlsContext &client)
{
  co_await weave::scope([&](weave::TaskScope &children) -> weave::Task<void> {
    auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
    const auto port = listener.local_port();

    for (int i = 0; i < 8; ++i) {
      auto job = children.spawn(clients(port, client));
      if (!job)
        co_await weave::fail(job.error());
    }

    for (int i = 0; i < 8; ++i) {
      auto transport = co_await listener.accept({.no_delay = true});
      auto job = children.spawn(echo(std::move(transport), server));
      if (!job)
        co_await weave::fail(job.error());
    }

    co_await children.join();
  });
}

TEST_CASE("TLS duplex streams drain on all four-worker scheduler and IO layout combinations")
{
  fixture::Certificates certificates;
  auto server = weave::TlsContext::server(
    {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key});
  auto client = weave::TlsContext::client({.ca_file = certificates.ca});
  REQUIRE(server);
  REQUIRE(client);

  constexpr auto layouts = support::io_layouts;
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};

  for (auto layout : layouts) {
    for (auto scheduler : schedulers) {
      INFO("layout=", static_cast<int>(layout), ", scheduler=", static_cast<int>(scheduler));
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
      REQUIRE(runtime);

      auto result = runtime->run(weave::timeout(20s, exchange(*server, *client)));
      const auto message = result ? std::string{} : result.error().message();
      REQUIRE_MESSAGE(result.has_value(), message);
    }
  }
}
