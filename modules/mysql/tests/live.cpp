#include <weave/mysql.hpp>
#include <weave/io.hpp>
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include <weave/scope.hpp>
#if defined(WEAVE_MYSQL_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <cstdio>
#include <cstdlib>

using namespace std::chrono_literals;

struct Shipment {
  weave::i64 id;
  std::string name;
};

static void verify(bool condition, const char *description)
{
  if (!condition) {
    std::fprintf(stderr, "MySQL live check failed: %s\n", description);
    std::abort();
  }
}

#include "scope_live.hpp"

static weave::Task<void> qualify(weave::u16 port, std::string ca, std::string password)
{
  auto credentials = weave::TlsContext::client({.ca_file = std::move(ca)});
  if (!credentials)
    co_await weave::fail(credentials.error());
  weave::mysql::Options options{
    .port = port,
    .user = "weave",
    .password = weave::mysql::Password{password},
    .database = "weave_test",
    .tls = *credentials};

  auto scope_options = options;
  scope_options.multi_statements = true;
  co_await qualify_scope(std::move(scope_options));

  // The fixture starts with an uncached account: first connect exercises full auth; second exercises fast auth.
  for (int iteration = 0; iteration < 2; ++iteration) {
    auto db = co_await weave::mysql::connect(options);
    auto tls = db.tls_info();
    verify(tls && *tls && (**tls).hostname_verified && (**tls).certificate_verified, "verified TLS");
    auto result = co_await db.execute("SELECT 42 AS id, 'shipment' AS name, NULL AS absent, '' AS blank");
    verify(result.rows.size() == 1, "one row");
    auto shipment = result.rows[0].as<Shipment>();
    verify(shipment && shipment->id == 42 && shipment->name == "shipment", "automatic struct mapping");
    auto absent = result.rows[0]["absent"].as<std::optional<int>>();
    verify(absent && !*absent, "NULL conversion");
    verify(result.rows[0]["blank"].as<std::string_view>() == "", "empty is not NULL");

    auto failed = co_await weave::as_result(db.execute("SELECT missing_column"));
    verify(!failed && failed.error() == weave::mysql::server_error(1054), "server errors propagate");
    auto diagnostic = db.last_failure();
    verify(diagnostic && *diagnostic && (**diagnostic).sqlstate == "42S22", "SQLSTATE retained");
    verify(db.open(), "SQL errors do not retire the session");
    result = co_await db.execute("SELECT 1 AS recovered");
    verify(result.rows[0]["recovered"].as<int>() == 1, "SQL error recovery");
    co_await db.disconnect();
    verify(!db.open(), "graceful disconnect");
  }

  auto db = co_await weave::mysql::connect(options);
  co_await db.execute(
    "CREATE TEMPORARY TABLE shipments (id BIGINT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(64), bits BIT(6))");
  auto inserted = co_await db.execute(
    "INSERT INTO shipments (name, bits) VALUES ('first', b'110001'), ('second', b'111111')");
  verify(
    inserted.rows.empty() && inserted.affected_rows == 2 && inserted.last_insert_id == 1,
    "DML counts and insert ID");
  auto selected = co_await db.execute("SELECT id, name FROM shipments ORDER BY id");
  verify(selected.rows.size() == 2 && selected.columns.size() == 2, "table metadata and rows");
  auto first = selected.rows[0].as<Shipment>();
  verify(first && first->id == 1 && first->name == "first", "real table struct mapping");
  auto bits = co_await db.execute("SELECT bits FROM shipments WHERE id = 1");
  verify(
    !bits.rows[0]["bits"].as<int>() && bits.rows[0]["bits"].as<std::string_view>() == "1",
    "BIT bytes are not decimal text");
  co_await db.execute("START TRANSACTION");
  co_await db.execute("UPDATE shipments SET name = 'changed' WHERE id = 1");
  co_await db.execute("ROLLBACK");
  auto rolled_back = co_await db.execute("SELECT name FROM shipments WHERE id = 1");
  verify(rolled_back.rows[0]["name"].as<std::string_view>() == "first", "verbatim transaction commands");
  auto result = co_await db.execute("SELECT REPEAT('x', 16777211) AS data");
  verify(
    result.rows[0]["data"].as<std::string_view>()->size() == 16777211,
    "exact-boundary receive with empty terminator");
  result = co_await db.execute("SELECT REPEAT('y', 16777215) AS data");
  verify(result.rows[0]["data"].as<std::string_view>()->size() == 16777215, "multi-packet receive");

  std::string sql = "SELECT 7 AS boundary /*";
  sql.append(16777214 - sql.size() - 2, 'x');
  sql.append("*/");
  result = co_await db.execute_borrowed(sql);
  verify(result.rows[0]["boundary"].as<int>() == 7, "exact-boundary send with empty terminator");
  sql.insert(sql.size() - 2, 8, 'x');
  result = co_await db.execute_borrowed(sql);
  verify(result.rows[0]["boundary"].as<int>() == 7, "multi-packet send");

  auto cancelled = co_await weave::as_result(weave::timeout(20ms, db.execute("SELECT SLEEP(1)")));
  verify(
    !cancelled && cancelled.error() == std::errc::timed_out && !db.open(),
    "pending cancellation retires/drains session");

  options.password = "incorrect";
  auto rejected = co_await weave::as_result(weave::mysql::connect(options));
  verify(!rejected && rejected.error() == weave::mysql::server_error(1045), "wrong credentials rejected");
  options.password = weave::mysql::Password{password};
  options.max_rows = 1;
  auto limited = co_await weave::mysql::connect(options);
  auto exceeded = co_await weave::as_result(limited.execute("SELECT 1 AS n UNION ALL SELECT 2"));
  verify(
    !exceeded && exceeded.error() == weave::mysql::ClientError::resource_limit && !limited.open(),
    "row resource limit");
  std::puts("MySQL live: TLS, full/fast auth, rows, SQL recovery, packet boundaries, cancellation and limits passed");
}

int main(int argc, char **argv)
{
  if (argc != 4)
    return 2;
  auto port = weave::parse_port(argv[1]);
  auto ctx = weave::Context::create();
  if (!port || !ctx)
    return 2;

  auto result = ctx->run(weave::timeout(30s, qualify(*port, argv[2], argv[3])));
  if (!result) {
    std::fprintf(stderr, "MySQL live: %s\n", result.error().message().c_str());
    return 1;
  }
  auto tls = weave::TlsContext::client({.ca_file = argv[2]});
  if (!tls)
    return 1;
  qualify_blocking({.port = *port, .user = "weave", .password = argv[3], .database = "weave_test", .tls = *tls});
#if defined(WEAVE_MYSQL_TEST_RUNTIME)
  auto runtime = weave::Runtime::create({.workers = 4, .scheduler = weave::Scheduler::work_stealing});
  verify(bool(runtime), "multicore Runtime");
  if (!runtime)
    return 1;
  auto concurrent = runtime->run(
    weave::timeout(
      10s,
      qualify_multicore({.port = *port, .user = "weave", .password = argv[3], .database = "weave_test", .tls = *tls})));
  if (!concurrent) {
    std::fprintf(stderr, "MySQL multicore: %s\n", concurrent.error().message().c_str());
    return 1;
  }
#endif
}
