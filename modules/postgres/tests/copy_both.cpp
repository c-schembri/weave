#include <weave/postgres.hpp>
#include <weave/io.hpp>
#include <weave/timer.hpp>
#include "wire.hpp"
#include <doctest/doctest.h>

namespace pg = weave::pg;
namespace wire = weave::pg::detail;
using namespace std::chrono_literals;

static weave::Task<wire::Bytes> startup(weave::TcpStream &socket)
{
  std::array<std::byte, 4> header;
  co_await socket.read_exactly(header);
  wire::Reader reader{header};
  auto size = reader.integer();
  if (size < 8 || size > 1024)
    co_await weave::fail(std::errc::bad_message);

  wire::Bytes body(size - 4);
  co_await socket.read_exactly(body);
  co_return body;
}

static weave::Task<char> request(weave::TcpStream &socket)
{
  std::array<std::byte, 5> header;
  co_await socket.read_exactly(header);
  wire::Reader reader{header};
  auto kind = static_cast<char>(reader.integer(1));
  auto size = reader.integer();
  if (size < 4 || size > 1024)
    co_await weave::fail(std::errc::bad_message);

  wire::Bytes body(size - 4);
  co_await socket.read_exactly(body);
  co_return kind;
}

static wire::Writer ready()
{
  wire::Writer body;
  body.integer('I', 1);
  wire::Writer response;
  response.message('Z', body);
  return response;
}

static wire::Writer answer()
{
  wire::Writer response;
  wire::Writer columns;
  columns.integer(1, 2);
  columns.string("value");
  columns.integer(0);
  columns.integer(0, 2);
  columns.integer(23);
  columns.integer(4, 2);
  columns.integer(0xffffffff);
  columns.integer(0, 2);
  response.message('T', columns);
  wire::Writer row;
  row.integer(1, 2);
  row.integer(2);
  row.raw("42");
  response.message('D', row);
  wire::Writer command;
  command.string("SELECT 1");
  response.message('C', command);
  response.raw(ready().bytes);
  return response;
}

static weave::Task<void> copy_writer(weave::TcpStream &socket)
{
  wire::Writer body;
  body.bytes.assign(1024 * 1024, std::byte{'x'});
  wire::Writer chunk;
  chunk.message('d', body);
  for (unsigned index = 0; index < 4; ++index)
    co_await socket.write_all(chunk.bytes);
  wire::Writer done;
  done.message('c');
  co_await socket.write_all(done.bytes);
}

static weave::Task<void> copy_receiver(weave::TcpStream &socket)
{
  for (unsigned index = 0; index < 4; ++index) {
    std::array<std::byte, 5> header;
    co_await socket.read_exactly(header);
    wire::Reader reader{header};
    CHECK(reader.integer(1) == 'd');
    auto size = reader.integer();
    CHECK(size == 1024 * 1024 + 4);
    wire::Bytes data(size - 4);
    co_await socket.read_exactly(data);
    CHECK(data.front() == std::byte{'y'});
  }
  CHECK(co_await request(socket) == 'c');
}

static weave::Task<void> duplex_backend(weave::TcpListener &listener)
{
  auto socket = co_await listener.accept({.no_delay = true});
  static_cast<void>(co_await startup(socket));
  wire::Writer response;
  wire::Writer authentication;
  authentication.integer(0);
  response.message('R', authentication);
  response.raw(ready().bytes);
  co_await socket.write_all(response.bytes);
  CHECK(co_await request(socket) == 'Q');
  wire::Writer format;
  format.integer(1, 1);
  format.integer(0, 2);
  wire::Writer start;
  start.message('W', format);
  co_await socket.write_all(start.bytes);
  co_await weave::when_all(copy_writer(socket), copy_receiver(socket));
  wire::Writer completion;
  wire::Writer tag;
  tag.string("COPY 0");
  completion.message('C', tag);
  tag.bytes.clear();
  tag.string("START_REPLICATION");
  completion.message('C', tag);
  completion.raw(ready().bytes);
  co_await socket.write_all(completion.bytes);
  CHECK(co_await request(socket) == 'X');
}

static weave::Task<void> duplex_send(pg::Connection &connection)
{
  wire::Bytes bytes(1024 * 1024, std::byte{'y'});
  co_await weave::when_all(
    connection.write_copy(bytes),
    connection.write_copy(bytes),
    connection.write_copy(bytes),
    connection.write_copy(bytes));
  co_await connection.finish_copy_send();
}

static weave::Task<void> duplex_read(pg::Connection &connection)
{
  std::size_t bytes = 0;
  while (auto data = co_await connection.read_copy())
    bytes += data->size();
  CHECK(bytes == 4 * 1024 * 1024);
}

static weave::Task<void> duplex_client(weave::u16 port)
{
  auto connection = co_await pg::connect({.host = "127.0.0.1", .port = port, .user = "test", .plaintext = true});
  auto format = co_await connection.start_copy("START_REPLICATION");
  CHECK(format.direction == pg::CopyDirection::both);
  {
    auto deferred = connection.finish_copy_send();
    pg::Options options;
    options.user = "test";
    options.plaintext = true;
    auto reset = co_await weave::as_result(connection.reset(options));
    CHECK_FALSE(reset);
    CHECK(reset.error() == pg::make_error_code(pg::Error::busy));
    auto finish = co_await weave::as_result(connection.finish());
    CHECK_FALSE(finish);
    CHECK(finish.error() == pg::make_error_code(pg::Error::busy));
  }
  co_await weave::when_all(duplex_send(connection), duplex_read(connection));
  co_await connection.end_copy();
  CHECK(connection.copy_results()->size() == 2);
  co_await connection.finish();
}

