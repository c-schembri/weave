#include <asio.hpp>
#include "common.hpp"
#include <array>
#include <cstdlib>

// Required by ASIO_NO_EXCEPTIONS; operational errors below use error_code values.
namespace asio::detail {

template <class Exception>
void throw_exception(const Exception &)
{
  std::abort();
}

} // namespace asio::detail

using tcp = asio::ip::tcp;
constexpr auto use_result = asio::as_tuple(asio::use_awaitable);

asio::awaitable<void> echo(tcp::socket client)
{
  asio::error_code error;
  client.set_option(tcp::no_delay(true), error);
  if (error) {
    example::fail("TCP_NODELAY", error.message());
    co_return;
  }
  std::array<std::byte, 4096> buffer;
  for (;;) {
    auto [read_error, size] = co_await client.async_read_some(asio::buffer(buffer), use_result);
    if (read_error == asio::error::eof) {
      client.shutdown(tcp::socket::shutdown_send, error);
      if (error)
        example::fail("Shutdown", error.message());
      co_return;
    }
    if (read_error) {
      example::fail("Read", read_error.message());
      co_return;
    }
    auto [write_error, sent] = co_await asio::async_write(client, asio::buffer(buffer.data(), size), use_result);
    if (write_error || sent != size) {
      example::fail("Write", write_error ? write_error.message() : "Incomplete echo");
      co_return;
    }
  }
}

asio::awaitable<void> serve(tcp::acceptor &listener)
{
  const auto executor = co_await asio::this_coro::executor;
  for (;;) {
    auto [error, client] = co_await listener.async_accept(use_result);
    if (error) {
      example::fail("Accept", error.message());
      std::exit(1);
    }
    asio::co_spawn(executor, echo(std::move(client)), asio::detached);
  }
}

int main(int argc, char **argv)
{
  auto port = example::port(argc, argv);
  if (!port)
    return 2;
  asio::io_context context(1);
  tcp::acceptor listener(context);
  asio::error_code error;
  listener.open(tcp::v4(), error);
  if (!error)
    listener.bind({asio::ip::address_v4::loopback(), *port}, error);
  if (!error)
    listener.listen(512, error);
  if (error)
    return example::fail("Listen", error.message());
  const auto endpoint = listener.local_endpoint(error);
  if (error)
    return example::fail("Local port", error.message());
  example::listening(endpoint.port());
  asio::co_spawn(context, serve(listener), asio::detached);
  context.run();
}
