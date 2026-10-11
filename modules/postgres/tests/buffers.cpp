#include <weave/postgres.hpp>
#include <weave/io.hpp>
#include <weave/timer.hpp>
#include "wire.hpp"
#include <doctest/doctest.h>

namespace pg = weave::pg;
namespace wire = weave::pg::detail;

TEST_CASE("Borrowed PostgreSQL parameters preserve owning Bind encoding and validation")
{
  const std::string binary{"a\0b", 3};
  const std::vector<pg::Parameter> owning{{""}, {std::nullopt}, {binary, 17, pg::Format::binary}};
  const std::array borrowed{
    pg::ParameterView{.data = ""},
    pg::ParameterView{},
    pg::ParameterView{.data = binary, .type = 17, .format = pg::Format::binary}};
  wire::Writer expected, actual;
  auto encoded = wire::bind(expected, "statement", owning, pg::Format::binary, 1024);
  auto viewed = wire::bind(actual, "statement", borrowed, pg::Format::binary, 1024);
  REQUIRE(encoded);
  REQUIRE(viewed);
  CHECK(actual.bytes == expected.bytes);

  auto reject = [&](const auto &parameters, std::size_t limit, std::error_code expected) {
    wire::Writer request;
    auto result = wire::bind(request, "", parameters, pg::Format::text, limit);
    CHECK_FALSE(result);
    if (!result)
      CHECK(result.error() == expected);
  };
  auto invalid = borrowed;
  invalid.back().format = pg::Format::text;
  reject(invalid, 1024, std::make_error_code(std::errc::invalid_argument));
  reject(borrowed, 8, pg::make_error_code(pg::Error::resource_limit));
  invalid.back().format = static_cast<pg::Format>(7);
  reject(invalid, 1024, std::make_error_code(std::errc::invalid_argument));
}

enum class BufferBackend {
  normal,
  canceled,
  malformed,
  excessive,
  truncated,
  duplex,
  sql_failure
};

static weave::Task<char> consume_frame(weave::TcpStream &stream, bool startup = false)
{
  std::array<std::byte, 5> header;
  co_await stream.read_exactly(std::span{header}.first(startup ? 4 : 5));
  wire::Reader reader{header};
  char kind = startup ? char{} : static_cast<char>(reader.integer(1));
  auto size = reader.integer();
  if (size < 4 || size > 4096)
    co_await weave::fail(std::errc::bad_message);
  wire::Bytes body(size - 4);
  co_await stream.read_exactly(body);
  co_return kind;
}

static wire::Writer ready_message()
{
  wire::Writer response, body;
  body.integer('I', 1);
  response.message('Z', body);
  return response;
}

static weave::Task<void> buffer_backend(weave::TcpListener &listener, BufferBackend mode)
{
  auto stream = co_await listener.accept({.no_delay = true});
  co_await consume_frame(stream, true);
  wire::Writer startup, authenticated;
  authenticated.integer(0);
  startup.message('R', authenticated);
  startup.raw(ready_message().bytes);
  co_await stream.write_all(startup.bytes);
  if (co_await consume_frame(stream) != 'Q')
    co_await weave::fail(std::errc::bad_message);

  wire::Writer copying, format;
  format.integer(0, 1);
  format.integer(0, 2);
  copying.message(mode == BufferBackend::duplex ? 'W' : 'H', format);
  co_await stream.write_all(copying.bytes);
  wire::Writer response;
  if (mode == BufferBackend::malformed || mode == BufferBackend::excessive) {
    response.integer('d', 1);
    response.integer(mode == BufferBackend::malformed ? 3 : 2048);
  } else if (mode == BufferBackend::canceled || mode == BufferBackend::truncated) {
    response.integer('d', 1);
    response.integer(20);
    response.raw("abc");
  } else {
    wire::Writer body;
    body.raw("abcdefgh");
    response.message('d', body);
    if (mode == BufferBackend::normal)
      response.message('d');
  }
  co_await stream.write_all(response.bytes);

  if (mode == BufferBackend::canceled || mode == BufferBackend::malformed || mode == BufferBackend::excessive) {
    std::array<std::byte, 1> byte;
    static_cast<void>(co_await stream.read(byte));
    co_return;
  }
  if (mode == BufferBackend::truncated)
    co_return;
  if (mode == BufferBackend::duplex && co_await consume_frame(stream) != 'c')
    co_await weave::fail(std::errc::bad_message);

  wire::Writer terminal;
  if (mode == BufferBackend::sql_failure) {
    wire::Writer error;
    error.integer('S', 1);
    error.string("ERROR");
    error.integer('C', 1);
    error.string("22012");
    error.integer('M', 1);
    error.string("COPY failed");
    error.integer(0, 1);
    terminal.message('E', error);
  } else {
    terminal.message('c');
    wire::Writer command;
    command.string(mode == BufferBackend::duplex ? "START_REPLICATION" : "COPY 2");
    terminal.message('C', command);
  }
  terminal.raw(ready_message().bytes);
  co_await stream.write_all(terminal.bytes);
  if (mode == BufferBackend::duplex)
    co_return;
  if (co_await consume_frame(stream) != 'X')
    co_await weave::fail(std::errc::bad_message);
}

