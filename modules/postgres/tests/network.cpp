#include <weave/postgres.hpp>
#include <weave/io.hpp>
#include <weave/timer.hpp>
#include "wire.hpp"
#include <doctest/doctest.h>

namespace pg = weave::pg;
namespace wire = weave::pg::detail;
using namespace std::chrono_literals;

enum class Backend {
  recovery,
  malformed,
  excessive,
  cancellation,
  incomplete,
  duplicate_description,
  no_description,
  function_missing,
  function_truncated,
  function_duplicate,
  function_recovery
};

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

static weave::Task<void> backend(weave::TcpListener &listener, Backend mode)
{
  auto socket = co_await listener.accept({.no_delay = true});
  auto greeting = co_await startup(socket);
  wire::Reader reader{greeting};
  CHECK(reader.integer() == 196610);

  wire::Writer response;
  wire::Writer version;
  version.integer(0);
  version.integer(0);
  response.message('v', version);
  wire::Writer authentication;
  authentication.integer(0);
  response.message('R', authentication);
  wire::Writer key;
  key.integer(42);
  key.integer(73);
  response.message('K', key);
  response.raw(ready().bytes);
  co_await socket.write_all(response.bytes);

  bool function = mode == Backend::function_missing || mode == Backend::function_truncated ||
    mode == Backend::function_duplicate || mode == Backend::function_recovery;
  CHECK(co_await request(socket) == (function ? 'F' : 'Q'));
  if (mode == Backend::malformed || mode == Backend::excessive) {
    wire::Writer invalid;
    invalid.integer('D', 1);
    invalid.integer(mode == Backend::malformed ? 3 : 32 * 1024 * 1024);
    co_await socket.write_all(invalid.bytes);
  } else if (mode == Backend::incomplete || mode == Backend::duplicate_description || mode == Backend::no_description) {
    wire::Writer empty_columns;
    empty_columns.integer(0, 2);
    wire::Writer invalid;
    if (mode == Backend::no_description) {
      invalid.message('D', empty_columns);
    } else {
      invalid.message('T', empty_columns);
      if (mode == Backend::duplicate_description)
        invalid.message('T', empty_columns);
    }

    invalid.raw(ready().bytes);
    co_await socket.write_all(invalid.bytes);
  } else if (function && mode != Backend::function_recovery) {
    wire::Writer invalid;
    wire::Writer value;
    value.integer(mode == Backend::function_truncated ? 4 : 0);
    if (mode != Backend::function_missing)
      invalid.message('V', value);
    if (mode == Backend::function_duplicate)
      invalid.message('V', value);

    invalid.raw(ready().bytes);
    co_await socket.write_all(invalid.bytes);
  } else if (mode == Backend::cancellation) {
    std::array<std::byte, 1> byte;
    auto closed = co_await weave::as_result(socket.read(byte));
    CHECK((!closed || *closed == 0));
    co_return;
  } else {
    wire::Writer error;
    error.integer('S', 1);
    error.string("ERROR");
    error.integer('C', 1);
    error.string(function ? "42883" : "42703");
    error.integer('M', 1);
    error.string("missing column");
    error.integer(0, 1);
    wire::Writer failed;
    failed.message('E', error);
    failed.raw(ready().bytes);
    // Fragment the error and ReadyForQuery across independent native completions.
    for (auto byte : failed.bytes)
      co_await socket.write_all(std::span{&byte, 1});

    CHECK(co_await request(socket) == 'Q');
    co_await socket.write_all(answer().bytes);
    CHECK(co_await request(socket) == 'X');
    co_return;
  }

  std::array<std::byte, 1> byte;
  auto closed = co_await weave::as_result(socket.read(byte));
  CHECK((!closed || *closed == 0));
}

