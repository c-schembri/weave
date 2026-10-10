#include <weave/postgres.hpp>
#include <weave/io.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include "tls_certificates.hpp"
#include <array>
#include <cstdio>
#include <iostream>
#include <thread>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif

namespace pg = weave::pg;
using namespace std::chrono_literals;

struct Handles {
  pg::CancelHandle old;
  pg::CancelHandle current;
};

static weave::Task<Handles> sessions(pg::Options options, bool reset)
{
  auto connection = co_await pg::connect(options);
  auto original = connection.cancel_handle();
  if (!original)
    co_await weave::fail(original.error());
  if (reset)
    co_await connection.reset(options);
  auto current = connection.cancel_handle();
  if (!current)
    co_await weave::fail(current.error());
  co_await connection.finish();
  co_return Handles{std::move(*original), std::move(*current)};
}

static weave::Result<Handles> make_handles(pg::Options options, bool reset)
{
  auto ctx = weave::Context::create();
  if (!ctx)
    return std::unexpected(ctx.error());
  return ctx->run(sessions(std::move(options), reset));
}

static weave::Task<void> dropped_handle(pg::CancelHandle handle)
{
  return handle.request();
}

static weave::Result<weave::Task<void>> owned_request(pg::Options options)
{
  auto handles = make_handles(std::move(options), false);
  if (!handles)
    return std::unexpected(handles.error());
  return dropped_handle(handles->old);
}

static weave::Task<void> blocking_in_task(pg::CancelHandle handle)
{
  auto result = handle.request_blocking();
  if (!result)
    co_await weave::fail(result.error());
}

static weave::Task<void> deferred_cancel_reset(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  auto pending = connection.request_cancel();
  co_await connection.reset(options);
  co_await std::move(pending);
  co_await connection.finish();
}

static weave::Task<weave::Task<void>> detached_request(pg::Options options, bool move)
{
  auto connection = co_await pg::connect(options);
  auto pending = connection.request_cancel();
  if (move) {
    auto relocated = std::move(connection);
    co_await relocated.finish();
  } else {
    co_await connection.finish();
  }
  co_return std::move(pending);
}

static weave::Result<weave::Task<void>> deferred_request(pg::Options options, bool move)
{
  auto ctx = weave::Context::create();
  if (!ctx)
    return std::unexpected(ctx.error());
  return ctx->run(detached_request(std::move(options), move));
}

static weave::Task<void> closed_request(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  co_await connection.finish();
  auto pending = connection.request_cancel();
  co_await connection.reset(options);
  co_await connection.finish();
  co_await std::move(pending);
}

static weave::Task<void> joined(weave::JoinHandle<void> job)
{
  co_await std::move(job);
}

static bool matches(const weave::Result<void> &result, std::string_view mode)
{
  if (mode == "closed_request")
    return !result && result.error() == pg::Error::closed;
  if (mode == "protocol")
    return !result && result.error() == pg::Error::protocol;
  if (mode == "refused" || mode == "invalid_ssl_reply")
    return !result && result.error() == pg::Error::authentication;
  if (mode == "certificate")
    return !result && result.error() == weave::TlsError::certificate_verification;
  if (mode.starts_with("deadline") || mode == "parent_cancel")
    return !result && result.error() == std::errc::timed_out;
  if (mode == "pre_cancel")
    return !result && result.error() == std::errc::operation_canceled;
  return result.has_value();
}