static weave::Task<void> buffer_client(weave::u16 port, BufferBackend mode)
{
  auto database = co_await pg::connect(
    {.host = "127.0.0.1",
      .port = port,
      .user = "weave",
      .plaintext = true,
      .limits = {.message_bytes = 1024},
      .max_protocol = pg::ProtocolVersion::v30});
  co_await database.start_copy("COPY test TO STDOUT");
  std::array<std::byte, 3> buffer;
  if (mode == BufferBackend::malformed || mode == BufferBackend::excessive || mode == BufferBackend::truncated) {
    auto result = co_await weave::as_result(database.read_copy(buffer));
    // The truncated backend supplies one whole three-byte chunk, then closes mid-message.
    if (mode == BufferBackend::truncated) {
      CHECK((result && *result && (*result)->size == 3 && !(*result)->end_of_message));
      result = co_await weave::as_result(database.read_copy(buffer));
    }
    CHECK_FALSE(result);
    if (result)
      co_return;
    auto expected = mode == BufferBackend::malformed ? pg::make_error_code(pg::Error::protocol)
      : mode == BufferBackend::excessive             ? pg::make_error_code(pg::Error::resource_limit)
                                                     : std::make_error_code(std::errc::connection_reset);
    CHECK(result.error() == expected);
    CHECK_FALSE(database.open());
    co_return;
  }
  if (mode == BufferBackend::canceled) {
    auto first = co_await database.read_copy(buffer);
    CHECK((first && first->size == 3 && !first->end_of_message));
    auto result = co_await weave::as_result(weave::timeout(std::chrono::milliseconds{20}, database.read_copy(buffer)));
    CHECK_FALSE(result);
    if (result)
      co_return;
    CHECK(result.error() == std::errc::timed_out);
    CHECK_FALSE(database.open());
    co_return;
  }
  if (mode == BufferBackend::duplex) {
    auto first = co_await database.read_copy(buffer);
    CHECK((first && first->size == 3 && !first->end_of_message));
    auto result = co_await database.end_copy();
    CHECK(result.command == "START_REPLICATION");
    CHECK(database.open());
    co_return;
  }

  std::string message;
  std::size_t complete = 0;
  for (;;) {
    auto result = co_await weave::as_result(database.read_copy(buffer));
    if (!result) {
      CHECK(mode == BufferBackend::sql_failure);
      CHECK(pg::sqlstate(result.error()) == "22012");
      CHECK(database.open());
      break;
    }
    if (!*result)
      break;
    auto read = **result;
    message.append(reinterpret_cast<const char *>(buffer.data()), read.size);
    if (read.end_of_message) {
      CHECK(message == (complete == 0 ? "abcdefgh" : ""));
      message.clear();
      ++complete;
    }
  }
  CHECK(complete == (mode == BufferBackend::normal ? 2 : 1));
  co_await database.finish();
}

static weave::Task<void> exercise_buffers(weave::Context &ctx, BufferBackend mode)
{
  auto listener = weave::tcp::listen(ctx, "127.0.0.1", 0);
  if (!listener)
    co_await weave::fail(listener.error());
  auto server = ctx.spawn(buffer_backend(*listener, mode));
  if (!server)
    co_await weave::fail(server.error());
  co_await buffer_client(listener->local_port(), mode);
  co_await std::move(*server);
}

TEST_CASE("PostgreSQL caller COPY buffers retain framing, recovery and cancellation contracts")
{
  const std::array modes{
    BufferBackend::normal,
    BufferBackend::canceled,
    BufferBackend::malformed,
    BufferBackend::excessive,
    BufferBackend::truncated,
    BufferBackend::duplex,
    BufferBackend::sql_failure};
  for (auto mode : modes) {
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    if (!ctx)
      continue;
    CHECK(ctx->run(weave::timeout(std::chrono::seconds{5}, exercise_buffers(*ctx, mode))));
  }
}
