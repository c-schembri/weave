#pragma once

#include <weave/postgres.hpp>
#include <weave/timer.hpp>
#include <weave/log.hpp>
#include <source_location>
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace weave::pg::test {

namespace pg = weave::pg;
using namespace std::chrono_literals;

static weave::Task<void> check(bool value, std::source_location location = std::source_location::current())
{
  if (!value) {
    WEAVE_LOG_ERROR("Check failed: %u", location.line());
    co_await weave::fail(std::errc::bad_message);
  }
}

static bool answer(const pg::ResultSet &result)
{
  return result.rows.size() == 1 && result.rows[0].size() == 1 && result.rows[0][0].bytes() == "42";
}

static weave::Task<void> cancel_entered(weave::CancelSource &source, bool stop_context)
{
#if !defined(_WIN32)
  using Entered = unsigned (*)();
  auto entered = reinterpret_cast<Entered>(dlsym(RTLD_DEFAULT, "weave_test_gss_entered"));
  co_await check(entered != nullptr);
  for (unsigned attempt = 0; attempt < 1000; ++attempt) {
    if (entered()) {
      if (stop_context)
        weave::detail::current_context->request_stop();
      else
        source.cancel();
      co_return;
    }
    co_await weave::sleep_for(1ms);
  }
#endif
  source.cancel();
  co_await weave::fail(std::errc::timed_out);
}

static weave::Task<void> connecting(weave::Task<pg::Connection> operation)
{
  auto connection = co_await std::move(operation);
  co_await weave::fail(std::errc::bad_message);
}

static weave::Task<void> cancellation(pg::Options options, std::string mode)
{
  if (mode == "pre-cancel") {
    weave::CancelSource source;
    source.cancel();
    auto operation = pg::connect(options);
    weave::detail::TaskAccess::bind(operation, source.token());
    auto result = co_await weave::as_result(std::move(operation));
    co_await check(!result && result.error() == std::errc::operation_canceled);
    co_return;
  }
  const auto before = std::chrono::steady_clock::now();
  weave::CancelSource source;
  auto connection = pg::connect(options);
  weave::detail::TaskAccess::bind(connection, source.token());
  auto result = co_await weave::as_result(
    weave::when_all(connecting(std::move(connection)), cancel_entered(source, mode == "context-stop")));
  co_await check(!result && result.error() == std::errc::operation_canceled);
  // Cancellation drains the native operation and shielded destruction before returning.
  if (mode == "cancel" || mode == "context-stop")
    co_await check(std::chrono::steady_clock::now() - before >= 150ms);
}

} // namespace weave::pg::test
