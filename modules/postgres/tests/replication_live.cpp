#include "replication_live.hpp"
#include <array>
#include <filesystem>
#include <fstream>

namespace fixture {

namespace pg = weave::pg;

static std::array<std::byte, 34> feedback()
{
  std::array<std::byte, 34> message{};
  message.front() = std::byte{'r'};
  message.back() = std::byte{1};
  return message;
}

static bool valid_identity(const pg::Results &results)
{
  return results.size() == 1 && results.front().rows.size() == 1 && results.front().rows.front().size() == 4 &&
    !results.front().rows.front()[2].is_null();
}

static bool valid_feedback(const std::optional<std::vector<std::byte>> &message)
{
  if (!message || message->empty())
    return false;

  const auto kind = message->front();
  return (kind == std::byte{'k'} && message->size() == 18) || (kind == std::byte{'w'} && message->size() >= 25);
}

static bool valid_completion(const pg::Results &results)
{
  return results.size() == 2 && results.front().command == "START_STREAMING" &&
    results.back().command == "START_REPLICATION";
}

template <class T>
static bool unsupported(const weave::Result<T> &result)
{
  return !result && result.error() == std::errc::operation_not_supported;
}

template <class T>
static weave::Task<void> unsupported(weave::Task<T> operation)
{
  auto result = co_await weave::as_result(std::move(operation));
  if (!unsupported(result))
    co_await weave::fail(std::errc::bad_message);
}

static weave::Task<void> simple_protocol_only(pg::Connection &connection)
{
  co_await unsupported(connection.execute("SELECT 1"));
  co_await unsupported(connection.prepare("probe", "SELECT 1"));
  co_await unsupported(connection.execute_prepared("probe"));
  co_await unsupported(connection.describe("probe"));
  co_await unsupported(connection.close_prepared("probe"));
  co_await unsupported(connection.open_portal("portal", "SELECT 1"));
  co_await unsupported(connection.fetch("portal"));
  co_await unsupported(connection.close_portal("portal"));
  co_await unsupported(connection.call_function(0));
  co_await unsupported(connection.batch({}));
  if (!unsupported(connection.pipeline()) || !connection.open())
    co_await weave::fail(std::errc::bad_message);
}

weave::Task<void> replication_loaded(pg::Options options, std::filesystem::path directory)
{
  auto path = directory / "replication-password";

  struct Cleanup {
    std::filesystem::path path;

    ~Cleanup()
    {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } cleanup{path};

  std::ofstream output(path, std::ios::binary);
  std::error_code error;
  std::filesystem::permissions(
    path,
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
    std::filesystem::perm_options::replace,
    error);
  if (error)
    co_await weave::fail(error);

  output << "*:*:postgres:weave_replication:wrong_database_entry\n"
         << "*:*:replication:weave_replication:" << options.password << '\n';
  output.close();
  if (!output)
    co_await weave::fail(std::errc::io_error);

  auto name = path.u8string();
  auto loaded = pg::Options::load(
    "user=weave_replication dbname=postgres replication=true",
    {.environment = false, .user_files = false, .password_file = std::string{name.begin(), name.end()}});
  if (!loaded)
    co_await weave::fail(loaded.error());
  if (loaded->password != options.password)
    co_await weave::fail(std::errc::bad_message);

  loaded->host = options.host;
  loaded->port = options.port;
  loaded->hosts = options.hosts;
  loaded->plaintext = options.plaintext;
  loaded->tls = options.tls;
  loaded->channel_binding = options.channel_binding;
  std::filesystem::remove(path, error);
  if (error)
    co_await weave::fail(error);

  co_await replication(std::move(*loaded));
}

weave::Task<void> replication(pg::Options options)
{
  options.keep_alive = {.idle = std::chrono::seconds{60}, .interval = std::chrono::seconds{5}, .probes = 3};
#if !defined(_WIN32)
  options.tcp_user_timeout = std::chrono::milliseconds{1234};
#endif
  options.user = "weave_replication";
  options.target_session = pg::TargetSession::any;
  options.replication = pg::Replication::physical;
  auto connection = co_await pg::connect(options);
  co_await simple_protocol_only(connection);
  auto identity = co_await connection.query("IDENTIFY_SYSTEM");
  if (!valid_identity(identity))
    co_await weave::fail(std::errc::bad_message);

  auto invalid = co_await weave::as_result(connection.start_copy("START_REPLICATION PHYSICAL invalid"));
  if (invalid || pg::sqlstate(invalid.error()) != "42601" || !connection.open())
    co_await weave::fail(std::errc::bad_message);

  auto lsn = identity.front().rows.front()[2].bytes();
  auto format = co_await connection.start_copy("START_REPLICATION PHYSICAL " + std::string(lsn));
  if (format.direction != pg::CopyDirection::both || !format.columns.empty())
    co_await weave::fail(std::errc::bad_message);

  auto message = feedback();
  co_await connection.write_copy(message);
  if (!valid_feedback(co_await connection.read_copy()))
    co_await weave::fail(std::errc::bad_message);

  co_await connection.finish_copy_send();
  while (co_await connection.read_copy()) {
  }
  if (connection.copy_results())
    co_await weave::fail(std::errc::bad_message);

  auto completion = co_await connection.end_copy();
  auto results = connection.copy_results();
  if (!results || !valid_completion(*results) || completion.command != "START_REPLICATION")
    co_await weave::fail(std::errc::bad_message);
  if (!valid_identity(co_await connection.query("IDENTIFY_SYSTEM")))
    co_await weave::fail(std::errc::bad_message);

  co_await connection.finish();

  options.replication = pg::Replication::database;
  auto database = co_await pg::connect(options);
  co_await simple_protocol_only(database);
  if (!valid_identity(co_await database.query("IDENTIFY_SYSTEM")))
    co_await weave::fail(std::errc::bad_message);
  auto sql = co_await database.query("SELECT 42");
  if (sql.size() != 1 || sql.front().rows.size() != 1 || sql.front().rows.front().size() != 1 ||
    sql.front().rows.front().front().bytes() != "42")
    co_await weave::fail(std::errc::bad_message);

  co_await database.finish();
}

weave::Result<void> replication_blocking(pg::Options options)
{
  options.user = "weave_replication";
  options.target_session = pg::TargetSession::any;
  options.replication = pg::Replication::physical;
  auto connection = pg::BlockingConnection::connect(options);
  if (!connection)
    return std::unexpected(connection.error());

  if (!unsupported(connection->execute("SELECT 1")) || !unsupported(connection->prepare("probe", "SELECT 1")) ||
    !unsupported(connection->execute_prepared("probe")) || !unsupported(connection->describe("probe")) ||
    !unsupported(connection->close_prepared("probe")) || !unsupported(connection->open_portal("portal", "SELECT 1")) ||
    !unsupported(connection->fetch("portal")) || !unsupported(connection->close_portal("portal")) ||
    !unsupported(connection->call_function(0)) || !unsupported(connection->batch({})) ||
    !unsupported(connection->pipeline()) || !connection->open())
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  auto identity = connection->query("IDENTIFY_SYSTEM");
  if (!identity)
    return std::unexpected(identity.error());
  if (!valid_identity(*identity))
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  auto lsn = identity->front().rows.front()[2].bytes();
  auto format = connection->start_copy("START_REPLICATION PHYSICAL " + std::string(lsn));
  if (!format)
    return std::unexpected(format.error());
  if (format->direction != pg::CopyDirection::both)
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  auto message = feedback();
  if (auto sent = connection->write_copy(message); !sent)
    return sent;
  auto received = connection->read_copy();
  if (!received)
    return std::unexpected(received.error());
  if (!valid_feedback(*received))
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  if (auto sent = connection->finish_copy_send(); !sent)
    return sent;
  if (auto completed = connection->end_copy(); !completed)
    return std::unexpected(completed.error());
  auto results = connection->copy_results();
  if (!results || !valid_completion(*results))
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  return connection->finish();
}

} // namespace fixture
