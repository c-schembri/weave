#include <weave/postgres.hpp>
#include <weave/io.hpp>
#include <weave/timer.hpp>
#include <weave/channel.hpp>
#include "wire.hpp"
#include <doctest/doctest.h>

namespace pg = weave::pg;
namespace wire = weave::pg::detail;
using namespace std::chrono_literals;

enum class PipelinePeer {
  missing_parse,
  duplicate_parse,
  missing_bind,
  missing_description,
  duplicate_description,
  empty_after_description,
  early_ready,
  duplicate_command,
  invalid_diagnostic,
  copy,
  oversized,
  duplex,
  lifetime
};

static wire::Writer pipeline_ready()
{
  wire::Writer idle;
  idle.integer('I', 1);
  wire::Writer response;
  response.message('Z', idle);
  return response;
}

static weave::Task<char> pipeline_request(weave::TcpStream &socket)
{
  std::array<std::byte, 5> header;
  co_await socket.read_exactly(header);
  wire::Reader reader{header};
  auto kind = static_cast<char>(reader.integer(1));
  auto size = reader.integer();
  if (size < 4 || size > 128 * 1024)
    co_await weave::fail(std::errc::bad_message);

  wire::Bytes body(size - 4);
  co_await socket.read_exactly(body);
  co_return kind;
}

static wire::Writer pipeline_columns()
{
  wire::Writer columns;
  columns.integer(1, 2);
  columns.string("value");
  columns.integer(0);
  columns.integer(0, 2);
  columns.integer(25);
  columns.integer(0xffff, 2);
  columns.integer(0xffffffff);
  columns.integer(0, 2);
  return columns;
}

static weave::Task<void> pipeline_backend(weave::TcpListener &listener, PipelinePeer mode)
{
  auto socket = co_await listener.accept({.no_delay = true});
  std::array<std::byte, 4> header;
  co_await socket.read_exactly(header);
  wire::Reader reader{header};
  auto size = reader.integer();
  REQUIRE(size >= 8);
  REQUIRE(size <= 1024);
  wire::Bytes greeting(size - 4);
  co_await socket.read_exactly(greeting);

  wire::Writer response;
  wire::Writer authentication;
  authentication.integer(0);
  response.message('R', authentication);
  response.raw(pipeline_ready().bytes);
  co_await socket.write_all(response.bytes);

  if (mode == PipelinePeer::lifetime) {
    std::array<std::byte, 1> closed;
    auto ended = co_await weave::as_result(socket.read(closed));
    CHECK((!ended || *ended == 0));
    co_return;
  }

  for (;;) {
    auto kind = co_await pipeline_request(socket);
    if (mode == PipelinePeer::duplex) {
      wire::Writer output;
      if (kind == 'P')
        output.message('1');
      else if (kind == 'B')
        output.message('2');
      else if (kind == 'D')
        output.message('T', pipeline_columns());
      else if (kind == 'E') {
        wire::Writer row;
        row.integer(1, 2);
        row.integer(65536);
        row.raw(std::string(65536, 'x'));
        output.message('D', row);
        wire::Writer complete;
        complete.string("SELECT 1");
        output.message('C', complete);
      } else if (kind == 'S') {
        output.raw(pipeline_ready().bytes);
      }
      co_await socket.write_all(output.bytes);
    }

    if (kind == 'H')
      break;
  }

  if (mode != PipelinePeer::duplex) {
    wire::Writer invalid;
    wire::Writer empty_columns;
    empty_columns.integer(0, 2);
    wire::Writer command;
    command.string("SELECT 0");
    switch (mode) {
    case PipelinePeer::missing_parse:
      invalid.message('2');
      break;
    case PipelinePeer::duplicate_parse:
      invalid.message('1');
      invalid.message('1');
      break;
    case PipelinePeer::missing_bind:
      invalid.message('1');
      invalid.message('T', empty_columns);
      break;
    case PipelinePeer::missing_description:
      invalid.message('1');
      invalid.message('2');
      invalid.message('D', empty_columns);
      break;
    case PipelinePeer::duplicate_description:
    case PipelinePeer::empty_after_description:
      invalid.message('1');
      invalid.message('2');
      invalid.message('T', empty_columns);
      invalid.message(mode == PipelinePeer::duplicate_description ? 'T' : 'I', empty_columns);
      break;
    case PipelinePeer::early_ready:
      invalid.raw(pipeline_ready().bytes);
      break;
    case PipelinePeer::duplicate_command:
      invalid.message('1');
      invalid.message('2');
      invalid.message('n');
      invalid.message('C', command);
      invalid.message('C', command);
      invalid.raw(pipeline_ready().bytes);
      break;
    case PipelinePeer::invalid_diagnostic: {
      wire::Writer diagnostic;
      diagnostic.integer('C', 1);
      diagnostic.string("bad");
      diagnostic.integer(0, 1);
      invalid.message('E', diagnostic);
      break;
    }
    case PipelinePeer::copy:
      invalid.message('G');
      break;
    case PipelinePeer::oversized:
      invalid.integer('T', 1);
      invalid.integer(2048);
      break;
    default:
      co_await weave::fail(std::errc::bad_message);
    }
    co_await socket.write_all(invalid.bytes);
  }

  std::array<std::byte, 1> closed;
  auto ended = co_await weave::as_result(socket.read(closed));
  CHECK((!ended || *ended == 0));
}