int main(int argc, char **argv)
{
#if defined(_WIN32)
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#else
  rlimit limit{0, 0};
  if (setrlimit(RLIMIT_CORE, &limit) != 0)
    return 2;
#endif
  if (argc != 7)
    return 2;
  auto mode = std::string_view{argv[1]};
  bool secured = std::string_view{argv[2]} == "tls";
  bool v30 = std::string_view{argv[3]} == "3.0";
  auto workers = weave::parse_port(argv[4]);
  if (!workers || *workers > 4)
    return 2;
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  auto scheduler = std::string_view{argv[5]} == "stealing" ? weave::Scheduler::work_stealing
                                                           : weave::Scheduler::worker_affine;
  auto layout = std::string_view{argv[6]} == "shared" ? weave::IoLayout::shared : weave::IoLayout::sharded;
#else
  if (*workers != 0)
    return 2;
#endif
  fixture::Certificates certificates;
  std::printf(
    "%s\n%s\n%s\n%s\n",
    certificates.ca.c_str(),
    certificates.leaf.c_str(),
    certificates.private_key.c_str(),
    certificates.expired.c_str());
  std::fflush(stdout);
  std::string line;
  std::getline(std::cin, line);
  auto port = weave::parse_port(line);
  if (!port)
    return 2;
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "weave", .plaintext = !secured};
  options.min_protocol = options.max_protocol = v30 ? pg::ProtocolVersion::v30 : pg::ProtocolVersion::v32;
  options.connect_timeout = mode.starts_with("deadline") ? 250ms : 5s;
  if (secured) {
    auto credentials = weave::TlsContext::client(
      {.ca_file = certificates.ca,
        .certificate_file = certificates.client,
        .private_key_file = certificates.client_key});
    if (!credentials)
      return weave::report_error(credentials.error());
    options.tls = *credentials;
  }
  if (mode == "deferred_reset" || mode == "closed_request") {
    auto ctx = weave::Context::create();
    if (!ctx)
      return weave::report_error(ctx.error());

    auto operation = mode == "closed_request" ? closed_request(options) : deferred_cancel_reset(options);
    auto result = ctx->run(std::move(operation));
    if (!matches(result, mode))
      return result ? 3 : weave::report_error(result.error());
    if (ctx->metrics().submitted != ctx->metrics().completed)
      return 3;
    return 0;
  }
  if (mode == "owned_request" || mode == "deferred_destroy" || mode == "deferred_move") {
    auto operation = mode == "owned_request" ? owned_request(options)
                                             : deferred_request(options, mode == "deferred_move");
    options.tls.reset();
    if (!operation)
      return weave::report_error(operation.error());
    auto ctx = weave::Context::create();
    if (!ctx)
      return weave::report_error(ctx.error());
    auto result = ctx->run(std::move(*operation));
    return result ? 0 : weave::report_error(result.error());
  }
  auto handles = make_handles(options, mode == "reset");
  if (!handles)
    return weave::report_error(handles.error());
  if (mode == "moved_from") {
    auto moved = std::move(handles->old);
    auto invalid = handles->old.request();
    return 3;
  }
  if (mode == "blocking_in_task") {
    auto ctx = weave::Context::create();
    if (!ctx)
      return weave::report_error(ctx.error());
    auto invalid = ctx->run(blocking_in_task(handles->old));
    return 3;
  }

  if (mode == "blocking" || mode == "reset_eof_blocking" || mode == "reset_after_packet_blocking") {
    std::array<std::error_code, 16> errors;
    std::vector<std::jthread> threads;
    for (int worker = 0; worker < 4; ++worker) {
      threads.emplace_back([worker, handle = handles->old, &errors] {
        for (int index = worker; index < 16; index += 4) {
          auto result = handle.request_blocking();
          if (!result)
            errors[index] = result.error();
        }
      });
    }
    threads.clear();
    for (auto error : errors) {
      if (error)
        return weave::report_error(error);
    }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  } else if (*workers != 0) {
    auto runtime = weave::Runtime::create({.workers = *workers, .scheduler = scheduler, .io_layout = layout});
    if (!runtime)
      return weave::report_error(runtime.error());
    std::vector<weave::JoinHandle<void>> jobs;
    for (int index = 0; index < 16; ++index) {
      auto job = runtime->spawn(dropped_handle(handles->old));
      if (!job)
        return weave::report_error(job.error());
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!matches(result, mode))
        return result ? 3 : weave::report_error(result.error());
    }
#endif
  } else {
    auto ctx = weave::Context::create();
    if (!ctx)
      return weave::report_error(ctx.error());
    weave::Result<void> result;
    if (mode == "pre_cancel") {
      weave::CancelSource source;
      source.cancel();
      auto job = ctx->spawn(dropped_handle(handles->old), {.cancel = source.token()});
      if (!job)
        return weave::report_error(job.error());
      result = ctx->run(joined(std::move(*job)));
      if (ctx->metrics().submitted != 0)
        return 3;
    } else if (mode == "parent_cancel") {
      result = ctx->run(weave::timeout(100ms, dropped_handle(handles->old)));
    } else {
      result = ctx->run(dropped_handle(handles->old));
    }
    if (!matches(result, mode))
      return result ? 3 : weave::report_error(result.error());
    if (mode == "reset") {
      auto current = ctx->run(dropped_handle(handles->current));
      if (!current)
        return weave::report_error(current.error());
    }
    if (ctx->metrics().submitted != ctx->metrics().completed)
      return 3;
  }
  std::puts("cancellation qualification passed");
}