TEST_CASE("PostgreSQL COPY BOTH progresses both directions with queued writers")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);

  auto result = ctx->run(
    weave::timeout(10s, weave::when_all(duplex_backend(*listener), duplex_client(listener->local_port()))));
  REQUIRE(result);
}

enum class CopyProbe {
  malformed,
  error,
  after_done,
  deadline,
  timeline
};

static weave::Task<void> probe_backend(weave::TcpListener &listener, CopyProbe mode)
{
  auto socket = co_await listener.accept({.no_delay = true});
  static_cast<void>(co_await startup(socket));
  wire::Writer response;
  wire::Writer authentication;
  authentication.integer(0);
  response.message('R', authentication);
  response.raw(ready().bytes);
  co_await socket.write_all(response.bytes);
  CHECK(co_await request(socket) == 'Q');
  wire::Writer format;
  format.integer(mode == CopyProbe::malformed ? 2 : 0, 1);
  format.integer(0, 2);
  wire::Writer start;
  start.message('W', format);
  co_await socket.write_all(start.bytes);

  if (mode == CopyProbe::error) {
    wire::Writer diagnostic;
    diagnostic.integer('C', 1);
    diagnostic.string("XX000");
    diagnostic.integer('M', 1);
    diagnostic.string("failed stream");
    diagnostic.integer(0, 1);
    wire::Writer failed;
    failed.message('E', diagnostic);
    co_await socket.write_all(failed.bytes);
  } else if (mode == CopyProbe::after_done) {
    wire::Writer ended;
    ended.message('c');
    co_await socket.write_all(ended.bytes);
    CHECK(co_await request(socket) == 'd');
    CHECK(co_await request(socket) == 'c');
    wire::Writer invalid;
    invalid.message('d');
    co_await socket.write_all(invalid.bytes);
  } else if (mode == CopyProbe::timeline) {
    wire::Writer ended;
    ended.message('c');
    co_await socket.write_all(ended.bytes);
    CHECK(co_await request(socket) == 'c');
    auto completion = answer();
    completion.bytes.resize(completion.bytes.size() - ready().bytes.size());
    wire::Writer tag;
    tag.string("START_REPLICATION");
    completion.message('C', tag);
    completion.raw(ready().bytes);
    co_await socket.write_all(completion.bytes);
    CHECK(co_await request(socket) == 'X');
    co_return;
  }
  std::array<std::byte, 1> byte;
  auto closed = co_await weave::as_result(socket.read(byte));
  CHECK((!closed || *closed == 0));
}

static weave::Task<void> probe_client(weave::u16 port, CopyProbe mode)
{
  auto connection = co_await pg::connect({.host = "127.0.0.1", .port = port, .user = "test", .plaintext = true});
  auto started = co_await weave::as_result(connection.start_copy("START_REPLICATION"));
  if (mode == CopyProbe::malformed) {
    CHECK_FALSE(started);
    CHECK(started.error() == pg::make_error_code(pg::Error::protocol));
  } else if (mode == CopyProbe::deadline) {
    auto read = co_await weave::as_result(weave::timeout(10ms, connection.read_copy()));
    CHECK_FALSE(read);
    CHECK(read.error() == std::errc::timed_out);
  } else if (mode == CopyProbe::error) {
    auto read = co_await weave::as_result(connection.read_copy());
    CHECK_FALSE(read);
    CHECK(pg::sqlstate(read.error()) == "XX000");
    CHECK(connection.last_error().message() == "failed stream");
  } else {
    CHECK_FALSE(co_await connection.read_copy());
    CHECK_FALSE(connection.copy_results());
    if (mode == CopyProbe::after_done) {
      std::array data{std::byte{'x'}};
      co_await connection.write_copy(data);
    }
    auto completed = co_await weave::as_result(connection.end_copy());
    if (mode == CopyProbe::timeline) {
      CHECK(completed.has_value());
      CHECK(connection.copy_results()->size() == 2);
      CHECK(connection.copy_results()->front().rows.front().front().bytes() == "42");
      co_await connection.finish();
      co_return;
    }
    CHECK_FALSE(completed);
    CHECK(completed.error() == pg::make_error_code(pg::Error::protocol));
  }
  CHECK_FALSE(connection.open());
}

TEST_CASE("PostgreSQL COPY BOTH validates framing, half-close and terminal failure")
{
  const std::array
    modes{CopyProbe::malformed, CopyProbe::error, CopyProbe::after_done, CopyProbe::deadline, CopyProbe::timeline};
  for (auto mode : modes) {
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    auto result = ctx->run(
      weave::timeout(2s, weave::when_all(probe_backend(*listener, mode), probe_client(listener->local_port(), mode))));
    if (!result)
      std::fprintf(stderr, "Hostile: %s\n", result.error().message().c_str());
    REQUIRE(result);
  }
}
