#include <weave/tcp.hpp>
#include <weave/runtime.hpp>
#include <weave/log.hpp>
#include <weave/io/detach.hpp>
#include "asio_pool.hpp"
#include "runtime_workload.hpp"
#include <windows.h>
#include <cstdio>
#include <string_view>
#include <vector>

using asio::ip::tcp;
using bench::stress::Workload;
static constexpr auto use_result = asio::as_tuple(asio::use_awaitable);

static weave::Task<void> weave_session(weave::TcpStream client, Workload config)
{
  std::vector<std::byte> buffer(config.bytes);
  for (;;) {
    co_await client.read_exactly(buffer);
    bench::stress::transform(buffer, config);
    co_await client.write_all(buffer);
  }
}

static weave::Task<void> weave_server(Workload config)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0, 8192);
  std::printf("{\"port\":%u,\"workers\":4}\n", static_cast<unsigned>(listener.local_port()));
  std::fflush(stdout);
  for (;;) {
    auto client = co_await listener.accept({.no_delay = true});
    weave::detach(weave_session(std::move(client), config));
  }
}

static asio::awaitable<void> asio_session(tcp::socket client, Workload config)
{
  std::vector<std::byte> buffer(config.bytes);
  for (;;) {
    auto [read_error, received] = co_await asio::async_read(client, asio::buffer(buffer), use_result);
    if (read_error || received != buffer.size())
      co_return;
    bench::stress::transform(buffer, config);
    auto [write_error, sent] = co_await asio::async_write(client, asio::buffer(buffer), use_result);
    if (write_error || sent != buffer.size())
      co_return;
  }
}

static asio::awaitable<void> asio_server(tcp::acceptor &listener, Workload config)
{
  for (;;) {
    auto [error, client] = co_await listener.async_accept(use_result);
    if (error)
      std::abort();
    client.set_option(tcp::no_delay(true), error);
    if (error)
      std::abort();
    asio::co_spawn(listener.get_executor(), asio_session(std::move(client), config), asio::detached);
  }
}

int main(int argc, char **argv)
{
  Workload config;
  unsigned uneven = 0;
  std::uintptr_t mask = 0;
  if (argc != 6 || !bench::stress::number(argv[2], config.bytes) || !bench::stress::number(argv[3], config.work) ||
    !bench::stress::number(argv[4], uneven) || !bench::stress::number(argv[5], mask) || uneven > 1 ||
    !bench::stress::valid(config)) {
    std::fputs("Usage: weave_runtime_server weave|weave-shared|asio bytes cpu uneven affinity-mask\n", stderr);
    return 1;
  }
  config.uneven = uneven != 0;
  if (mask && !SetProcessAffinityMask(GetCurrentProcess(), mask))
    return 1;
  const std::string_view backend{argv[1]};
  if (backend == "weave" || backend == "weave-shared") {
    const auto layout = backend == "weave" ? weave::IoLayout::sharded : weave::IoLayout::shared;
    auto runtime = weave::Runtime::create(
      {.workers = 4, .scheduler = weave::Scheduler::work_stealing, .io_layout = layout});
    if (!runtime)
      return weave::report_error(runtime.error());
    auto result = runtime->run(weave_server(config));
    return result ? 0 : weave::report_error(result.error());
  }
  if (backend != "asio")
    return 1;
  bench::AsioPool pool(4, false);
  tcp::acceptor listener(pool.context(0));
  asio::error_code error;
  listener.open(tcp::v4(), error);
  if (!error)
    listener.bind(tcp::endpoint(asio::ip::address_v4::loopback(), 0), error);
  if (!error)
    listener.listen(8192, error);
  if (error)
    return 1;
  const auto endpoint = listener.local_endpoint(error);
  if (error)
    return 1;
  asio::co_spawn(pool.context(0), asio_server(listener, config), asio::detached);
  std::printf("{\"port\":%u,\"workers\":4}\n", static_cast<unsigned>(endpoint.port()));
  std::fflush(stdout);
  // The external supervisor ends each server after sampling its process counters.
  for (;;)
    Sleep(INFINITE);
}