static weave::Task<void> client(weave::u16 port, Backend mode)
{
  auto connection = co_await pg::connect({.host = "127.0.0.1", .port = port, .user = "test", .plaintext = true});
  if (mode == Backend::cancellation) {
    auto result = co_await weave::as_result(weave::timeout(10ms, connection.query("SELECT pg_sleep(30)")));
    CHECK_FALSE(result);
    CHECK(result.error() == std::errc::timed_out);
    CHECK_FALSE(connection.open());
    co_return;
  }

  bool function = mode == Backend::function_missing || mode == Backend::function_truncated ||
    mode == Backend::function_duplicate || mode == Backend::function_recovery;
  std::error_code failure;
  if (function) {
    auto invalid = co_await weave::as_result(connection.call_function(0));
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error() == std::errc::invalid_argument);
    CHECK(connection.open());
    auto result = co_await weave::as_result(connection.call_function(42));
    REQUIRE_FALSE(result);
    failure = result.error();
  } else {
    auto result = co_await weave::as_result(connection.query("SELECT missing_column"));
    REQUIRE_FALSE(result);
    failure = result.error();
  }
  if (mode == Backend::recovery || mode == Backend::function_recovery) {
    CHECK(pg::sqlstate(failure) == (function ? "42883" : "42703"));
    CHECK(connection.last_error().message() == "missing column");
    CHECK(connection.open());
    auto results = co_await connection.query("SELECT 42");
    CHECK(results.size() == 1);
    CHECK(results.front().rows.front().front().integer<int>() == 42);
    co_await connection.finish();
  } else {
    CHECK(failure == (mode == Backend::excessive ? pg::Error::resource_limit : pg::Error::protocol));
    CHECK_FALSE(connection.open());
  }
}

TEST_CASE("PostgreSQL SQL errors drain but malformed oversized and cancelled exchanges are terminal")
{
  const std::array modes{
    Backend::recovery,
    Backend::malformed,
    Backend::excessive,
    Backend::cancellation,
    Backend::incomplete,
    Backend::duplicate_description,
    Backend::no_description,
    Backend::function_missing,
    Backend::function_truncated,
    Backend::function_duplicate,
    Backend::function_recovery};
  for (auto mode : modes) {
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    CHECK(ctx->run(weave::when_all(backend(*listener, mode), client(listener->local_port(), mode))));
  }
}

static weave::Task<void> reject_startup(weave::TcpListener &listener, bool tls, unsigned *visits = nullptr)
{
  auto socket = co_await listener.accept();
  if (visits)
    ++*visits;
  auto greeting = co_await startup(socket);
  wire::Reader reader{greeting};
  CHECK(reader.integer() == (tls ? 80877103u : 196610u));
  if (tls) {
    const std::array denied{std::byte{'N'}};
    co_await socket.write_all(denied);
    co_return;
  }

  wire::Writer error;
  error.integer('S', 1);
  error.string("FATAL");
  error.integer('C', 1);
  error.string("28P01");
  error.integer('M', 1);
  error.string("invalid password");
  error.integer(0, 1);
  wire::Writer response;
  response.message('E', error);
  co_await socket.write_all(response.bytes);
}

static weave::Task<void> observe_fallback(weave::TcpListener &listener, bool &visited)
{
  auto socket = co_await weave::as_result(weave::timeout(100ms, listener.accept()));
  visited = socket.has_value();
  if (!socket)
    CHECK(socket.error() == std::errc::timed_out);
}

static weave::Task<void> rejected_connection(weave::u16 first, weave::u16 second, bool tls)
{
  pg::Options options;
  options.user = "test";
  options.plaintext = !tls;
  options.hosts = {{"127.0.0.1", first, {}}, {"127.0.0.1", second, {}}};
  options.connect_timeout = 1s;
  pg::Diagnostic diagnostic;
  auto result = co_await weave::as_result(pg::connect(options, diagnostic));
  REQUIRE_FALSE(result);
  if (tls) {
    CHECK(result.error() == pg::Error::authentication);
    CHECK(diagnostic.fields.empty());
  } else {
    CHECK(pg::sqlstate(result.error()) == "28P01");
    CHECK(diagnostic.sqlstate() == "28P01");
  }
}

TEST_CASE("PostgreSQL authentication and TLS rejection never fall through to another host")
{
  const std::array modes{false, true};
  for (auto tls : modes) {
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    auto first = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    auto second = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(first);
    REQUIRE(second);
    bool visited = false;
    CHECK(ctx->run(
      weave::when_all(
        reject_startup(*first, tls),
        observe_fallback(*second, visited),
        rejected_connection(first->local_port(), second->local_port(), tls))));
    CHECK_FALSE(visited);
  }
}

