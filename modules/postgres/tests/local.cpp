#include <weave/postgres.hpp>
#include <weave/local.hpp>
#include <weave/timer.hpp>
#include "wire.hpp"
#include <doctest/doctest.h>
#include <filesystem>
#include <random>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace pg = weave::pg;
namespace wire = weave::pg::detail;
using namespace std::chrono_literals;

struct LocalDatabase {
  std::filesystem::path directory;
  std::vector<std::filesystem::path> paths;

  LocalDatabase()
  {
    std::error_code error;
    auto root = std::filesystem::temp_directory_path(error);
    REQUIRE_FALSE(error);
    for (int attempt = 0; attempt < 100; ++attempt) {
      directory = root / ("weave-pg-" + std::to_string(std::random_device{}()));
      if (std::filesystem::create_directory(directory, error))
        return;
      REQUIRE_FALSE(error);
    }
    FAIL("Unable to create a private local PostgreSQL fixture");
  }

  ~LocalDatabase()
  {
    std::error_code ignored;
    for (const auto &path : paths)
      std::filesystem::remove(path / ".s.PGSQL.5432", ignored);
    for (const auto &path : paths)
      std::filesystem::remove(path, ignored);
    std::filesystem::remove(directory, ignored);
  }

  std::string host()
  {
    auto path = directory / std::to_string(paths.size());
    std::error_code error;
    REQUIRE(std::filesystem::create_directory(path, error));
    REQUIRE_FALSE(error);
    paths.push_back(path);
    auto bytes = path.generic_u8string();
    return {bytes.begin(), bytes.end()};
  }
};

static weave::Task<std::vector<std::byte>> local_packet(weave::LocalStream &stream, bool typed = false)
{
  std::array<std::byte, 5> header;
  co_await stream.read_exactly(std::span{header}.first(typed ? 5 : 4));
  wire::Reader size{std::span{header}.subspan(typed ? 1 : 0, 4)};
  auto length = size.integer();
  if (length < 4 || length > 4096)
    co_await weave::fail(std::errc::bad_message);

  std::vector<std::byte> bytes(length - 4);
  co_await stream.read_exactly(bytes);
  if (typed)
    bytes.insert(bytes.begin(), header[0]);
  co_return bytes;
}

static wire::Writer local_ready()
{
  wire::Writer body;
  body.integer('I', 1);
  wire::Writer response;
  response.message('Z', body);
  return response;
}

static wire::Writer local_answer()
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
  response.raw(local_ready().bytes);
  return response;
}

static weave::Task<void> local_client(std::string host, pg::ProtocolVersion version)
{
  pg::Options options{.host = host, .user = "test", .plaintext = true};
  options.min_protocol = options.max_protocol = version;
  options.hosts = {{.name = host + "missing"}, {.name = host}};
#if !defined(_WIN32)
  options.required_peer_user = geteuid();
#endif
  auto connection = co_await pg::connect(std::move(options));
  CHECK(connection.protocol_version() == version);
  auto results = co_await connection.query("SELECT 42");
  CHECK(results.size() == 1);
  CHECK(results.front().rows.front().front().integer<int>() == 42);
  auto handle = connection.cancel_handle();
  if (!handle)
    co_await weave::fail(handle.error());
  co_await connection.finish();
  CHECK_FALSE(connection.open());
  co_await handle->request();
}

static weave::Task<void> local_exchange(weave::Context &ctx, std::string host, pg::ProtocolVersion version)
{
  auto listener = co_await weave::local::listen(host + "/.s.PGSQL.5432");
  auto job = ctx.spawn(local_client(host, version));
  if (!job)
    co_await weave::fail(job.error());

  auto stream = co_await listener.accept();
  auto startup = co_await local_packet(stream);
  wire::Reader reader{startup};
  CHECK(reader.integer() == static_cast<weave::u32>(version));
  wire::Writer response;
  wire::Writer authentication;
  authentication.integer(0);
  response.message('R', authentication);
  wire::Writer key;
  key.integer(42);
  key.integer(73);
  if (version == pg::ProtocolVersion::v32)
    key.integer(74);
  response.message('K', key);
  response.raw(local_ready().bytes);
  co_await stream.write_all(response.bytes);
  auto query = co_await local_packet(stream, true);
  CHECK(query.front() == std::byte{'Q'});
  co_await stream.write_all(local_answer().bytes);
  auto terminate = co_await local_packet(stream, true);
  CHECK(terminate.size() == 1);
  CHECK(terminate.front() == std::byte{'X'});
  if (auto closed = stream.close(); !closed)
    co_await weave::fail(closed.error());

  auto cancellation = co_await listener.accept();
  auto bytes = co_await local_packet(cancellation);
  wire::Reader cancel{bytes};
  CHECK(cancel.integer() == 80877102);
  CHECK(cancel.integer() == 42);
  CHECK(cancel.integer() == 73);
  if (version == pg::ProtocolVersion::v32)
    CHECK(cancel.integer() == 74);
  CHECK(cancel.empty());
  if (auto shutdown = cancellation.shutdown_send(); !shutdown)
    co_await weave::fail(shutdown.error());
  co_await std::move(*job);
}

static weave::Task<void> local_startup_cancel(weave::Context &ctx, std::string host)
{
  auto listener = co_await weave::local::listen(host + "/.s.PGSQL.5432");
  pg::Options options{.host = host, .user = "test", .plaintext = true};
  auto job = ctx.spawn(pg::connect(std::move(options)));
  if (!job)
    co_await weave::fail(job.error());
  auto stream = co_await listener.accept();
  auto startup = co_await local_packet(stream);
  CHECK_FALSE(startup.empty());
  job->cancel();
  std::array<std::byte, 1> byte;
  CHECK(co_await stream.read(byte) == 0);
  auto result = co_await weave::as_result(std::move(*job));
  CHECK_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
}

