#if defined(WEAVE_USE_TCP)
#include <weave/tcp.hpp>
#elif defined(WEAVE_USE_RUNTIME)
#include <weave/runtime.hpp>
#elif defined(WEAVE_USE_IO)
#include <weave/io.hpp>
#else
#include <weave/core.hpp>
#endif

#if defined(_WINDOWS_) || defined(_WINSOCKAPI_) || defined(_WINSOCK2API_)
#error Public header leaked Windows headers
#endif

#include <array>
#include <cstdio>

static weave::Task<int> value()
{
  co_return 42;
}

#if defined(WEAVE_USE_TCP)
static weave::Task<void> server(weave::TcpListener &listener)
{
  auto socket = co_await listener.accept();
  std::array<std::byte, 4> buffer;
  co_await socket.read_exactly(buffer);
  co_await socket.write_all(buffer);
}

static weave::Task<void> client(weave::Context &context, weave::u16 port)
{
  auto socket = co_await weave::tcp::connect(context, "127.0.0.1", port);
  std::array<std::byte, 4> sent{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  std::array<std::byte, 4> received;
  co_await socket.write_all(sent);
  co_await socket.read_exactly(received);
  if (sent != received)
    co_await weave::fail(std::errc::io_error);
}
#endif

int main()
{
#if defined(WEAVE_USE_TCP)
  weave::Context context;
  if (!context.status())
    return 1;
  auto listener = weave::tcp::listen(context, "127.0.0.1", 0);
  if (!listener)
    return 2;
  auto port = listener->local_port();
  if (!port)
    return 3;
  auto result = context.run(weave::when_all(server(*listener), client(context, *port)));
  return result ? 0 : 4;
#elif defined(WEAVE_USE_RUNTIME)
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    weave::Runtime runtime({.workers = 2, .scheduler = scheduler});
    if (!runtime.status())
      return 1;
    auto task = runtime.spawn([](weave::Context &context) -> weave::Task<int> {
      co_await context.yield();
      co_return co_await value();
    });
    if (!task || std::move(*task).get() != 42)
      return 2;
    runtime.join();
  }
  return 0;
#elif defined(WEAVE_USE_IO)
  weave::Context context;
  if (!context.status())
    return 1;
  auto task = [&]() -> weave::Task<int> {
    co_await context.yield();
    co_return co_await value();
  };
  return context.run(task()) == 42 ? 0 : 2;
#else
  auto task = value();
  weave::detail::TaskAccess::start(task);
  if (!weave::detail::TaskAccess::done(task))
    return 1;
  return weave::detail::TaskAccess::take(task) == 42 ? 0 : 2;
#endif
}
