#include <weave/postgres.hpp>
#include <weave/io.hpp>
#include <weave/log.hpp>
#include "wire.hpp"
#include <atomic>
#include <iostream>

namespace pg = weave::pg;
namespace wire = weave::pg::detail;
using namespace std::chrono_literals;

static_assert(std::constructible_from<pg::TraceHandler, decltype([](const pg::TraceMessage &) noexcept {
})>);
static_assert(!std::constructible_from<pg::TraceHandler, decltype([](const pg::TraceMessage &) {
})>);
static_assert(!std::copy_constructible<pg::Trace>);

static void check(bool condition, const char *message)
{
  if (!condition) {
    WEAVE_LOG_ERROR("Trace: %s", message);
    std::abort();
  }
}

static weave::Task<wire::Message> receive(weave::TcpStream &client)
{
  std::array<std::byte, 5> header;
  co_await client.read_exactly(header);
  wire::Reader reader{header};
  wire::Message message;
  message.kind = static_cast<char>(reader.integer(1));
  auto length = reader.integer();
  check(length >= 4 && length <= 4096, "Owned peer frame bound");
  message.body.resize(length - 4);
  co_await client.read_exactly(message.body);
  co_return message;
}

static weave::Task<void> peer(weave::TcpListener &listener)
{
  for (unsigned round = 0; round < 2; ++round) {
    auto client = co_await listener.accept();
    std::array<std::byte, 4> prefix;
    co_await client.read_exactly(prefix);
    wire::Reader header{prefix};
    auto length = header.integer();
    check(length >= 8 && length <= 4096, "Startup bound");
    wire::Bytes startup(length - 4);
    co_await client.read_exactly(startup);

    wire::Writer challenge;
    challenge.integer(5);
    challenge.raw("salt");
    wire::Writer outgoing;
    outgoing.message('R', challenge);
    co_await client.write_all(outgoing.bytes);
    auto password = co_await receive(client);
    check(password.kind == 'p' && password.body.size() == 36, "Actual MD5 response");

    wire::Writer accepted;
    accepted.integer(0);
    wire::Writer key;
    key.integer(99);
    key.raw("S3CR");
    wire::Writer ready;
    ready.integer('I', 1);
    outgoing.bytes.clear();
    outgoing.message('R', accepted);
    outgoing.message('K', key);
    outgoing.message('Z', ready);
    co_await client.write_all(outgoing.bytes);

    for (;;) {
      auto message = co_await weave::as_result(receive(client));
      if (!message) {
        check(message.error() == std::errc::connection_reset, "Reset closes its original session");
        break;
      }
      if (message->kind == 'X')
        break;
      check(message->kind == 'Q', "Simple query");
      std::string text{reinterpret_cast<const char *>(message->body.data()), message->body.size()};
      outgoing.bytes.clear();
      if (text.starts_with("INJECT")) {
        outgoing.message('K', key);
      } else {
        wire::Writer command;
        command.string("SELECT 1");
        outgoing.message('C', command);
        outgoing.message('Z', ready);
      }
      co_await client.write_all(outgoing.bytes);
      if (text.starts_with("INJECT"))
        break;
    }
  }
}

static weave::Task<void> client(pg::Options options)
{
  pg::Connection *active = nullptr;
  unsigned frames = 0, queries = 0, keys = 0, passwords = 0;
  pg::Trace trace{
    .handler =
      [&](const pg::TraceMessage &message) noexcept {
        ++frames;
        if (message.kind == 'K' || message.kind == 'R' || message.kind == 'p' || message.kind == 0) {
          check(message.redacted && message.payload.empty() && !message.truncated, "Credentials never reach callback");
          keys += message.kind == 'K';
          passwords += message.kind == 'p';
        }
        if (message.kind == 'Q') {
          ++queries;
          check(
            message.payload.size() == 2 && message.truncated && !message.redacted,
            "Explicit bounded application body");
          check(message.length > message.payload.size() + 4, "Original full frame length retained");
          if (active) {
            auto rejected = active->on_trace({});
            check(!rejected && rejected.error() == pg::Error::busy, "No reentrant receiver replacement");
          }
        }
      },
    .content = pg::TraceContent::application,
    .payload_bytes = 2};
  auto connection = co_await pg::connect(options, {}, std::move(trace));
  active = &connection;
  auto before = frames;
  {
    auto deferred = connection.query("TRACE-UNSTARTED");
  }
  check(frames == before, "Dropped unstarted task emits nothing");
  co_await connection.query("TRACE-SQL-SECRET");
  co_await connection.reset(options);
  co_await connection.query("TRACE-RESET");
  auto borrowed = connection.query("TRACE-DEFERRED");
  auto busy = connection.on_trace({});
  check(!busy && busy.error() == pg::Error::busy, "Deferred borrower retains receiver");
  co_await std::move(borrowed);
  unsigned metadata = 0;
  auto application = connection.on_trace({.handler = [&metadata](const pg::TraceMessage &message) noexcept {
    ++metadata;
    check(
      message.payload.empty() && message.payload.data() == nullptr && message.redacted && !message.truncated,
      "Default metadata does not expose any body address or data");
  }});
  check(
    application && application->handler && application->content == pg::TraceContent::application,
    "Replacement returns owning application configuration");
  co_await connection.query("METADATA-PRIVATE-DATA");
  check(metadata == 3, "One query, command completion and ready frame");
  auto saved = connection.on_trace(std::move(*application));
  check(saved && saved->handler, "Application configuration restored");
  auto invalid = connection.on_trace({.content = static_cast<pg::TraceContent>(99)});
  check(!invalid && invalid.error() == std::errc::invalid_argument, "Invalid replacement leaves receiver intact");
  auto rejected = co_await weave::as_result(connection.query("INJECT-AUTH-KEY"));
  check(!rejected && rejected.error() == pg::Error::protocol, "Invalid backend key is still rejected");
  check(queries == 4 && keys == 3 && passwords == 2, "Reset retains one receiver; late key remains redacted");
  auto previous = connection.on_trace({});
  check(previous && previous->handler, "Synchronous disable returns owning prior configuration");
}

int main()
{
  auto ctx = weave::Context::create();
  check(bool(ctx), "Context");
  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  check(bool(listener), "Owned listener");
  pg::Options options;
  options.host = "127.0.0.1";
  options.port = listener->local_port();
  options.user = "weave";
  options.password = "trace-password";
  options.plaintext = true;
  options.allow_md5_password = true;
  options.channel_binding = pg::ChannelBinding::disable;
  std::atomic<unsigned> released{0};
  auto owner = std::shared_ptr<int>(new int{42}, [&released](int *value) noexcept {
    delete value;
    ++released;
  });
  {
    auto unstarted = pg::connect(options, {}, {.handler = [owner](const pg::TraceMessage &) noexcept {
    }});
    owner.reset();
    check(released == 0, "Lazy connect owns receiver before first resume");
  }
  check(released == 1, "Dropped connect releases observer without invocation");
  auto invalid = ctx->run(pg::connect(options, {}, {.content = static_cast<pg::TraceContent>(99)}));
  check(!invalid && invalid.error() == std::errc::invalid_argument, "Invalid startup trace rejected before I/O");
  auto result = ctx->run(weave::timeout(10s, weave::when_all(peer(*listener), client(options))));
  if (!result)
    return weave::report_error(result.error());
  std::cout << "Trace: redaction, bounds, reset, deferred lifetime and rejection passed\n";
}