static weave::Task<void> pipeline_client(weave::u16 port, PipelinePeer mode)
{
  pg::Options options{.host = "127.0.0.1", .port = port, .user = "test", .plaintext = true};
  if (mode != PipelinePeer::duplex) {
    options.limits.message_bytes = 1024;
    options.limits.result_bytes = 1024;
  }
  auto connection = co_await pg::connect(options);
  auto pipeline = connection.pipeline();
  REQUIRE(pipeline);

  if (mode == PipelinePeer::duplex) {
    const std::string payload(65536, 'x');
    for (int index = 0; index < 64; ++index)
      REQUIRE(pipeline->execute({"SELECT $1::text", {{payload}}}));
    REQUIRE(pipeline->sync());
    co_await weave::timeout(2s, pipeline->flush());
    for (weave::u64 id = 1; id <= 64; ++id) {
      auto result = co_await pipeline->next();
      REQUIRE(result);
      CHECK(result->id == id);
      REQUIRE(result->outcome.result);
      CHECK(result->outcome.result->rows.front().front().bytes() == payload);
    }
    auto barrier = co_await pipeline->next();
    REQUIRE(barrier);
    CHECK(barrier->kind == pg::PipelineKind::sync);
    CHECK(barrier->transaction == pg::Transaction::idle);
    CHECK_FALSE(co_await pipeline->next());
    CHECK(pipeline->finish());
    CHECK(connection.close());
    co_return;
  }

  REQUIRE(pipeline->execute({"SELECT 1"}));
  REQUIRE(pipeline->sync());
  auto result = co_await weave::as_result(pipeline->flush());
  REQUIRE_FALSE(result);
  auto expected = mode == PipelinePeer::copy ? pg::Error::unexpected_copy : pg::Error::protocol;
  if (mode == PipelinePeer::oversized)
    expected = pg::Error::resource_limit;
  CHECK(result.error() == expected);
  CHECK_FALSE(connection.open());
}

TEST_CASE("PostgreSQL pipeline rejects out-of-order malformed and COPY responses")
{
  const std::array modes{
    PipelinePeer::missing_parse,
    PipelinePeer::duplicate_parse,
    PipelinePeer::missing_bind,
    PipelinePeer::missing_description,
    PipelinePeer::duplicate_description,
    PipelinePeer::empty_after_description,
    PipelinePeer::early_ready,
    PipelinePeer::duplicate_command,
    PipelinePeer::invalid_diagnostic,
    PipelinePeer::copy,
    PipelinePeer::oversized,
    PipelinePeer::duplex};
  for (auto mode : modes) {
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    CHECK(ctx->run(weave::when_all(pipeline_backend(*listener, mode), pipeline_client(listener->local_port(), mode))));
  }
}

