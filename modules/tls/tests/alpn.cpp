#include <weave/tls.hpp>
#include <weave/tls/detail/engine.hpp>
#include "tls_certificates.hpp"
#include <atomic>
#include <source_location>
#include <cstdio>
#include <cstdlib>

static std::atomic<unsigned> checks{0};

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Required ALPN check failed: %s:%u\n", where.file_name(), where.line());
    std::exit(1);
  }
}

static bool transfer(weave::detail::TlsEngine &from, weave::detail::TlsEngine &to)
{
  std::array<std::byte, 32768> buffer;
  for (;;) {
    auto count = from.output(buffer);
    check(count.has_value());
    if (!*count)
      return true;
    check(to.input(std::span{buffer}.first(*count)).has_value());
  }
}

static bool handshake(weave::detail::TlsEngine &client, weave::detail::TlsEngine &server)
{
  bool connected = false;
  bool accepted = false;
  for (int i = 0; i < 100; ++i) {
    if (!connected) {
      auto step = client.handshake();
      if (step.action == weave::detail::TlsAction::failed)
        return false;
      connected = step.action == weave::detail::TlsAction::ready;
    }
    transfer(client, server);
    if (!accepted) {
      auto step = server.handshake();
      if (step.action == weave::detail::TlsAction::failed)
        return false;
      accepted = step.action == weave::detail::TlsAction::ready;
    }
    transfer(server, client);
    if (connected && accepted)
      return true;
  }
  return false;
}

static void engine_controls(fixture::Certificates &files)
{
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
  const std::array modes{weave::TlsSessionMode::stateful, weave::TlsSessionMode::tickets};
  const std::array protocols{std::string("postgresql"), std::string("p\0\xff", 3)};
  for (auto version : versions) {
    for (auto mode : modes) {
      for (const auto &protocol : protocols) {
        auto server = weave::TlsContext::server(
          {.certificate_file = files.leaf,
            .private_key_file = files.private_key,
            .alpn = {protocol},
            .min_version = version,
            .max_version = version,
            .sessions = {.mode = mode}});
        auto client = weave::TlsContext::client(
          {.ca_file = files.ca,
            .alpn = {"unrelated"},
            .min_version = version,
            .max_version = version,
            .session_resumption = true});
        check(server.has_value() && client.has_value());
        auto accepted = weave::detail::TlsEngine::create(*server, true, "", nullptr, protocol);
        auto connected = weave::detail::TlsEngine::create(*client, false, "localhost", nullptr, protocol);
        check(accepted.has_value() && connected.has_value());
        check(handshake(*connected, *accepted));
        check(connected->alpn() == protocol && accepted->alpn() == protocol);
        std::array<std::byte, 1> byte{std::byte{3}};
        check(accepted->write(byte).action == weave::detail::TlsAction::ready);
        transfer(*accepted, *connected);
        check(connected->read(byte).action == weave::detail::TlsAction::ready);
        auto session = connected->session();
        check(session.has_value());
        auto wrong = weave::detail::TlsEngine::create(*client, false, "localhost", &*session);
        check(!wrong && wrong.error() == weave::TlsError::session_rejected && session->available());
        auto resumed = weave::detail::TlsEngine::create(*client, false, "localhost", &*session, protocol);
        auto peer = weave::detail::TlsEngine::create(*server, true, "", nullptr, protocol);
        check(resumed.has_value() && peer.has_value());
        check(handshake(*resumed, *peer));
        check(resumed->session_reused() && peer->session_reused());
        check(resumed->alpn() == protocol);
      }
    }

    auto server = weave::TlsContext::server(
      {.certificate_file = files.leaf,
        .private_key_file = files.private_key,
        .min_version = version,
        .max_version = version});
    auto client = weave::TlsContext::client({.ca_file = files.ca, .min_version = version, .max_version = version});
    check(server.has_value() && client.has_value());
    auto accepted = weave::detail::TlsEngine::create(*server, true, "");
    auto connected = weave::detail::TlsEngine::create(*client, false, "localhost", nullptr, "postgresql");
    check(accepted.has_value() && connected.has_value());
    check(!handshake(*connected, *accepted));
    check(connected->handshake().error == weave::TlsError::protocol);
    check(!weave::detail::TlsEngine::create(*client, false, "localhost", nullptr, std::string(256, 'p')));

    auto unrelated = weave::TlsContext::server(
      {.certificate_file = files.leaf,
        .private_key_file = files.private_key,
        .alpn = {"unrelated"},
        .min_version = version,
        .max_version = version});
    auto shared = weave::TlsContext::client(
      {.ca_file = files.ca, .alpn = {"unrelated"}, .min_version = version, .max_version = version});
    check(unrelated.has_value() && shared.has_value());
    auto rejected_server = weave::detail::TlsEngine::create(*unrelated, true, "", nullptr, "postgresql");
    auto ordinary_client = weave::detail::TlsEngine::create(*shared, false, "localhost");
    check(rejected_server.has_value() && ordinary_client.has_value());
    check(!handshake(*ordinary_client, *rejected_server));
    check(rejected_server->handshake().error == weave::TlsError::protocol);
    auto ordinary_server = weave::detail::TlsEngine::create(*unrelated, true, "");
    auto unchanged_client = weave::detail::TlsEngine::create(*shared, false, "localhost");
    check(ordinary_server.has_value() && unchanged_client.has_value());
    check(handshake(*unchanged_client, *ordinary_server));
    check(unchanged_client->alpn() == "unrelated");
  }
}

int main()
{
  static fixture::Certificates files;
  engine_controls(files);
  std::printf("Required ALPN: %u checks passed\n", checks.load());
}
