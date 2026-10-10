#include <weave/tls.hpp>
#include <weave/timer.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include "tls_certificates.hpp"
#include <cstdio>

using namespace std::chrono_literals;

static weave::Task<void> echo(weave::TcpListener &listener, const weave::TlsContext &credentials, bool mutual)
{
  auto transport = co_await listener.accept();
  auto peer = co_await weave::tls::server(std::move(transport), credentials);
  if (mutual && !peer.peer_identity())
    co_await weave::fail(std::errc::permission_denied);
  std::array<std::byte, 4096> buffer;

  while (auto received = co_await peer.read(buffer))
    co_await peer.write_all(std::span{buffer}.first(received));

  co_await peer.shutdown();
}

static weave::Task<void> client(const weave::TlsContext &credentials, weave::u16 port)
{
  auto peer = co_await weave::tls::connect(credentials, "localhost", port);
  std::vector<std::byte> payload(131073, std::byte{83});
  std::vector<std::byte> reply(payload.size());

  co_await weave::when_all(peer.write_all(payload), peer.read_exactly(reply));
  if (payload != reply || peer.negotiated_protocol() != "echo")
    co_await weave::fail(std::errc::io_error);

  co_await peer.shutdown();
}

int main(int argc, char **argv)
{
  if (argc != 3)
    return 2;

  fixture::Certificates certificates;
  const std::string_view mode{argv[1]};
  const bool mutual = mode.starts_with("mtls-");
  const auto version = std::string_view{argv[2]} == "12" ? weave::TlsVersion::tls12 : weave::TlsVersion::tls13;
  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  std::printf("%s\n%s\n%s\n", certificates.ca.c_str(), certificates.leaf.c_str(), certificates.private_key.c_str());
  if (mutual)
    std::printf("%s\n%s\n", certificates.client.c_str(), certificates.client_key.c_str());
  std::fflush(stdout);

  weave::Result<void> result;
  if (mode == "server" || mode == "mtls-server") {
    auto tls = weave::TlsContext::server(
      {.certificate_file = certificates.leaf,
        .private_key_file = certificates.private_key,
        .alpn = {"echo"},
        .min_version = version,
        .max_version = version,
        .client_auth = mutual ? weave::TlsClientAuth::required : weave::TlsClientAuth::none,
        .ca_file = mutual ? certificates.ca : ""});
    if (!tls)
      return weave::report_error(tls.error());

    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    if (!listener)
      return weave::report_error(listener.error());

    std::printf("%u\n", static_cast<unsigned>(listener->local_port()));
    std::fflush(stdout);
    result = ctx->run(weave::timeout(10s, echo(*listener, *tls, mutual)));
  } else {
    auto tls = weave::TlsContext::client(
      {.ca_file = certificates.ca,
        .alpn = {"echo"},
        .min_version = version,
        .max_version = version,
        .certificate_file = mutual ? certificates.client : "",
        .private_key_file = mutual ? certificates.client_key : ""});
    if (!tls)
      return weave::report_error(tls.error());

    std::array<char, 32> input{};
    if (!std::fgets(input.data(), static_cast<int>(input.size()), stdin))
      return 2;

    std::string_view argument{input.data()};
    argument = argument.substr(0, argument.find_first_of("\r\n"));
    auto port = weave::parse_port(argument);
    if (!port)
      return weave::report_error(port.error());

    result = ctx->run(weave::timeout(10s, client(*tls, *port)));
  }

  if (!result)
    return weave::report_error(result.error());
}