static wire::Writer split_result(bool synchronized)
{
  wire::Writer result;
  result.message('1');
  result.message('2');
  result.message('n');
  wire::Writer tag;
  tag.string("SELECT 0");
  result.message('C', tag);
  if (synchronized)
    result.raw(pipeline_ready().bytes);
  return result;
}

static weave::Task<void> split_peer(weave::TcpListener &listener)
{
  auto socket = co_await listener.accept();
  std::array<std::byte, 4> size;
  co_await socket.read_exactly(size);
  wire::Reader startup{size};
  wire::Bytes body(startup.integer() - 4);
  co_await socket.read_exactly(body);
  wire::Writer greeting;
  wire::Writer authentication;
  authentication.integer(0);
  greeting.message('R', authentication);
  greeting.raw(pipeline_ready().bytes);
  co_await socket.write_all(greeting.bytes);

  for (unsigned batch = 0; batch < 2; ++batch) {
    while (co_await pipeline_request(socket) != 'H') {
    }
  }
  auto first = split_result(true);
  co_await socket.write_all(first.bytes);
  auto second = split_result(false);
  co_await socket.write_all(second.bytes);

  while (co_await pipeline_request(socket) != 'H') {
  }
  auto final = pipeline_ready();
  co_await socket.write_all(final.bytes);
  std::array<std::byte, 1> eof;
  static_cast<void>(co_await weave::as_result(socket.read(eof)));
}

static weave::Task<void> split_client(weave::u16 port)
{
  auto connection = co_await pg::connect({.host = "127.0.0.1", .port = port, .user = "test", .plaintext = true});
  auto pipe = connection.pipeline();
  REQUIRE(pipe);
  REQUIRE(pipe->execute({"SELECT 1"}));
  REQUIRE(pipe->sync());

  co_await weave::scope([&](weave::TaskScope &tasks) -> weave::Task<void> {
    auto reader = tasks.spawn(pipe->receive());
    REQUIRE(reader);
    co_await weave::sleep_for(1ms);
    auto duplicate = co_await weave::as_result(pipe->receive());
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error() == pg::Error::busy);

    co_await pipe->send();
    auto none = pipe->try_next();
    REQUIRE_FALSE(none);
    CHECK(none.error() == std::errc::resource_unavailable_try_again);
    auto duplex = co_await weave::as_result(pipe->flush());
    REQUIRE_FALSE(duplex);
    CHECK(duplex.error() == pg::Error::busy);

    REQUIRE(pipe->execute({"SELECT 2"}));
    co_await pipe->send();
    co_await std::move(*reader);
  });

  for (weave::u64 id = 1; id <= 2; ++id) {
    auto event = co_await pipe->next();
    REQUIRE(event);
    CHECK(event->id == id);
  }
  co_await pipe->receive();
  auto event = co_await pipe->next();
  REQUIRE(event);
  CHECK(event->id == 3);
  auto unfinished = pipe->finish();
  REQUIRE_FALSE(unfinished);
  CHECK(unfinished.error() == pg::Error::busy);

  REQUIRE(pipe->sync());
  co_await pipe->send();
  co_await pipe->receive();
  auto sync = co_await pipe->next();
  REQUIRE(sync);
  CHECK(sync->id == 4);
  CHECK(pipe->finish());
  CHECK(connection.open());
  CHECK(connection.close());
}

TEST_CASE("split send acknowledges bytes before responses and preserves later sync debt")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  CHECK(ctx->run(weave::timeout(5s, weave::when_all(split_peer(*listener), split_client(listener->local_port())))));
}

