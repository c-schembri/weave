#include <weave/postgres.hpp>
#include <weave/scope.hpp>
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include "tls_certificates.hpp"
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include "runtime_fixture.hpp"
#endif
#include <iostream>
#include <atomic>
#include <source_location>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <thread>

static std::atomic<unsigned> checks{0};
static std::atomic<unsigned> cancellations{0};

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Direct TLS server control failed: %s:%u\n", where.file_name(), where.line());
    std::exit(EXIT_FAILURE);
  }
}

static weave::pg::Options options(
  fixture::Certificates &files,
  weave::u16 port,
  const std::string &password,
  const weave::IpAddress &address,
  bool prebuilt)
{
  weave::pg::Options result;
  result.host = "localhost";
  result.hosts = {{"localhost", port, address}};
  result.port = port;
  result.user = "weave";
  result.database = "postgres";
  result.password = password;
  result.channel_binding = weave::pg::ChannelBinding::require;
  result.tls_negotiation = weave::pg::TlsNegotiation::direct;

  weave::TlsClientOptions tls;
  tls.ca_file = files.ca;
  tls.alpn = {"unrelated"};
  tls.certificate_file = files.client;
  tls.private_key_file = files.client_key;

  if (prebuilt) {
    auto credentials = weave::TlsContext::client(std::move(tls));
    check(credentials.has_value());
    result.tls = std::move(*credentials);
  } else {
    result.tls_options = std::move(tls);
  }

  return result;
}

static weave::Task<void> cancel_after_notice(weave::pg::Connection &connection)
{
  for (int i = 0; i < 200; ++i) {
    auto notices = connection.take_notices();
    for (const auto &notice : notices) {
      if (notice.message() == "direct_cancel_ready") {
        co_await connection.request_cancel();
        ++cancellations;
        co_return;
      }
    }
    co_await weave::sleep_for(std::chrono::milliseconds{10});
  }

  co_await weave::fail(std::errc::timed_out);
}

static weave::Task<void> slow_query(weave::pg::Connection &connection)
{
  auto result = co_await weave::as_result(
    connection.query("DO $$BEGIN RAISE NOTICE 'direct_cancel_ready'; PERFORM pg_sleep(30); END$$"));
  check(!result && weave::pg::sqlstate(result.error()) == "57014");
}

static weave::Task<void> live(weave::pg::Options configuration)
{
  auto connection = co_await weave::pg::connect(configuration);
  auto info = connection.info();
  check(info.has_value() && info->tls && info->tls->negotiated_protocol == "postgresql");
  check(info->authentication.complete && info->authentication.method == weave::pg::Authentication::scram_sha256);

  auto snapshot = connection.configuration();
  check(snapshot.has_value() && snapshot->tls_negotiation == weave::pg::TlsNegotiation::direct);
  auto result = co_await connection.query("SELECT 42");
  check(result.size() == 1 && result.front().rows.size() == 1 && result.front().rows.front().size() == 1);
  check(result.front().rows.front().front().bytes() == "42");

  result = co_await connection.query(
    "SELECT ssl::text, (client_dn IS NOT NULL)::text FROM pg_stat_ssl WHERE pid=pg_backend_pid()");
  check(result.size() == 1 && result.front().rows.size() == 1 && result.front().rows.front().size() == 2);
  check(result.front().rows.front()[0].bytes() == "true" && result.front().rows.front()[1].bytes() == "true");

  co_await connection.reset(std::move(configuration));
  info = connection.info();
  check(info.has_value() && info->tls && info->tls->negotiated_protocol == "postgresql");
  check(info->authentication.complete && info->authentication.method == weave::pg::Authentication::scram_sha256);

  co_await weave::timeout(
    std::chrono::seconds{5},
    weave::when_all(slow_query(connection), cancel_after_notice(connection)));
  result = co_await connection.query("SELECT 43");
  check(result.size() == 1 && result.front().rows.size() == 1 && result.front().rows.front().size() == 1);
  check(result.front().rows.front().front().bytes() == "43");

  co_await connection.finish();
}

