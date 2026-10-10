#include <weave/postgres.hpp>
#include <weave/io.hpp>
#include <weave/scope.hpp>
#include <weave/timer.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include "wire.hpp"
#include <doctest/doctest.h>

namespace pg = weave::pg;
namespace wire = weave::pg::detail;
using namespace std::chrono_literals;

enum class StreamMode {
  rows,
  sql_error,
  partial_error,
  oversized_row,
  queue_cancellation,
  byte_cancellation,
  consumer_error
};

struct StreamCase {
  StreamMode mode;
  weave::u32 chunk_rows = 1;
  std::size_t result_bytes = 4096;
  weave::u32 capacity = 2;
  bool split = false;
};

static wire::Writer stream_ready()
{
  wire::Writer idle;
  idle.integer('I', 1);
  wire::Writer response;
  response.message('Z', idle);
  return response;
}

static weave::Task<void> stream_backend(weave::TcpListener &listener, StreamMode mode)
{
  auto socket = co_await listener.accept();
  std::array<std::byte, 4> size;
  co_await socket.read_exactly(size);
  wire::Reader startup{size};
  auto startup_size = startup.integer();
  REQUIRE(startup_size >= 8);
  REQUIRE(startup_size <= 1024);
  wire::Bytes greeting(startup_size - 4);
  co_await socket.read_exactly(greeting);

  wire::Writer response;
  wire::Writer authentication;
  authentication.integer(0);
  response.message('R', authentication);
  response.raw(stream_ready().bytes);
  co_await socket.write_all(response.bytes);

  for (;;) {
    std::array<std::byte, 5> header;
    co_await socket.read_exactly(header);
    wire::Reader message{header};
    auto kind = message.integer(1);
    auto message_size = message.integer();
    REQUIRE(message_size >= 4);
    REQUIRE(message_size <= 1024);
    wire::Bytes body(message_size - 4);
    co_await socket.read_exactly(body);
    if (kind == 'H')
      break;
  }

  response.bytes.clear();
  response.message('1');
  response.message('2');
  wire::Writer columns;
  columns.integer(1, 2);
  columns.string("value");
  columns.integer(0);
  columns.integer(0, 2);
  columns.integer(25);
  columns.integer(0xffff, 2);
  columns.integer(0xffffffff);
  columns.integer(0, 2);
  response.message('T', columns);

  const auto payload_size = mode == StreamMode::oversized_row ? 800 : 100;
  const std::string payload(payload_size, 'x');
  for (unsigned index = 0; index < 50; ++index) {
    wire::Writer row;
    row.integer(1, 2);
    row.integer(payload_size);
    row.raw(payload);
    response.message('D', row);
  }

  bool sql_error = mode == StreamMode::sql_error || mode == StreamMode::partial_error;
  if (sql_error) {
    wire::Writer diagnostic;
    diagnostic.integer('C', 1);
    diagnostic.string("22012");
    diagnostic.integer('M', 1);
    diagnostic.string("division by zero");
    diagnostic.integer(0, 1);
    response.message('E', diagnostic);
  } else {
    wire::Writer tag;
    tag.string("SELECT 50");
    response.message('C', tag);
  }
  response.raw(stream_ready().bytes);
  co_await socket.write_all(response.bytes);

  std::array<std::byte, 1> eof;
  static_cast<void>(co_await weave::as_result(socket.read(eof)));
}