static weave::Task<void> split_large_consumer(pg::Pipeline &pipe)
{
  unsigned rows = 0;
  unsigned commands = 0;
  while (auto event = co_await pipe.next()) {
    if (event->kind == pg::PipelineKind::sync) {
      CHECK(event->complete);
      CHECK(event->id == 65);
      continue;
    }
    REQUIRE(event->outcome.result);
    if (event->complete) {
      CHECK(event->outcome.result->command == "SELECT 1");
      ++commands;
    } else {
      REQUIRE(event->outcome.result->rows.size() == 1);
      CHECK(event->outcome.result->rows.front().front().bytes().size() == 65536);
      ++rows;
    }
  }
  CHECK(rows == 64);
  CHECK(commands == 64);
}

static weave::Task<void> split_large_client(weave::u16 port)
{
  pg::Options options{.host = "127.0.0.1", .port = port, .user = "test", .plaintext = true};
  options.limits.message_bytes = 128 * 1024;
  options.limits.result_bytes = 8 * 1024 * 1024;
  options.limits.pipeline_commands = 65;
  auto connection = co_await pg::connect(options);
  auto pipe = connection.pipeline();
  REQUIRE(pipe);
  const std::string sql(65536, 'x');
  for (unsigned index = 0; index < 64; ++index)
    REQUIRE(pipe->execute({sql}, {.chunk_rows = 1}));
  REQUIRE(pipe->sync());

  co_await weave::scope([&](weave::TaskScope &tasks) -> weave::Task<void> {
    auto reader = tasks.spawn(pipe->receive());
    REQUIRE(reader);
    auto sender = tasks.spawn(pipe->send());
    REQUIRE(sender);
    co_await split_large_consumer(*pipe);
  });
  CHECK(pipe->finish());
  CHECK(connection.close());
}

TEST_CASE("split directions avoid full-duplex send receive deadlock")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  CHECK(ctx->run(
    weave::timeout(
      10s,
      weave::when_all(pipeline_backend(*listener, PipelinePeer::duplex), split_large_client(listener->local_port())))));
}

static weave::Task<void> waiting_pipeline(pg::Pipeline &pipeline)
{
  auto result = co_await weave::as_result(pipeline.next());
  REQUIRE_FALSE(result);
  CHECK(result.error() == pg::Error::closed);
}

static weave::Task<void> close_waiting_pipeline(pg::Connection &connection, pg::Pipeline &pipeline)
{
  co_await weave::sleep_for(1ms);
  auto competing = co_await weave::as_result(pipeline.next());
  REQUIRE_FALSE(competing);
  CHECK(competing.error() == pg::Error::busy);
  CHECK(connection.close());
}

static weave::Task<void> pipeline_lifetime(weave::u16 port)
{
  pg::Options options{.host = "127.0.0.1", .port = port, .user = "test", .plaintext = true};
  options.limits.pipeline_commands = 2;
  auto connection = co_await pg::connect(options);
  {
    auto deferred = connection.query("SELECT 1");
    auto rejected = connection.pipeline();
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error() == pg::Error::busy);
  }

  {
    auto pipeline = connection.pipeline();
    REQUIRE(pipeline);
    {
      auto deferred = pipeline->flush();
      CHECK_FALSE(pipeline->finish());
    }
    {
      auto deferred = pipeline->next();
      CHECK_FALSE(pipeline->finish());
    }
    {
      auto deferred = pipeline->send();
      CHECK_FALSE(pipeline->finish());
    }
    {
      auto deferred = pipeline->receive();
      CHECK_FALSE(pipeline->finish());
    }
    auto rejected = co_await weave::as_result(connection.reset(options));
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error() == pg::Error::busy);
    auto ordinary = co_await weave::as_result(connection.query("SELECT 1"));
    REQUIRE_FALSE(ordinary);
    CHECK(ordinary.error() == pg::Error::busy);
    auto terminate = co_await weave::as_result(connection.finish());
    REQUIRE_FALSE(terminate);
    CHECK(terminate.error() == pg::Error::busy);
    CHECK(connection.open());
    REQUIRE(pipeline->finish());
    auto finished = co_await weave::as_result(pipeline->flush());
    REQUIRE_FALSE(finished);
    CHECK(finished.error() == pg::Error::closed);
  }

  {
    auto pipeline = connection.pipeline();
    REQUIRE(pipeline);
    CHECK(pipeline->execute({"SELECT 1"}) == 1);
    CHECK(pipeline->sync() == 2);
    auto full = pipeline->execute({"SELECT 2"});
    REQUIRE_FALSE(full);
    CHECK(full.error() == pg::Error::resource_limit);
    auto unavailable = pipeline->try_next();
    REQUIRE_FALSE(unavailable);
    CHECK(unavailable.error() == std::errc::resource_unavailable_try_again);
    CHECK_FALSE(pipeline->finish());
  }
  CHECK(connection.open());

  auto pipeline = connection.pipeline();
  REQUIRE(pipeline);
  REQUIRE(pipeline->execute({"SELECT 1"}));
  co_await weave::when_all(waiting_pipeline(*pipeline), close_waiting_pipeline(connection, *pipeline));
}