static void blocking_cancel(weave::pg::BlockingConnection &connection, weave::pg::Options configuration)
{
  auto cancel = connection.cancel_handle();
  check(cancel.has_value());
  const auto process = connection.backend_process();
  std::error_code cancel_error;
  bool dispatched = false;

  // Only the independent snapshot crosses threads; the query connection stays on its owner.
  std::thread worker{[handle = *cancel, configuration = std::move(configuration), process, &cancel_error, &dispatched] {
    auto observer = weave::pg::BlockingConnection::connect(configuration);
    if (!observer) {
      cancel_error = observer.error();
      return;
    }

    const auto active_query = "SELECT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid = " + std::to_string(process) +
      " AND state = 'active' AND wait_event = 'PgSleep')";
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
      auto active = observer->query(active_query);
      if (!active) {
        cancel_error = active.error();
        return;
      }
      check(active->size() == 1 && active->front().rows.size() == 1 && active->front().rows.front().size() == 1);
      if (active->front().rows.front().front().bytes() == "t") {
        auto status = handle.request_blocking();
        if (!status)
          cancel_error = status.error();
        else
          dispatched = true;

        if (auto finish = observer->finish(); !finish && !cancel_error)
          cancel_error = finish.error();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    cancel_error = std::make_error_code(std::errc::timed_out);
  }};

  auto result = connection.query("SELECT pg_sleep(6)");
  worker.join();
  check(dispatched && !cancel_error);
  check(!result && weave::pg::sqlstate(result.error()) == "57014");
  ++cancellations;
}

int main()
{
  static fixture::Certificates files;
  std::printf("%s\n%s\n%s\n", files.ca.c_str(), files.leaf.c_str(), files.private_key.c_str());
  std::fflush(stdout);

  std::string port_text, password, standby, address_text, local;
  check(static_cast<bool>(std::getline(std::cin, port_text)));
  check(static_cast<bool>(std::getline(std::cin, password)));
  check(static_cast<bool>(std::getline(std::cin, standby)));
  check(static_cast<bool>(std::getline(std::cin, address_text)));
  check(static_cast<bool>(std::getline(std::cin, local)));
  auto port = weave::parse_port(port_text);
  auto address = weave::IpAddress::parse(address_text);
  check(port.has_value() && address.has_value());
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);

  auto ctx = weave::Context::create();
  check(ctx.has_value());
  const std::array prebuilt_options{false, true};
  for (bool prebuilt : prebuilt_options) {
    auto configuration = options(files, *port, password, *address, prebuilt);
    check(ctx->run(live(configuration)).has_value());

    auto blocking = weave::pg::BlockingConnection::connect(configuration);
    check(blocking.has_value());
    auto info = blocking->info();
    check(info.has_value() && info->tls && info->tls->negotiated_protocol == "postgresql");
    check(info->authentication.complete && info->authentication.method == weave::pg::Authentication::scram_sha256);
    check(blocking->reset(configuration).has_value());

    blocking_cancel(*blocking, configuration);
    auto result = blocking->query("SELECT 44");
    check(
      result.has_value() && result->size() == 1 && result->front().rows.size() == 1 &&
      result->front().rows.front().size() == 1);
    check(result->front().rows.front().front().bytes() == "44");
    check(blocking->finish().has_value());

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
    const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
    for (auto layout : support::io_layouts) {
      for (auto scheduler : schedulers) {
        auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
        check(runtime.has_value());

        std::vector<weave::JoinHandle<void>> jobs;
        for (int i = 0; i < 16; ++i) {
          auto job = runtime->spawn(live(configuration));
          check(job.has_value());
          jobs.push_back(std::move(*job));
        }

        for (auto &job : jobs)
          check(std::move(job).get().has_value());
      }
    }
#endif
  }

  unsigned expected = 4;
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  expected += 64 * static_cast<unsigned>(support::io_layouts.size());
#endif
  check(cancellations == expected);
  std::printf("Direct TLS live: %u checks, %u observed query cancellations\n", checks.load(), cancellations.load());
}
