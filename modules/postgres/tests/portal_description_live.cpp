#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include "tls_certificates.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <source_location>

namespace pg = weave::pg;
using namespace std::chrono_literals;
static std::atomic<unsigned> checks = 0;

static void check(bool condition, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "Portal live check failed at line %u\n", location.line());
    std::exit(1);
  }
}

static void columns(const pg::ResultSet &description, pg::Format format)
{
  if (description.columns.size() == 2) {
    const auto &column = description.columns[0];
    if (column.type_size != 4 || column.modifier != -1 || column.format != format)
      std::fprintf(
        stderr,
        "Observed portal column: size=%d modifier=%d format=%u expected=%u\n",
        column.type_size,
        column.modifier,
        static_cast<unsigned>(column.format),
        static_cast<unsigned>(format));
  }
  check(description.columns.size() == 2 && description.rows.empty());
  check(description.parameter_types.empty() && description.command.empty() && !description.suspended);
  check(description.columns[0].name == "value" && description.columns[0].type == 23);
  check(description.columns[0].table == 0 && description.columns[0].attribute == 0);
  check(description.columns[0].type_size == 4 && description.columns[0].modifier == -1);
  check(description.columns[0].format == format);
  check(description.columns[1].name == "nullable" && description.columns[1].type == 25);
  check(description.columns[1].format == format);
}

static weave::Task<void> session(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  co_await connection.query("BEGIN");
  co_await connection.query("DECLARE probe BINARY CURSOR FOR SELECT 1::int AS value, NULL::text AS nullable");
  auto description = co_await connection.describe_portal("probe");
  columns(description, pg::Format::text);
  auto first = co_await connection.query("FETCH 1 FROM probe");
  check(first.size() == 1 && first[0].rows.size() == 1);
  check(first[0].rows[0][0].binary_integer<int>() == weave::Result<int>{1});
  check(first[0].rows[0][1].is_null());
  {
    auto deferred = connection.describe_portal("probe");
    auto blocked = co_await weave::as_result(connection.reset(options));
    check(!blocked && blocked.error() == pg::Error::busy && connection.open());
  }
  auto nonexistent = co_await weave::as_result(connection.describe_portal("missing"));
  check(!nonexistent && pg::sqlstate(nonexistent.error()) == "34000" && connection.open());
  check(connection.transaction() == pg::Transaction::failed);
  co_await connection.query("ROLLBACK");
  co_await connection.query("BEGIN");
  auto opened = co_await connection.open_portal(
    "bound",
    "SELECT $1::int AS value, NULL::text AS nullable",
    {{"7", 23}});
  columns(opened, pg::Format::text);
  auto bound = co_await connection.describe_portal("bound");
  columns(bound, pg::Format::text);
  auto fetched = co_await connection.fetch("bound", 1);
  check(fetched.rows.size() == 1 && fetched.rows[0][0].integer<int>() == weave::Result<int>{7});
  co_await connection.close_portal("bound");
  co_await connection.query("COMMIT");
  // An unnamed portal is observable after an extended Execute in an explicit transaction.
  co_await connection.query("BEGIN");
  co_await connection.execute("SELECT 1::int AS value, NULL::text AS nullable");
  auto unnamed = co_await connection.describe_portal("");
  columns(unnamed, pg::Format::text);
  co_await connection.execute("SELECT 1::int AS value, NULL::text AS nullable", {}, pg::Format::binary);
  auto binary = co_await connection.describe_portal("");
  columns(binary, pg::Format::binary);
  co_await connection.execute("SET application_name TO 'portal_no_data'");
  auto no_data = co_await connection.describe_portal("");
  check(no_data.columns.empty() && no_data.parameter_types.empty() && no_data.rows.empty() && no_data.command.empty());
  co_await connection.query("COMMIT");
  co_await connection.finish();
  columns(description, pg::Format::text);
  columns(bound, pg::Format::text);
  columns(binary, pg::Format::binary);
}

static void blocking(const pg::Options &options)
{
  auto connection = pg::BlockingConnection::connect(options);
  check(connection.has_value());
  check(connection->query("BEGIN").has_value());
  check(connection->query("DECLARE probe CURSOR FOR SELECT 1::int AS value, NULL::text AS nullable").has_value());
  auto description = connection->describe_portal("probe");
  check(description.has_value());
  columns(*description, pg::Format::text);
  check(connection->close_portal("probe").has_value());
  auto missing = connection->describe_portal("probe");
  check(!missing && pg::sqlstate(missing.error()) == "34000" && connection->open());
  check(connection->query("ROLLBACK").has_value());
  check(connection->finish().has_value());
  columns(*description, pg::Format::text);
}

int main()
{
  static fixture::Certificates certificates;
  std::printf("%s\n%s\n%s\n", certificates.ca.c_str(), certificates.leaf.c_str(), certificates.private_key.c_str());
  std::fflush(stdout);
  std::string port_text, password, standby, address_text, local;
  std::getline(std::cin, port_text);
  std::getline(std::cin, password);
  std::getline(std::cin, standby);
  std::getline(std::cin, address_text);
  std::getline(std::cin, local);
  std::printf("Owned certificate fixture: %s\n", certificates.directory.string().c_str());
  auto port = weave::parse_port(port_text);
  auto address = weave::IpAddress::parse(address_text);
  check(port && address);
  auto credentials = weave::TlsContext::client(
    {.ca_file = certificates.ca, .certificate_file = certificates.client, .private_key_file = certificates.client_key});
  check(credentials.has_value());
  pg::Options plain;
  plain.host = "localhost";
  plain.hosts = {{"localhost", *port, *address}};
  plain.user = "weave";
  plain.database = "postgres";
  plain.password = password;
  plain.plaintext = true;
  auto secured = plain;
  secured.plaintext = false;
  secured.tls = *credentials;
  secured.channel_binding = pg::ChannelBinding::require;
  const std::array profiles{plain, secured};
  auto ctx = weave::Context::create();
  check(ctx.has_value());
  for (const auto &profile : profiles) {
    auto result = ctx->run(weave::timeout(30s, session(profile)));
    if (!result)
      return weave::report_error(result.error());
    blocking(profile);
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    check(runtime.has_value());
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 64; ++index) {
      auto job = runtime->spawn(weave::timeout(30s, session(profiles[index % profiles.size()])));
      check(job.has_value());
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!result)
        return weave::report_error(result.error());
    }
  }
#endif
  std::printf("Portal live controls passed: %u checks\n", checks.load());
}