TEST_CASE("PostgreSQL pipeline leases bounds completion and waiting-reader cleanup")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  CHECK(ctx->run(
    weave::when_all(pipeline_backend(*listener, PipelinePeer::lifetime), pipeline_lifetime(listener->local_port()))));
}

static weave::Task<void> early_pipeline_backend(weave::TcpListener &listener, weave::Channel<weave::u8> &done)
{
  auto socket = co_await listener.accept();
  std::array<std::byte, 4> header;
  co_await socket.read_exactly(header);
  wire::Reader reader{header};
  auto size = reader.integer();
  REQUIRE(size >= 8);
  REQUIRE(size <= 1024);
  wire::Bytes greeting(size - 4);
  co_await socket.read_exactly(greeting);

  wire::Writer response;
  wire::Writer authentication;
  authentication.integer(0);
  response.message('R', authentication);
  response.raw(pipeline_ready().bytes);
  co_await socket.write_all(response.bytes);
  CHECK(co_await pipeline_request(socket) == 'P');

  wire::Writer invalid;
  invalid.message('?');
  co_await socket.write_all(invalid.bytes);
  // Keep the socket open without draining the large request: failure must cancel the writer.
  CHECK(co_await done.receive());
}

enum class PipelinePump {
  batch,
  duplex,
  split
};

static weave::Task<void> early_pipeline_client(weave::u16 port, PipelinePump pump, weave::Channel<weave::u8> &done)
{
  auto connection = co_await pg::connect({.host = "127.0.0.1", .port = port, .user = "test", .plaintext = true});
  const std::string payload(8 * 1024 * 1024, 'x');
  std::error_code failure;
  if (pump != PipelinePump::batch) {
    auto pipeline = connection.pipeline();
    REQUIRE(pipeline);
    REQUIRE(pipeline->execute({"SELECT $1::text", {{payload}}}));
    REQUIRE(pipeline->sync());
    auto operation = pump == PipelinePump::split ? weave::when_all(pipeline->send(), pipeline->receive())
                                                 : pipeline->flush();
    auto result = co_await weave::as_result(std::move(operation));
    if (!result)
      failure = result.error();

    auto original = co_await weave::as_result(pipeline->next());
    REQUIRE_FALSE(original);
    CHECK(original.error() == pg::Error::protocol);
  } else {
    std::vector<pg::Command> commands{{"SELECT $1::text", {{payload}}}};
    auto result = co_await weave::as_result(connection.batch(std::move(commands)));
    if (!result)
      failure = result.error();
  }

  weave::u8 completed = 1;
  REQUIRE(done.try_send(completed));
  CHECK(failure == pg::Error::protocol);
  CHECK_FALSE(connection.open());
}

TEST_CASE("PostgreSQL duplex pumps preserve an early protocol failure rather than sibling cancellation")
{
  const std::array modes{PipelinePump::batch, PipelinePump::duplex, PipelinePump::split};
  for (auto pump : modes) {
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    weave::Channel<weave::u8> done{1};
    CHECK(ctx->run(
      weave::timeout(
        2s,
        weave::when_all(
          early_pipeline_backend(*listener, done),
          early_pipeline_client(listener->local_port(), pump, done)))));
  }
}