static weave::Task<void> stream_consumer(pg::Pipeline &pipeline, const StreamCase &configuration)
{
  std::size_t rows = 0;
  unsigned completed = 0;
  std::optional<pg::ResultSet> retained;
  const std::string payload(100, 'x');
  bool sql_error = configuration.mode == StreamMode::sql_error || configuration.mode == StreamMode::partial_error;

  while (auto event = co_await pipeline.next()) {
    if (!event->complete) {
      CHECK(pg::status_name(*event) == "row_chunk");
      CHECK(event->id == 1);
      CHECK(event->kind == pg::PipelineKind::execute);
      REQUIRE(event->outcome.result);
      const auto &result = *event->outcome.result;
      REQUIRE(result.columns.size() == 1);
      CHECK(result.columns.front().name == "value");
      REQUIRE_FALSE(result.rows.empty());
      CHECK(result.rows.size() <= configuration.chunk_rows);
      CHECK(result.command.empty());
      for (const auto &row : result.rows) {
        REQUIRE(row.size() == 1);
        CHECK(row.front().bytes() == payload);
        ++rows;
      }

      if (configuration.capacity == 2) {
        auto rejected = pipeline.execute({"SELECT 3"});
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error() == pg::Error::resource_limit);
      }
      CHECK_FALSE(pipeline.finish());
      if (!retained)
        retained = std::move(event->outcome.result);
      co_await weave::sleep_for(1ms);
      continue;
    }

    CHECK(event->id == ++completed);
    if (event->id == 1) {
      if (sql_error) {
        CHECK(pg::status_name(*event) == "sql_error");
        CHECK_FALSE(event->outcome.result);
        CHECK(event->outcome.error.sqlstate() == "22012");
      } else {
        CHECK(pg::status_name(*event) == "tuples");
        REQUIRE(event->outcome.result);
        CHECK(event->outcome.result->rows.empty());
        CHECK(event->outcome.result->command == "SELECT 50");
        CHECK(event->outcome.result->columns.front().name == "value");
      }
    } else if (sql_error && event->id == 2) {
      CHECK(pg::status_name(*event) == "aborted");
      CHECK(event->outcome.aborted);
      CHECK_FALSE(event->outcome.result);
    } else {
      CHECK(pg::status_name(*event) == "pipeline_sync");
      CHECK(event->kind == pg::PipelineKind::sync);
      CHECK(event->transaction == pg::Transaction::idle);
    }
  }

  CHECK(rows == (configuration.mode == StreamMode::partial_error ? 49 : 50));
  CHECK(completed == (sql_error ? 3 : 2));
  REQUIRE(retained);
  CHECK(retained->columns.front().name == "value");
  CHECK(retained->rows.front().front().bytes() == payload);
}

static weave::Task<void> failing_stream_consumer(pg::Pipeline &pipeline)
{
  auto event = co_await pipeline.next();
  REQUIRE(event);
  CHECK_FALSE(event->complete);
  co_await weave::fail(std::errc::invalid_argument);
}

static weave::Task<void> stream_producer(pg::Pipeline &pipeline, bool split)
{
  if (split) {
    co_await pipeline.send();
    co_await pipeline.receive();
  } else {
    co_await pipeline.flush();
  }
}

static weave::Task<void> stream_client(weave::u16 port, StreamCase configuration)
{
  pg::Options options{.host = "127.0.0.1", .port = port, .user = "test", .plaintext = true};
  options.limits.message_bytes = 1024;
  options.limits.result_bytes = configuration.result_bytes;
  options.limits.pipeline_commands = configuration.capacity;
  auto connection = co_await pg::connect(options);
  auto pipeline = connection.pipeline();
  REQUIRE(pipeline);
  REQUIRE(pipeline->execute({"SELECT 1"}, {.chunk_rows = configuration.chunk_rows}));
  bool sql_error = configuration.mode == StreamMode::sql_error || configuration.mode == StreamMode::partial_error;
  if (sql_error)
    REQUIRE(pipeline->execute({"SELECT 99"}));
  REQUIRE(pipeline->sync());

  bool cancellation = configuration.mode == StreamMode::queue_cancellation ||
    configuration.mode == StreamMode::byte_cancellation;
  if (cancellation) {
    auto result = co_await weave::as_result(weave::timeout(20ms, stream_producer(*pipeline, configuration.split)));
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::timed_out);
    CHECK_FALSE(connection.open());

    unsigned chunks = 0;
    for (;;) {
      auto event = pipeline->try_next();
      if (!event) {
        CHECK(event.error() == std::errc::operation_canceled);
        break;
      }
      REQUIRE(*event);
      CHECK_FALSE((**event).complete);
      ++chunks;
    }
    if (configuration.mode == StreamMode::queue_cancellation)
      CHECK(chunks == configuration.capacity);
    else
      CHECK((chunks > 0 && chunks < configuration.capacity));
  } else if (configuration.mode == StreamMode::consumer_error) {
    auto result = co_await weave::as_result(weave::scope([&](weave::TaskScope &tasks) -> weave::Task<void> {
      auto producer = tasks.spawn(stream_producer(*pipeline, configuration.split));
      REQUIRE(producer);
      co_await failing_stream_consumer(*pipeline);
    }));
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::invalid_argument);
    CHECK_FALSE(connection.open());
  } else if (configuration.mode == StreamMode::oversized_row) {
    auto result = co_await weave::as_result(weave::timeout(1s, stream_producer(*pipeline, configuration.split)));
    REQUIRE_FALSE(result);
    CHECK(result.error() == pg::Error::resource_limit);
    CHECK_FALSE(connection.open());
  } else {
    co_await weave::when_all(
      stream_producer(*pipeline, configuration.split),
      stream_consumer(*pipeline, configuration));
    CHECK_FALSE(pipeline->aborted());
    CHECK(pipeline->finish());
  }
  CHECK(connection.close());
}

