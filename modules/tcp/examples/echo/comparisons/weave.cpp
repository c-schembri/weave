#include <weave/tcp.hpp>
#include <weave/runtime.hpp>
#include <weave/port.hpp>
#include <cstdio>
#include <span>

static weave::Task<void> serve(weave::u16 port)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", port, 512);
  std::printf("Listening on 127.0.0.1:%u\n", static_cast<unsigned>(listener.local_port()));
  std::fflush(stdout);

  co_await weave::tcp::serve(
    listener,
    {.no_delay = true},
    weave::tcp::on_data(
      [](weave::TcpStream &client, std::span<const std::byte> data) { return client.write_all(data); }),
    [](std::error_code error) noexcept { std::fprintf(stderr, "Client: %s\n", error.message().c_str()); });
}

int main(int argc, char **argv)
{
  auto port = weave::parse_port(argc == 2 ? argv[1] : "8080");
  if (argc < 1 || argc > 2 || !port) {
    std::fputs("Usage: echo_server [port: 0-65535]\n", stderr);
    return 2;
  }

  auto runtime = weave::Runtime::create({.workers = 1});
  if (!runtime) {
    std::fprintf(stderr, "Runtime: %s\n", runtime.error().message().c_str());
    return 1;
  }

  auto result = runtime->run(serve(*port));
  if (!result) {
    std::fprintf(stderr, "Server: %s\n", result.error().message().c_str());
    return 1;
  }
}