#if !defined(_WIN32)
static weave::Task<void> local_wrong_peer(weave::Context &ctx, std::string host)
{
  auto listener = co_await weave::local::listen(host + "/.s.PGSQL.5432");
  pg::Options options{.host = host, .user = "test", .plaintext = true};
  options.hosts = {{.name = host}, {.name = host + "fallback"}};
  options.required_peer_user = static_cast<weave::u64>(geteuid()) + 1;
  auto job = ctx.spawn(pg::connect(std::move(options)));
  if (!job)
    co_await weave::fail(job.error());
  auto stream = co_await listener.accept();
  std::array<std::byte, 1> byte;
  CHECK(co_await stream.read(byte) == 0);
  auto result = co_await weave::as_result(std::move(*job));
  CHECK_FALSE(result);
  CHECK(result.error() == pg::Error::authentication);
}
#endif

TEST_CASE("PostgreSQL local policy rejects downgrade, hostaddr ambiguity and unsupported controls before submission")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  pg::Options options{.host = "/not-submitted", .user = "test"};
  auto secure = ctx->run(pg::connect(options));
  CHECK((!secure && secure.error() == std::errc::operation_not_supported));
  options.plaintext = true;
  options.hosts = {{.name = options.host, .address = weave::IpAddress::loopback_v4()}};
  auto ambiguous = ctx->run(pg::connect(options));
  CHECK((!ambiguous && ambiguous.error() == std::errc::invalid_argument));
  options.hosts.clear();
  options.keep_alive.idle = 1s;
  auto tuning = ctx->run(pg::connect(options));
  CHECK((!tuning && tuning.error() == std::errc::operation_not_supported));
  options.keep_alive.idle = 0s;
  options.required_peer_user = 0;
#if defined(_WIN32)
  auto peer = ctx->run(pg::connect(options));
  CHECK((!peer && peer.error() == std::errc::operation_not_supported));
#else
  options.host = "127.0.0.1";
  auto peer = ctx->run(pg::connect(options));
  CHECK((!peer && peer.error() == std::errc::invalid_argument));
  options.host = "/not-submitted";
  options.required_peer_user = weave::u64{1} << 32;
  auto overflow = ctx->run(pg::connect(options));
  CHECK((!overflow && overflow.error() == std::errc::invalid_argument));
#endif
  CHECK(ctx->metrics().submitted == 0);
}

TEST_CASE("PostgreSQL local protocol, failover and retained cancellation preserve paths and key versions")
{
  const std::array versions{pg::ProtocolVersion::v30, pg::ProtocolVersion::v32};
  const std::array skip_modes{false, true};
  for (auto skip : skip_modes) {
    LocalDatabase fixture;
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);
    for (auto version : versions) {
      CAPTURE(skip);
      CAPTURE(version);
      auto result = ctx->run(weave::timeout(5s, local_exchange(*ctx, fixture.host(), version)));
      CAPTURE(result ? 0 : result.error().value());
      REQUIRE(result);
#if !defined(_WIN32)
      auto host = "@weave-pg-" + std::to_string(getpid()) + "-" + std::to_string(std::random_device{}());
      result = ctx->run(weave::timeout(5s, local_exchange(*ctx, host, version)));
      REQUIRE(result);
#endif
    }
    CHECK(ctx->metrics().submitted == ctx->metrics().completed);
  }
}

TEST_CASE("PostgreSQL local startup cancellation drains and a rejected peer receives no startup bytes")
{
  LocalDatabase fixture;
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto cancelled = ctx->run(weave::timeout(5s, local_startup_cancel(*ctx, fixture.host())));
  REQUIRE(cancelled);
#if !defined(_WIN32)
  auto rejected = ctx->run(weave::timeout(5s, local_wrong_peer(*ctx, fixture.host())));
  REQUIRE(rejected);
#endif
  CHECK(ctx->metrics().submitted == ctx->metrics().completed);
}

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
TEST_CASE("PostgreSQL local connections and cancellation retain affinity under four-worker execution")
{
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
#if defined(_WIN32)
  const std::array layouts{weave::IoLayout::sharded, weave::IoLayout::shared};
#else
  const std::array layouts{weave::IoLayout::sharded};
#endif
  const std::array skip_modes{false, true};
  for (auto scheduler : schedulers) {
    for (auto layout : layouts) {
      for (auto skip : skip_modes) {
        CAPTURE(scheduler);
        CAPTURE(layout);
        CAPTURE(skip);
        LocalDatabase fixture;
        auto runtime = weave::Runtime::create(
          {.workers = 4,
            .scheduler = scheduler,
            .context = {.skip_successful_completions = skip},
            .io_layout = layout});
        REQUIRE(runtime);
        std::vector<weave::JoinHandle<void>> jobs;
        for (int index = 0; index < 16; ++index) {
          auto host = fixture.host();
          auto version = index % 2 == 0 ? pg::ProtocolVersion::v30 : pg::ProtocolVersion::v32;
          auto job = runtime->spawn([host = std::move(host), version](weave::Context &ctx) {
            return weave::timeout(5s, local_exchange(ctx, host, version));
          });
          REQUIRE(job);
          jobs.push_back(std::move(*job));
        }
        for (auto &job : jobs) {
          auto result = std::move(job).get();
          CAPTURE(result ? 0 : result.error().value());
          CHECK(result);
        }
      }
    }
  }
}
#endif
