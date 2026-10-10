#include <weave/postgres.hpp>
#include <weave/channel.hpp>
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include "credential_allocations.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace pg = weave::pg;

using namespace std::chrono_literals;
static unsigned checks = 0;

static void check(bool condition, std::source_location location = std::source_location::current())
{
  if (!condition) {
    std::fprintf(stderr, "Password session check failed at line %u\n", location.line());
    std::exit(1);
  }
  ++checks;
}

static weave::Task<void> exercise(weave::Context &ctx, pg::Options options, std::string mode)
{
  bool sensitive = false;
  unsigned secret_frames = 0;
  weave::Channel<int> markers{1};
  pg::Trace trace{
    .handler =
      [&](const pg::TraceMessage &message) noexcept {
        if (message.direction == pg::TraceDirection::frontend && message.kind == 'Q' && message.redacted)
          sensitive = true;
        if (sensitive) {
          check(message.redacted && message.payload.empty() && !message.truncated);
          ++secret_frames;
        }
      },
    .content = pg::TraceContent::application,
    .payload_bytes = 65536};
  auto connection = co_await pg::connect(options, {}, std::move(trace));
  auto observer = connection.on_notice([&](const pg::Diagnostic &notice) noexcept {
    if (notice.message() == "PENDING") {
      int marker = 1;
      check(markers.try_send(marker).has_value());
    }
  });
  check(observer.has_value());
  pg::PasswordOptions policy;
  if (mode == "explicit_scram" || mode == "explicit_md5" || mode == "limit")
    policy.algorithm = mode == "explicit_md5" ? pg::PasswordAlgorithm::md5 : pg::PasswordAlgorithm::scram_sha256;
  if (mode == "normal_md5")
    policy.allow_md5 = true;

  std::string user = "test";
  if (mode == "quote_utf8")
    user = "na\xc3\xafve\"role";
  if (mode == "quote_sjis")
    user = "\x83\x5c\"role";
  if (mode == "limit")
    user = std::string(1000, 'u');
  std::string password(127, '~');

  if (mode == "cancel_show" || mode == "cancel_alter") {
    auto operation = connection.change_password(user, password, policy);
    auto copy = fixture::credential_copy(password);
    check(!copy.empty());
    fixture::CredentialAllocation witness{copy};
    weave::CancelSource cancellation;
    auto job = ctx.spawn(std::move(operation), {.cancel = cancellation.token()});
    check(job.has_value());
    auto marker = co_await markers.receive();
    check(marker == 1);
    // The peer acknowledged the query and withholds completion. Observe native pending I/O, not a timed guess.
    while (ctx.metrics().submitted == ctx.metrics().completed)
      co_await ctx.yield();
    auto pending = ctx.metrics();
    check(pending.submitted == pending.completed + 1);
    auto busy = co_await weave::as_result(connection.query("NOOP"));
    check(!busy && busy.error() == pg::Error::busy && connection.open());
    auto reset = co_await weave::as_result(connection.reset(options));
    check(!reset && reset.error() == pg::Error::busy && connection.open());
    cancellation.cancel();
    auto result = co_await weave::as_result(std::move(*job));
    check(!result && result.error() == std::errc::operation_canceled && !connection.open());
    check(witness.cleansed());
    auto drained = ctx.metrics();
    check(drained.submitted == drained.completed);
    if (mode == "cancel_alter")
      check(secret_frames >= 2);
    co_return;
  }

  auto result = co_await weave::as_result(connection.change_password(user, password, policy));
  const std::array malformed{"no_rows", "two_rows", "null", "binary", "two_columns", "no_columns"};
  bool malformed_shape = std::ranges::find(malformed, mode) != malformed.end();
  if (malformed_shape) {
    check(!result && result.error() == pg::Error::protocol && !connection.open());
    co_return;
  }
  if (mode == "unknown_algorithm" || mode == "missing_encoding" || mode == "unknown_encoding" ||
    mode == "policy_encoding" || mode == "limit" || mode == "sql_error") {
    std::error_code expected;
    if (mode == "missing_encoding")
      expected = pg::make_error_code(pg::Error::protocol);
    else if (mode == "limit")
      expected = pg::make_error_code(pg::Error::resource_limit);
    else if (mode == "sql_error")
      expected = pg::sql_error("42501");
    else
      expected = std::make_error_code(std::errc::operation_not_supported);
    check(!result && result.error() == expected && connection.open());
  } else {
    check(result && result->command == "ALTER ROLE" && connection.open());
    check(secret_frames >= 3);
  }
  sensitive = false;
  auto reusable = co_await connection.query("NOOP");
  check(reusable.size() == 1 && reusable.front().command == "SELECT 1");
  co_await connection.finish();
}

int main(int argc, char **argv)
{
  if (argc != 3)
    return 1;
  auto port = weave::parse_port(argv[1]);
  check(port.has_value());
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "test", .plaintext = true};
  if (std::string_view{argv[2]} == "limit")
    options.limits.message_bytes = 1024;
  auto ctx = weave::Context::create();
  check(ctx.has_value());
  auto result = ctx->run(weave::timeout(10s, exercise(*ctx, options, argv[2])));
  if (!result)
    return weave::report_error(result.error());
  std::printf("Password session controls passed: %u checks\n", checks);
}
