#include <asio.hpp>
#include <weave/port.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <utility>

// Required by ASIO_NO_EXCEPTIONS; operational errors below use error_code values.
namespace asio::detail {

template <class Exception>
void throw_exception(const Exception &)
{
  std::abort();
}

} // namespace asio::detail

using tcp = asio::ip::tcp;

static asio::awaitable<void> echo(tcp::socket client)
{
  asio::error_code error;
  client.set_option(tcp::no_delay(true), error);
  const auto use_result = asio::redirect_error(asio::use_awaitable, error);
  std::array<std::byte, 4096> buffer;

  while (!error) {
    auto received = co_await client.async_read_some(asio::buffer(buffer), use_result);
    if (error)
      break;

    co_await asio::async_write(client, asio::buffer(buffer.data(), received), use_result);
  }

  if (error != asio::error::eof)
    std::fprintf(stderr, "Client: %s\n", error.message().c_str());
}

static asio::awaitable<void> serve(tcp::acceptor &listener)
{
  const auto executor = co_await asio::this_coro::executor;
  asio::error_code error;
  const auto use_result = asio::redirect_error(asio::use_awaitable, error);

  for (;;) {
    auto client = co_await listener.async_accept(use_result);
    if (error) {
      std::fprintf(stderr, "Accept: %s\n", error.message().c_str());
      std::exit(1);
    }

    asio::co_spawn(executor, echo(std::move(client)), asio::detached);
  }
}

int main(int argc, char **argv)
{
  auto port = weave::parse_port(argc == 2 ? argv[1] : "8080");
  if (argc < 1 || argc > 2 || !port) {
    std::fputs("Usage: echo_server [port: 0-65535]\n", stderr);
    return 2;
  }

  asio::io_context context(1);
  tcp::acceptor listener(context);
  asio::error_code error;
  listener.open(tcp::v4(), error);
  if (!error)
    listener.bind({asio::ip::address_v4::loopback(), *port}, error);
  if (!error)
    listener.listen(512, error);
  if (error) {
    std::fprintf(stderr, "Listen: %s\n", error.message().c_str());
    return 1;
  }

  const auto endpoint = listener.local_endpoint(error);
  if (error) {
    std::fprintf(stderr, "Local port: %s\n", error.message().c_str());
    return 1;
  }
  std::printf("Listening on 127.0.0.1:%u\n", static_cast<unsigned>(endpoint.port()));
  std::fflush(stdout);

  asio::co_spawn(context, serve(listener), asio::detached);
  context.run();
}