static weave::Task<void> startup_peer(
  weave::TcpListener &listener,
  bool stall,
  bool rows = false,
  unsigned *visits = nullptr)
{
  auto socket = co_await listener.accept();
  if (visits)
    ++*visits;
  static_cast<void>(co_await startup(socket));
  if (stall) {
    std::array<std::byte, 1> bytes;
    auto ended = co_await weave::as_result(socket.read(bytes));
    CHECK((!ended || *ended == 0));
    co_return;
  }

  wire::Writer authentication;
  authentication.integer(0);
  wire::Writer response;
  response.message('R', authentication);
  response.raw(ready().bytes);
  co_await socket.write_all(response.bytes);
  if (rows) {
    CHECK(co_await request(socket) == 'Q');
    co_await socket.write_all(answer().bytes);
  }
  CHECK(co_await request(socket) == 'X');
}

static weave::Task<void> deadline_client(weave::u16 first, weave::u16 second, bool cancel)
{
  pg::Options options;
  options.user = "test";
  options.plaintext = true;
  options.hosts = {{"127.0.0.1", first, {}}, {"127.0.0.1", second, {}}};
  options.connect_timeout = cancel ? 1s : 20ms;
  if (cancel) {
    auto result = co_await weave::as_result(weave::timeout(10ms, pg::connect(options)));
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::timed_out);
    co_return;
  }

  auto connection = co_await pg::connect(options);
  CHECK(connection.open());
  co_await connection.finish();
}

TEST_CASE("PostgreSQL host deadlines drain before failover and parent cancellation stops further attempts")
{
  const std::array modes{false, true};
  for (auto cancel : modes) {
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    auto first = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    auto second = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(first);
    REQUIRE(second);
    bool visited = false;
    auto fallback = cancel ? observe_fallback(*second, visited) : startup_peer(*second, false);
    CHECK(ctx->run(
      weave::when_all(
        startup_peer(*first, true),
        std::move(fallback),
        deadline_client(first->local_port(), second->local_port(), cancel))));
    CHECK_FALSE(visited);
  }
}

enum class BalancedPeer {
  authentication,
  tls,
  cancellation,
  deadline,
  success
};

static weave::Task<void> balanced_peer(weave::TcpListener &listener, BalancedPeer mode, unsigned &visits)
{
  bool reject = mode == BalancedPeer::authentication || mode == BalancedPeer::tls;
  auto task = reject ? reject_startup(listener, mode == BalancedPeer::tls, &visits)
                     : startup_peer(listener, mode != BalancedPeer::success, false, &visits);
  auto result = co_await weave::as_result(weave::timeout(250ms, std::move(task)));
  if (!result && result.error() != std::errc::timed_out)
    co_await weave::fail(result.error());
}

static weave::Task<void> balanced_client(weave::u16 first, weave::u16 second, BalancedPeer mode)
{
  pg::Options options;
  options.user = "test";
  options.plaintext = mode != BalancedPeer::tls;
  options.host_balance = pg::HostBalance::random;
  options.hosts = {{"127.0.0.1", first}, {"127.0.0.1", second}};
  options.connect_timeout = mode == BalancedPeer::deadline ? 20ms : 1s;
  pg::Diagnostic diagnostic;
  auto task = mode == BalancedPeer::cancellation ? weave::timeout(50ms, pg::connect(options, diagnostic))
                                                 : pg::connect(options, diagnostic);
  auto result = co_await weave::as_result(std::move(task));
  if (mode == BalancedPeer::success) {
    REQUIRE(result);
    if (result)
      co_await result->finish();
    co_return;
  }

  REQUIRE_FALSE(result);
  if (result)
    co_return;
  if (mode == BalancedPeer::authentication) {
    CHECK(pg::sqlstate(result.error()) == "28P01");
    CHECK(diagnostic.sqlstate() == "28P01");
  } else if (mode == BalancedPeer::tls) {
    CHECK(result.error() == pg::Error::authentication);
  } else {
    CHECK(result.error() == std::errc::timed_out);
  }
}

TEST_CASE("PostgreSQL random ordering preserves security stops, cancellation and per-host deadlines")
{
  const std::array modes{
    BalancedPeer::authentication,
    BalancedPeer::tls,
    BalancedPeer::cancellation,
    BalancedPeer::deadline,
    BalancedPeer::success};
  for (auto mode : modes) {
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    if (!ctx)
      return;
    auto first = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    auto second = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(first);
    REQUIRE(second);
    if (!first || !second)
      return;

    unsigned visits = 0;
    auto result = ctx->run(
      weave::when_all(
        balanced_peer(*first, mode, visits),
        balanced_peer(*second, mode, visits),
        balanced_client(first->local_port(), second->local_port(), mode)));
    CHECK(result);
    CHECK(visits == (mode == BalancedPeer::deadline ? 2u : 1u));
  }
}

