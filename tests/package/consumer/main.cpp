#if defined(WEAVE_USE_TCP)
#include <weave/tcp.hpp>
#elif defined(WEAVE_USE_RUNTIME)
#include <weave/runtime.hpp>
#elif defined(WEAVE_USE_IO)
#include <weave/io.hpp>
#else
#include <weave/core.hpp>
#endif
#include <weave/port.hpp>

#if defined(_WINDOWS_) || defined(_WINSOCKAPI_) || defined(_WINSOCK2API_)
#error Public header leaked Windows headers
#endif

#include <array>
#include <cstdio>
#include <cstdlib>

static weave::Task<int> value()
{
  co_return 42;
}

#if defined(WEAVE_USE_RUNTIME) || defined(WEAVE_USE_IO)
static void detach_error(weave::Error) noexcept
{
  std::abort();
}

static weave::Task<void> scoped_detach_values()
{
  weave::detach(value());
  weave::detach(value);
  co_return;
}
#endif

#if defined(WEAVE_USE_TCP)
static weave::Task<void> server(weave::TcpListener &listener)
{
  auto socket = co_await listener.accept();
  std::array<std::byte, 4> buffer;
  co_await socket.read_exactly(buffer);
  co_await socket.write_all(buffer);
}

static weave::Task<void> client(weave::u16 port)
{
  auto socket = co_await weave::tcp::connect("127.0.0.1", port);
  std::array<std::byte, 4> sent{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  std::array<std::byte, 4> received;
  co_await socket.write_all(sent);
  co_await socket.read_exactly(received);
  if (sent != received)
    co_await weave::fail(std::errc::io_error);
}

static weave::Task<void> discard_client(weave::TcpStream)
{
  co_return;
}

static weave::Task<void> echo_data(weave::TcpStream &socket, std::span<const std::byte> data)
{
  co_await socket.write_all(data);
}
#endif

int main()
{
  if (weave::parse_port("8080") != 8080)
    return 5;

#if defined(WEAVE_USE_TCP)
  auto context = weave::Context::create();
  if (!context)
    return 1;
  auto listener = weave::tcp::listen(*context, "127.0.0.1", 0);
  if (!listener)
    return 2;
  auto port = listener->local_port();
  if (port == 0)
    return 3;
  auto result = context->run(weave::when_all(server(*listener), client(port)));
  if (!result)
    return 4;
  auto implicit = context->run(weave::tcp::listen("127.0.0.1", 0));
  if (!implicit || implicit->local_port() == 0)
    return 5;
  // Instantiate the server helper against installed TCP/IO headers, without Runtime.
  auto invalid = context->run(weave::tcp::serve("invalid", 0, {.no_delay = true}, discard_client));
  if (invalid || invalid.error() != std::errc::invalid_argument)
    return 6;
  auto buffered = context->run(weave::tcp::serve("invalid", 0, {}, weave::tcp::on_data(echo_data)));
  if (buffered || buffered.error() != std::errc::invalid_argument)
    return 7;
  return 0;
#elif defined(WEAVE_USE_RUNTIME)
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    auto runtime = weave::Runtime::create({.workers = 2, .scheduler = scheduler});
    if (!runtime)
      return 1;
    if (runtime->run(value()) != 42 || runtime->run(value) != 42)
      return 2;
    auto inherited = runtime->run(scoped_detach_values());
    if (!inherited)
      return 2;
    runtime->detach([](weave::Context &ctx) -> weave::Task<void> { co_await ctx.yield(); }, detach_error);
    runtime->detach(value(), detach_error);
    runtime->detach(value, detach_error);
    runtime->detach_on(0, value(), detach_error);
    runtime->detach_on(0, value);
    auto direct = runtime->spawn(value());
    auto pinned = runtime->spawn_on(0, value());
    auto deferred = runtime->spawn_on(0, value);
    if (!direct || !pinned || !deferred || std::move(*direct).get() != 42 || std::move(*pinned).get() != 42 ||
      std::move(*deferred).get() != 42)
      return 2;
    auto task = runtime->spawn([](weave::Context &context) -> weave::Task<int> {
      co_await context.yield();
      co_return co_await value();
    });
    if (!task || std::move(*task).get() != 42)
      return 2;
    runtime->join();
  }
  return 0;
#elif defined(WEAVE_USE_IO)
  auto context = weave::Context::create();
  if (!context)
    return 1;
  auto inherited = context->run(scoped_detach_values());
  if (!inherited)
    return 2;
  context->detach([](weave::Context &ctx) -> weave::Task<void> { co_await ctx.yield(); }, detach_error);
  context->detach(value(), detach_error);
  context->detach(value, detach_error);
  auto direct = context->spawn(value());
  auto deferred = context->spawn(value);
  if (!direct || !deferred || context->run(std::move(*direct).as_task()) != 42 ||
    context->run(std::move(*deferred).as_task()) != 42)
    return 2;
  auto task = context->spawn([](weave::Context &ctx) -> weave::Task<int> {
    co_await ctx.yield();
    co_return co_await value();
  });
  if (!task)
    return 2;
  return context->run(std::move(*task).as_task()) == 42 ? 0 : 3;
#else
  int observed = 0;
  auto task = value().on_error([&](weave::Error) noexcept { ++observed; });
  weave::detail::TaskAccess::start(task);
  if (!weave::detail::TaskAccess::done(task))
    return 1;
  return weave::detail::TaskAccess::take(task) == 42 && observed == 0 ? 0 : 2;
#endif
}
