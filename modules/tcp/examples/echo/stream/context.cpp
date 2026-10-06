#include <weave/tcp.hpp>
#include <weave/io/detach.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>

#include <array>
#include <cstdlib>
#include <span>
#include <utility>

static weave::Task<void> echo(weave::TcpStream client)
{
  std::array<std::byte, 4096> buffer;

  while (auto received = co_await client.read(buffer))
    co_await client.write_all(std::span{buffer}.first(received));
}

static weave::Task<void> serve(weave::u16 port)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", port);
  WEAVE_LOG_INFO("Weave echo: 127.0.0.1:%u", static_cast<unsigned>(listener.local_port()));

  for (;;) {
    auto client = co_await listener.accept({.no_delay = true});
    weave::detach(echo(std::move(client)), [](std::error_code error) noexcept {
      WEAVE_LOG_ERROR("Client: %s", error.message().c_str());
    });
  }
}

int main(int argc, char **argv)
{
  auto port = weave::parse_port(argc == 2 ? argv[1] : "8080");
  if (argc < 1 || argc > 2 || !port) {
    WEAVE_LOG_ERROR("Usage: echo_server_stream_context [port: 0-65535]");
    return EXIT_FAILURE;
  }

  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  auto result = ctx->run(serve(*port));
  if (!result)
    return weave::report_error(result.error());
}
