#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include "tls_certificates.hpp"
#include "credential_allocations.hpp"
#include <atomic>
#include <iostream>
#include <optional>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

namespace pg = weave::pg;

using namespace std::chrono_literals;
static std::atomic<int> checks{0};

static void check(bool condition)
{
  auto index = checks.fetch_add(1) + 1;
  if (!condition) {
    std::fprintf(stderr, "Password live check %d failed\n", index);
    std::exit(1);
  }
}

struct TraceState {
  int redacted_queries = 0;
  int visible_queries = 0;
  bool credential_exposed = false;
};

static weave::Task<void> live(weave::Context &ctx, pg::Options options, std::string replacement)
{
  TraceState traces;
  pg::Trace trace{
    .handler =
      [&traces](const pg::TraceMessage &message) noexcept {
        if (message.direction == pg::TraceDirection::frontend && message.kind == 'Q') {
          if (message.redacted)
            ++traces.redacted_queries;
          else
            ++traces.visible_queries;
          std::string_view payload{reinterpret_cast<const char *>(message.payload.data()), message.payload.size()};
          traces.credential_exposed |= payload.find("ALTER USER") != std::string_view::npos;
          traces.credential_exposed |= payload.find("SCRAM-SHA-256$") != std::string_view::npos;
        }
      },
    .content = pg::TraceContent::application,
    .payload_bytes = 65536};
  auto connection = co_await pg::connect(options, {}, std::move(trace));
  auto verifier = co_await connection.password_verifier(options.user, "pencil");
  check(verifier.starts_with("SCRAM-SHA-256$4096:"));
  auto explicit_hash = co_await connection.password_verifier(
    options.user,
    "pencil",
    {.algorithm = pg::PasswordAlgorithm::md5});
  auto expected = pg::password_verifier(options.user, "pencil", pg::PasswordAlgorithm::md5);
  check(expected && explicit_hash == *expected);

  std::string watched(127, '~');
  {
    fixture::CredentialPattern cleanup{watched};
    std::optional<weave::Task<pg::ResultSet>> unstarted;
    unstarted.emplace(connection.change_password(options.user, watched));
    auto copy = fixture::credential_copy(watched);
    check(!copy.empty());
    fixture::CredentialAllocation witness{copy};
    unstarted.reset();
    check(witness.cleansed());
    check(cleanup.dirty_releases() == 0);
  }
  std::optional<weave::Task<pg::ResultSet>> deferred;
  deferred.emplace(connection.change_password(options.user, watched));
  auto reset = co_await weave::as_result(connection.reset(options));
  check(!reset && reset.error() == pg::make_error_code(pg::Error::busy));
  deferred.reset();
  weave::CancelSource cancellation;
  cancellation.cancel();
  auto job = ctx.spawn(connection.change_password(options.user, watched), {.cancel = cancellation.token()});
  check(static_cast<bool>(job));
  auto cancelled = co_await weave::as_result(std::move(*job));
  check(!cancelled && cancelled.error() == std::errc::operation_canceled && connection.open());
  auto stopped = weave::Context::create();
  check(static_cast<bool>(stopped));
  stopped->shutdown();
  std::string rejected_password(127, '^');
  auto rejected_task = connection.password_verifier(options.user, rejected_password);
  auto copied = fixture::credential_copy(rejected_password);
  check(!copied.empty());
  fixture::CredentialAllocation rejected_witness{copied};
  auto rejected = stopped->spawn(std::move(rejected_task));
  check(!rejected && connection.open() && rejected_witness.cleansed());
  auto invalid = co_await weave::as_result(connection.change_password(options.user, std::string_view{"x\0y", 3}));
  check(!invalid && invalid.error() == std::errc::invalid_argument && connection.open());

  auto owned = replacement;
  auto change = connection.change_password(options.user, owned);
  owned.assign("caller changed its buffer");
  auto changed = co_await std::move(change);
  check(changed.command == "ALTER ROLE");
  check(traces.redacted_queries >= 1 && traces.visible_queries >= 1 && !traces.credential_exposed);
  auto renewed_options = options;
  renewed_options.password = replacement;
  auto renewed = co_await pg::connect(renewed_options);
  auto old = co_await weave::as_result(pg::connect(options));
  if (!old)
    std::printf("Old password rejection SQLSTATE: %s\n", pg::sqlstate(old.error()).c_str());
  check(!old && pg::sqlstate(old.error()) == "28P01");
  co_await renewed.finish();

  auto injection = co_await weave::as_result(connection.change_password("weave\"; SELECT 1; --", "pencil"));
  check(!injection && pg::sqlstate(injection.error()) == "42704" && connection.open());
  auto query = co_await connection.query("SELECT 17");
  check(query.size() == 1 && query.front().rows.front().front().bytes() == "17");
  co_await connection.query("SET password_encryption=md5");
  auto refused = co_await weave::as_result(connection.password_verifier(options.user, "pencil"));
  check(!refused && refused.error() == std::errc::operation_not_supported && connection.open());
  auto legacy = co_await connection.password_verifier(options.user, "pencil", {.allow_md5 = true});
  check(legacy == *expected);
  co_await connection.query("SET password_encryption='scram-sha-256'");
  co_await connection.change_password(options.user, options.password);
  auto failed = co_await weave::as_result(connection.query("BEGIN; SELECT 1/0"));
  check(!failed && pg::sqlstate(failed.error()) == "22012" && connection.open());
  auto aborted = co_await weave::as_result(connection.password_verifier(options.user, "pencil"));
  check(!aborted && pg::sqlstate(aborted.error()) == "25P02" && connection.open());
  co_await connection.query("ROLLBACK");
  co_await connection.finish();
}

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
static weave::Task<void> session(pg::Options options)
{
  auto connection = co_await pg::connect(std::move(options));
  auto verifier = co_await connection.password_verifier("weave", "pencil");
  check(verifier.starts_with("SCRAM-SHA-256$4096:"));
  co_await connection.finish();
}
#endif

