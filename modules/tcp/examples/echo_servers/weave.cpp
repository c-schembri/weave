#include <weave/tcp.hpp>
#include <weave/runtime.hpp>
#include "common.hpp"
#include <array>

weave::Task<void> echo(weave::TcpStream &client)
{
  co_await client.no_delay();
  std::array<std::byte, 4096> buffer;
  for (;;) {
    auto received = co_await client.read(buffer);
    if (received == 0) {
      co_await client.shutdown_send();
      co_return;
    }
    co_await client.write_all(std::span(buffer).first(received));
  }
}

weave::Task<void> session(weave::TcpStream client)
{
  if (auto result = co_await weave::as_result(echo(client)); !result)
    example::fail("Client", result.error().message());
}

weave::Task<void> serve(weave::Runtime &runtime, weave::Context &context, weave::u16 port)
{
  auto listener = co_await weave::tcp::listen(context, "127.0.0.1", port, 512);
  example::listening(co_await listener.local_port());

  for (;;) {
    auto client = co_await listener.accept();
    auto job = co_await runtime.spawn_on(0, [socket = std::move(client)](weave::Context &) mutable {
      return session(std::move(socket));
    });
    // Dropping the result handle is safe: Runtime owns the session until completion.
  }
}

int main(int argc, char **argv)
{
  auto port = example::port(argc, argv);
  if (!port)
    return 2;
  weave::Runtime runtime({.workers = 1});
  if (auto result = runtime.status(); !result)
    return example::fail("Runtime", result.error().message());
  auto server = runtime.spawn_on(0, [&](weave::Context &context) { return serve(runtime, context, *port); });
  if (!server)
    return example::fail("Spawn", server.error().message());
  auto result = std::move(*server).get();
  runtime.shutdown();
  return result ? 0 : example::fail("Server", result.error().message());
}
