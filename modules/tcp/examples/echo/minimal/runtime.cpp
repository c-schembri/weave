#include <weave/tcp.hpp>
#include <weave/runtime.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>

#include <cstdlib>
#include <span>

static weave::Task<void> serve(weave::u16 port)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", port);
  WEAVE_LOG_INFO("Weave echo: 127.0.0.1:%u", static_cast<unsigned>(listener.local_port()));

  co_await weave::tcp::serve(
    listener,
    {.no_delay = true},
    weave::tcp::on_data(
      [](weave::TcpStream &client, std::span<const std::byte> data) { return client.write_all(data); }),
    [](std::error_code error) noexcept { WEAVE_LOG_ERROR("Client: %s", error.message().c_str()); });
}

int main(int argc, char **argv)
{
  auto port = weave::parse_port(argc == 2 ? argv[1] : "8080");
  if (argc < 1 || argc > 2 || !port) {
    WEAVE_LOG_ERROR("Usage: echo_server_runtime_minimal [port: 0-65535]");
    return EXIT_FAILURE;
  }

  auto runtime = weave::Runtime::create({.workers = 4, .scheduler = weave::Scheduler::work_stealing});
  if (!runtime)
    return weave::report_error(runtime.error());

  auto result = runtime->run(serve(*port));
  if (!result)
    return weave::report_error(result.error());
}