static weave::Task<void> balanced_address_peer(weave::TcpListener &listener, unsigned &visits)
{
  for (unsigned iteration = 0; iteration < 8; ++iteration) {
    auto result = co_await weave::as_result(weave::timeout(250ms, startup_peer(listener, false, false, &visits)));
    if (!result) {
      if (result.error() != std::errc::timed_out)
        co_await weave::fail(result.error());
      co_return;
    }
  }
}

static weave::Task<void> balanced_address_client(weave::u16 port)
{
  pg::Options options;
  options.host = "localhost";
  options.port = port;
  options.user = "test";
  options.plaintext = true;
  options.host_balance = pg::HostBalance::random;
  for (unsigned iteration = 0; iteration < 8; ++iteration) {
    auto connection = co_await pg::connect(options);
    co_await connection.finish();
  }
}

TEST_CASE("PostgreSQL random single-host selection resolves native localhost addresses")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  if (!ctx)
    return;
  auto v4 = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(v4);
  if (!v4)
    return;
  auto v6 = weave::tcp::listen(*ctx, "::1", v4->local_port());
  REQUIRE(v6);
  if (!v6)
    return;

  unsigned visits = 0;
  auto result = ctx->run(
    weave::when_all(
      balanced_address_peer(*v4, visits),
      balanced_address_peer(*v6, visits),
      balanced_address_client(v4->local_port())));
  CHECK(result);
  CHECK(visits == 8);
}

TEST_CASE("PostgreSQL invalid host and startup policies fail before network setup")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  pg::Options options;
  options.user = "test";
  options.plaintext = true;
  const std::array invalid_targets{static_cast<pg::TargetSession>(-1), static_cast<pg::TargetSession>(100)};
  for (auto target : invalid_targets) {
    options.target_session = target;
    auto result = ctx->run(pg::connect(options));
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::invalid_argument);
  }

  options.target_session = pg::TargetSession::any;
  const std::array invalid_balances{static_cast<pg::HostBalance>(-1), static_cast<pg::HostBalance>(100)};
  for (auto balance : invalid_balances) {
    options.host_balance = balance;
    auto result = ctx->run(pg::connect(options));
    REQUIRE_FALSE(result);
    if (!result)
      CHECK(result.error() == std::errc::invalid_argument);
  }
  options.host_balance = pg::HostBalance::ordered;
  options.hosts.resize(65);
  auto too_many = ctx->run(pg::connect(options));
  REQUIRE_FALSE(too_many);
  CHECK(too_many.error() == std::errc::invalid_argument);
  options.hosts.resize(1);
  options.hosts.front().port = 0;
  auto no_port = ctx->run(pg::connect(options));
  REQUIRE_FALSE(no_port);
  CHECK(no_port.error() == std::errc::invalid_argument);
  options.hosts.front().port = 5432;
  options.server_options = std::string("-c a=b\0suffix", 14);
  auto embedded_nul = ctx->run(pg::connect(options));
  REQUIRE_FALSE(embedded_nul);
  CHECK(embedded_nul.error() == std::errc::invalid_argument);
  options.server_options.clear();
  options.hosts.front().password = std::string{"secret\0suffix", 13};
  auto invalid_password = ctx->run(pg::connect(options));
  REQUIRE_FALSE(invalid_password);
  CHECK(invalid_password.error() == std::errc::invalid_argument);
  options.hosts.front().password.reset();

  const std::array reserved_settings{
    "user",
    "database",
    "application_name",
    "client_encoding",
    "options",
    "replication",
    "_pq_.unsupported"};
  for (auto name : reserved_settings) {
    options.settings = {{name, "value"}};
    auto result = ctx->run(pg::connect(options));
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::invalid_argument);
  }
  options.settings = {{"TimeZone", std::string{"UTC\0suffix", 10}}};
  auto invalid_setting = ctx->run(pg::connect(options));
  REQUIRE_FALSE(invalid_setting);
  CHECK(invalid_setting.error() == std::errc::invalid_argument);
  options.settings.assign(65, {"TimeZone", "UTC"});
  auto excessive_settings = ctx->run(pg::connect(options));
  REQUIRE_FALSE(excessive_settings);
  CHECK(excessive_settings.error() == std::errc::invalid_argument);

  options.settings.clear();
  const std::array<std::size_t, 2> invalid_pipeline_limits{0, 65536};
  for (auto limit : invalid_pipeline_limits) {
    options.limits.pipeline_commands = limit;
    auto result = ctx->run(pg::connect(options));
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::invalid_argument);
  }
}

