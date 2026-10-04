#include <weave/tcp.hpp>
#include <weave/log.hpp>

#include <array>
#include <cstdio>

weave::Task<void> echo(weave::TcpStream &client)
{
  std::array<std::byte, 4096> buffer;

  for (;;) {
    const auto read = co_await client.read(buffer);
    if (read == 0)
      co_return;
    co_await client.write_all(std::span(buffer).first(read));
  }
}

weave::Task<void> serve(weave::TcpListener &listener)
{
  for (;;) {
    auto client = co_await listener.accept();
    auto result = co_await weave::as_result(echo(client));
    if (!result)
      WEAVE_LOG_ERROR("Client: %s\n", result.error().message().c_str());
  }
}

int main()
{
  weave::Context ctx;
  auto status = ctx.status();
  if (!status)
    return weave::report_error(status.error());

  auto listener = weave::tcp::listen(ctx, "127.0.0.1", 8080);
  if (!listener)
    return weave::report_error(listener.error());

  WEAVE_LOG_INFO("Weave echo: 127.0.0.1:8080");

  auto result = ctx.run(serve(*listener));
  if (!result)
    return weave::report_error(result.error());
}
