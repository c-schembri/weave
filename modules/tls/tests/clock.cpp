#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/tls.hpp>
#include "tls_certificates.hpp"
#include <array>
#include <chrono>
#include <ctime>
#include <optional>

static thread_local std::optional<std::time_t> coarse_time;

extern "C" std::time_t __real_time(std::time_t *output);

extern "C" std::time_t __wrap_time(std::time_t *output)
{
  if (!coarse_time)
    return __real_time(output);
  if (output)
    *output = *coarse_time;
  return *coarse_time;
}

static bool transfer(weave::detail::TlsEngine &from, weave::detail::TlsEngine &to)
{
  std::array<std::byte, 32768> buffer;
  for (;;) {
    auto count = from.output(buffer);
    if (!count)
      return false;
    if (*count == 0)
      return true;
    if (!to.input(std::span{buffer}.first(*count)))
      return false;
  }
}

static bool handshake(weave::detail::TlsEngine &client, weave::detail::TlsEngine &server)
{
  bool connected = false;
  bool accepted = false;
  for (unsigned step = 0; step < 100; ++step) {
    if (!connected) {
      auto result = client.handshake();
      if (result.action == weave::detail::TlsAction::failed)
        return false;
      connected = result.action == weave::detail::TlsAction::ready;
    }
    if (!transfer(client, server))
      return false;
    if (!accepted) {
      auto result = server.handshake();
      if (result.action == weave::detail::TlsAction::failed)
        return false;
      accepted = result.action == weave::detail::TlsAction::ready;
    }
    if (!transfer(server, client))
      return false;
    if (connected && accepted)
      return true;
  }
  return false;
}

TEST_CASE("TLS sessions use coherent realtime rather than a lagging coarse time() sample")
{
  fixture::Certificates certificates;
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
  const std::array modes{weave::TlsSessionMode::stateful, weave::TlsSessionMode::tickets};
  for (auto version : versions) {
    for (auto mode : modes) {
      CAPTURE(version);
      CAPTURE(mode);
      auto server_context = weave::TlsContext::server(
        {.certificate_file = certificates.leaf,
          .private_key_file = certificates.private_key,
          .min_version = version,
          .max_version = version,
          .sessions = {.mode = mode}});
      auto client_context = weave::TlsContext::client(
        {.ca_file = certificates.ca, .min_version = version, .max_version = version, .session_resumption = true});
      REQUIRE(server_context);
      REQUIRE(client_context);
      if (!server_context || !client_context)
        return;

      const auto before = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch())
                            .count();
      auto server = weave::detail::TlsEngine::create(*server_context, true, "");
      auto client = weave::detail::TlsEngine::create(*client_context, false, "localhost");
      REQUIRE(server);
      REQUIRE(client);
      if (!server || !client)
        return;
      REQUIRE(handshake(*client, *server));

      std::array byte{std::byte{1}};
      REQUIRE(server->write(byte).action == weave::detail::TlsAction::ready);
      REQUIRE(transfer(*server, *client));
      REQUIRE(client->read(byte).action == weave::detail::TlsAction::ready);

      struct Restore {
        ~Restore()
        {
          coarse_time.reset();
        }
      } restore;

      coarse_time = static_cast<std::time_t>(before - 1);
      CHECK(std::time(nullptr) == *coarse_time);
      auto session = client->session();
      REQUIRE(session);
      if (!session)
        return;
      CHECK(session->available());
      CHECK(session->remaining() > std::chrono::seconds{0});

      // Expiry checks must use the same coherent clock as initial capture.
      coarse_time = static_cast<std::time_t>(before + 86400);
      CHECK(session->available());
      CHECK(session->remaining() > std::chrono::seconds{0});
    }
  }
}