static weave::Task<void> legacy_changes(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  co_await connection.query("SET password_encryption=md5");
  auto refused = co_await weave::as_result(connection.change_password(options.user, "refused"));
  check(!refused && refused.error() == std::errc::operation_not_supported && connection.open());

  const std::array policies{
    pg::PasswordOptions{.algorithm = pg::PasswordAlgorithm::md5},
    pg::PasswordOptions{.allow_md5 = true}};
  for (auto policy : policies) {
    auto replacement = std::string{"O'Reilly\\legacy-password"};
    auto changed = co_await connection.change_password(options.user, replacement, policy);
    check(changed.command == "ALTER ROLE");
    auto renewed_options = options;
    renewed_options.password = replacement;
    auto renewed = co_await pg::connect(renewed_options);
    check(renewed.authentication_method() == pg::Authentication::md5);
    auto old = co_await weave::as_result(pg::connect(options));
    check(!old && pg::sqlstate(old.error()) == "28P01");
    co_await renewed.finish();
    co_await connection.change_password(options.user, options.password, policy);
  }
  co_await connection.finish();
}

int main()
{
#if defined(_MSC_VER)
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
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
  std::fflush(stdout);
  auto port = weave::parse_port(port_text);
  auto address = weave::IpAddress::parse(address_text);
  check(port && address);
  auto credentials = weave::TlsContext::client(
    {.ca_file = certificates.ca, .certificate_file = certificates.client, .private_key_file = certificates.client_key});
  check(static_cast<bool>(credentials));
  pg::Options plain;
  plain.host = "127.0.0.1";
  plain.hosts = {{plain.host, *port, *address}};
  plain.port = *port;
  plain.user = "weave";
  plain.database = "postgres";
  plain.password = password;
  plain.plaintext = true;
  auto secured = plain;
  secured.plaintext = false;
  secured.tls = *credentials;
  secured.channel_binding = pg::ChannelBinding::require;
  auto ctx = weave::Context::create();
  check(static_cast<bool>(ctx));
  const std::array profiles{plain, secured};
  for (const auto &profile : profiles) {
    auto result = ctx->run(weave::timeout(20s, live(*ctx, profile, "O'Reilly\xc2\xadX\\new-password")));
    if (!result)
      return weave::report_error(result.error());
  }
  auto legacy = secured;
  legacy.user = "weave_md5";
  legacy.channel_binding = pg::ChannelBinding::disable;
  legacy.allow_md5_password = true;
  if (auto result = ctx->run(weave::timeout(20s, legacy_changes(legacy))); !result)
    return weave::report_error(result.error());

  auto blocking = pg::BlockingConnection::connect(secured);
  check(static_cast<bool>(blocking));
  auto verifier = blocking->password_verifier("weave", "pencil");
  check(verifier && verifier->starts_with("SCRAM-SHA-256$4096:"));
  auto changed = blocking->change_password("weave", password);
  check(changed && changed->command == "ALTER ROLE");
  check(static_cast<bool>(blocking->finish()));
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    check(static_cast<bool>(runtime));
    std::vector<weave::JoinHandle<void>> jobs;
    for (std::size_t index = 0; index < 64; ++index) {
      auto job = runtime->spawn(weave::timeout(20s, session(profiles[index % profiles.size()])));
      check(static_cast<bool>(job));
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!result)
        return weave::report_error(result.error());
    }
  }
#endif
  std::printf("Password live controls passed: %d checks\n", checks.load());
}