TEST_CASE("PostgreSQL pipeline chunks preserve ownership admission recovery and bounded progress")
{
  const std::array cases{
    StreamCase{StreamMode::rows, 1},
    StreamCase{StreamMode::rows, 37, 1024},
    StreamCase{StreamMode::sql_error, 1, 4096, 3},
    StreamCase{StreamMode::partial_error, 7, 4096, 3},
    StreamCase{StreamMode::oversized_row, 1, 1024},
    StreamCase{StreamMode::queue_cancellation},
    StreamCase{StreamMode::byte_cancellation, 1, 1024, 64},
    StreamCase{StreamMode::consumer_error},
    StreamCase{StreamMode::rows, 1, 4096, 2, true},
    StreamCase{StreamMode::rows, 37, 1024, 2, true},
    StreamCase{StreamMode::sql_error, 1, 4096, 3, true},
    StreamCase{StreamMode::partial_error, 7, 4096, 3, true},
    StreamCase{StreamMode::oversized_row, 1, 1024, 2, true},
    StreamCase{StreamMode::queue_cancellation, 1, 4096, 2, true},
    StreamCase{StreamMode::byte_cancellation, 1, 1024, 64, true},
    StreamCase{StreamMode::consumer_error, 1, 4096, 2, true}};

  for (const auto &configuration : cases) {
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    auto result = ctx->run(
      weave::timeout(
        5s,
        weave::when_all(
          stream_backend(*listener, configuration.mode),
          stream_client(listener->local_port(), configuration))));
    CHECK(result);
  }
}

static weave::Task<void> abandoned_pipeline(weave::u16 port)
{
  pg::Options options{.host = "127.0.0.1", .port = port, .user = "test", .plaintext = true};
  options.limits.message_bytes = 1024;
  options.limits.result_bytes = 16384;
  auto connection = co_await pg::connect(options);
  {
    auto pipeline = connection.pipeline();
    REQUIRE(pipeline);
    REQUIRE(pipeline->execute({"SELECT 1"}));
    REQUIRE(pipeline->sync());
    co_await pipeline->flush();
    // Discard queued results after acknowledged Sync, without consuming them.
  }
  CHECK(connection.open());
  CHECK(connection.close());
}

TEST_CASE("PostgreSQL pipeline destruction releases unconsumed deliveries before their budget owner")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  CHECK(ctx->run(
    weave::timeout(
      5s,
      weave::when_all(stream_backend(*listener, StreamMode::rows), abandoned_pipeline(listener->local_port())))));
}

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
static weave::Task<void> runtime_stream(StreamCase configuration)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
  co_await weave::timeout(
    5s,
    weave::when_all(stream_backend(listener, configuration.mode), stream_client(listener.local_port(), configuration)));
}

TEST_CASE("PostgreSQL pipeline chunks and cancellation drain across four-worker schedulers and IO layouts")
{
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
#if defined(_WIN32)
  const std::array layouts{weave::IoLayout::sharded, weave::IoLayout::shared};
#else
  const std::array layouts{weave::IoLayout::sharded};
#endif
  const std::array cases{
    StreamCase{StreamMode::rows, 37, 1024},
    StreamCase{StreamMode::partial_error, 7, 4096, 3},
    StreamCase{StreamMode::queue_cancellation},
    StreamCase{StreamMode::byte_cancellation, 1, 1024, 64},
    StreamCase{StreamMode::consumer_error}};

  for (auto scheduler : schedulers) {
    for (auto layout : layouts) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
      REQUIRE(runtime);
      std::vector<weave::JoinHandle<void>> jobs;
      for (unsigned index = 0; index < 16; ++index) {
        auto configuration = cases[index % cases.size()];
        configuration.split = index >= 8;
        auto job = runtime->spawn(runtime_stream(configuration));
        REQUIRE(job);
        jobs.push_back(std::move(*job));
      }
      for (auto &job : jobs)
        CHECK(std::move(job).get());
    }
  }
}
#endif