template <class T>
static weave::Task<void> pending_reset(
  pg::Connection &connection,
  const pg::Options &options,
  [[maybe_unused]] weave::Task<T> pending)
{
  auto result = co_await weave::as_result(connection.reset(options));
  REQUIRE_FALSE(result);
  CHECK(result.error() == pg::Error::busy);
  CHECK(connection.open());
}

static weave::Task<void> discard_row(const std::vector<pg::Column> &, pg::Row &)
{
  co_return;
}

static weave::Task<void> deferred_client(weave::u16 port)
{
  pg::Options options{.host = "127.0.0.1", .port = port, .user = "test", .plaintext = true};
  auto connection = co_await pg::connect(options);
  co_await pending_reset(connection, options, connection.query("SELECT 42"));
  co_await pending_reset(connection, options, connection.execute("SELECT 42"));
  co_await pending_reset(connection, options, connection.prepare("statement", "SELECT 42"));
  co_await pending_reset(connection, options, connection.execute_prepared("statement"));
  co_await pending_reset(connection, options, connection.describe("statement"));
  co_await pending_reset(connection, options, connection.close_prepared("statement"));
  co_await pending_reset(connection, options, connection.call_function(42));
  co_await pending_reset(connection, options, connection.batch({}));

  co_await pending_reset(connection, options, connection.start_copy("COPY table_name TO STDOUT"));
  co_await pending_reset(connection, options, connection.write_copy({}));
  co_await pending_reset(connection, options, connection.read_copy());
  co_await pending_reset(connection, options, connection.end_copy());
  co_await pending_reset(connection, options, connection.start_rows("SELECT 42"));
  co_await pending_reset(connection, options, connection.read_row());

  co_await pending_reset(connection, options, connection.open_portal("portal", "SELECT 42"));
  co_await pending_reset(connection, options, connection.fetch("portal"));
  co_await pending_reset(connection, options, connection.close_portal("portal"));
  co_await pending_reset(connection, options, connection.wait_notification());
  co_await pending_reset(connection, options, connection.finish());
  co_await pending_reset(connection, options, connection.for_each_row("SELECT 42", discard_row));

  namespace lo = pg::lo;
  co_await pending_reset(connection, options, lo::create(connection));
  co_await pending_reset(connection, options, lo::open(connection, 42));
  co_await pending_reset(connection, options, lo::read(connection, 0));
  co_await pending_reset(connection, options, lo::read(connection, 0, std::span<std::byte>{}));
  co_await pending_reset(connection, options, lo::write(connection, 0, {}));
  co_await pending_reset(connection, options, lo::write_all(connection, 0, {}));
  co_await pending_reset(connection, options, lo::seek(connection, 0, 0));
  co_await pending_reset(connection, options, lo::tell(connection, 0));
  co_await pending_reset(connection, options, lo::truncate(connection, 0, 0));
  co_await pending_reset(connection, options, lo::close(connection, 0));
  co_await pending_reset(connection, options, lo::remove(connection, 42));
  co_await connection.finish();
}

TEST_CASE("PostgreSQL reset rejects every deferred session task without freeing its implementation")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  CHECK(ctx->run(weave::when_all(startup_peer(*listener, false), deferred_client(listener->local_port()))));
}

struct ResetFromRow {
  pg::Connection &connection;
  const pg::Options &options;

  weave::Task<void> operator()(const std::vector<pg::Column> &columns, pg::Row &row)
  {
    auto result = co_await weave::as_result(connection.reset(options));
    REQUIRE_FALSE(result);
    CHECK(result.error() == pg::Error::busy);
    CHECK(columns.size() == 1);
    CHECK(columns.front().name == "value");
    CHECK(row.front().integer<int>() == 42);
  }
};

static weave::Task<void> callback_client(weave::u16 port)
{
  pg::Options options{.host = "127.0.0.1", .port = port, .user = "test", .plaintext = true};
  auto connection = co_await pg::connect(options);
  co_await connection.for_each_row("SELECT 42", ResetFromRow{connection, options});
  co_await connection.finish();
}

TEST_CASE("PostgreSQL row callbacks retain session metadata through nested reset rejection")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  CHECK(ctx->run(weave::when_all(startup_peer(*listener, false, true), callback_client(listener->local_port()))));
}
