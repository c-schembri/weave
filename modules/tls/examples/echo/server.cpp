#include <weave/tls.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include <array>
#include <cstdlib>

static weave::Task<void> echo(weave::TcpStream transport, const weave::TlsContext &credentials)
{
  auto client = co_await weave::tls::server(std::move(transport), credentials);
  std::array<std::byte, 4096> buffer;

  while (auto received = co_await client.read(buffer))
    co_await client.write_all(std::span{buffer}.first(received));

  co_await client.shutdown();
}

static weave::Task<void> serve(const weave::TlsContext &credentials, weave::u16 port)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", port);
  WEAVE_LOG_INFO("Weave TLS echo: 127.0.0.1:%u", static_cast<unsigned>(listener.local_port()));

  co_await weave::tcp::serve(
    listener,
    {.no_delay = true},
    [&](weave::TcpStream transport) {
      return echo(std::move(transport), credentials);
    },
    [](weave::Error error) noexcept {
      WEAVE_LOG_ERROR("Client: %s", error.message().c_str());
    });
}

int main(int argc, char **argv)
{
  if (argc < 3 || argc > 4) {
    WEAVE_LOG_ERROR("Usage: tls_echo_server certificate.pem key.pem [port: 0-65535]");
    return EXIT_FAILURE;
  }

  auto port = weave::parse_port(argc == 4 ? argv[3] : "8443");
  if (!port)
    return weave::report_error(port.error());

  auto credentials = weave::TlsContext::server({.certificate_file = argv[1], .private_key_file = argv[2]});
  if (!credentials)
    return weave::report_error(credentials.error());

  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  auto result = ctx->run(serve(*credentials, *port));
  if (!result)
    return weave::report_error(result.error());
}
