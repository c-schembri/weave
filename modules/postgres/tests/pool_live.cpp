#include <weave/postgres.hpp>
#include <weave/io.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include "tls_certificates.hpp"
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
#include "runtime_fixture.hpp"
#endif
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace pg = weave::pg;

static void check(bool value, std::source_location location = std::source_location::current())
{
  if (!value) {
    std::fprintf(stderr, "Live pool check failed: %s:%u\n", location.file_name(), location.line());
    std::exit(EXIT_FAILURE);
  }
}

static weave::Task<void> exercise(pg::Options options)
{
  auto pool = co_await pg::Pool::create(options, {.max_connections = 1});
  weave::u32 process = 0;
  std::string search_path;
  {
    auto database = co_await pool.acquire();
    process = database->backend_process();
    auto settings = co_await database->query("SHOW search_path");
    search_path = settings.front().rows.front().front().bytes();
    co_await database->query(
      "CREATE TEMP TABLE pool_leak(x int); PREPARE pool_statement AS SELECT 1; "
      "LISTEN pool_channel; SELECT pg_advisory_lock(42); SET search_path=pg_catalog");
    co_await database->set_client_encoding(pg::Encoding::sql_ascii);
    co_await database->query("BEGIN; INSERT INTO pool_leak VALUES (42)");
  }

  {
    auto database = co_await pool.acquire();
    check(database->backend_process() == process && database->transaction() == pg::Transaction::idle);
    check(database->client_encoding() == pg::Encoding::utf8);
    auto restored = co_await database->query("SHOW search_path");
    check(restored.front().rows.front().front().bytes() == search_path);
    auto clean = co_await database->query(
      "SELECT to_regclass('pg_temp.pool_leak') IS NULL, "
      "(SELECT count(*)=0 FROM pg_prepared_statements), "
      "(SELECT count(*)=0 FROM pg_listening_channels()), "
      "(SELECT count(*)=0 FROM pg_locks WHERE pid=pg_backend_pid() AND locktype='advisory')");
    for (const auto &value : clean.front().rows.front())
      check(value.bytes() == "t");

    co_await database->query("BEGIN");
    auto bad = co_await weave::as_result(database->query("SELECT missing_pool_column"));
    check(!bad && database->transaction() == pg::Transaction::failed);
  }

  {
    auto database = co_await pool.acquire();
    check(database->transaction() == pg::Transaction::idle && database->backend_process() == process);
    co_await database->finish();
  }
  {
    auto database = co_await pool.acquire();
    check(database->backend_process() != process && pool.stats().created == 2);
    auto result = co_await database->query("SELECT 42");
    check(result.front().rows.front().front().bytes() == "42");
  }
}

int main()
{
  fixture::Certificates files;
  std::printf("%s\n%s\n%s\n", files.ca.c_str(), files.leaf.c_str(), files.private_key.c_str());
  std::fflush(stdout);
  std::string port_text, password, standby, address, local;
  check(static_cast<bool>(std::getline(std::cin, port_text)));
  check(static_cast<bool>(std::getline(std::cin, password)));
  check(static_cast<bool>(std::getline(std::cin, standby)));
  check(static_cast<bool>(std::getline(std::cin, address)));
  check(static_cast<bool>(std::getline(std::cin, local)));
  auto port = weave::parse_port(port_text);
  check(port.has_value());

  pg::Options plain{
    .host = "127.0.0.1",
    .port = *port,
    .user = "weave",
    .database = "postgres",
    .password = password,
    .plaintext = true};
  plain.max_protocol = pg::ProtocolVersion::v30;
  auto secured = plain;
  weave::TlsClientOptions policy;
  policy.ca_file = files.ca;
  policy.certificate_file = files.client;
  policy.private_key_file = files.client_key;
  auto credentials = weave::TlsContext::client(policy);
  check(credentials.has_value());
  secured.tls = std::move(*credentials);
  secured.plaintext = false;
  secured.channel_binding = pg::ChannelBinding::require;

  auto ctx = weave::Context::create();
  check(ctx.has_value());
  const std::array profiles{plain, secured};
  for (const auto &options : profiles) {
    auto result = ctx->run(weave::timeout(std::chrono::seconds{10}, exercise(options)));
    if (!result)
      return weave::report_error(result.error());
  }

#ifdef WEAVE_POSTGRES_TEST_RUNTIME
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto layout : support::io_layouts) {
    for (auto scheduler : schedulers) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
      check(runtime.has_value());
      std::vector<weave::JoinHandle<void>> jobs;
      for (std::size_t worker = 0; worker < 4; ++worker) {
        auto job = runtime->spawn_on(worker, weave::timeout(std::chrono::seconds{10}, exercise(secured)));
        check(job.has_value());
        jobs.push_back(std::move(*job));
      }
      for (auto &job : jobs) {
        auto result = std::move(job).get();
        if (!result)
          return weave::report_error(result.error());
      }
    }
  }
#endif

  std::puts("Pool session isolation: plaintext, verified mTLS and four-worker runtimes passed");
}
