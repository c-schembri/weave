#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include <source_location>
#include "wire.hpp"
#include "gss_startup_support.hpp"
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace pg = weave::pg;
namespace wire = weave::pg::detail;
using namespace std::chrono_literals;
using pg::test::answer;
using pg::test::cancellation;
using pg::test::check;

static weave::Task<void> malicious(weave::TcpListener &listener, std::string mode)
{
  auto client = co_await listener.accept();
  std::array<std::byte, 4> header;
  co_await client.read_exactly(header);
  wire::Reader reader{header};
  wire::Bytes startup(reader.integer() - 4);
  co_await client.read_exactly(startup);
  wire::Writer challenge;
  wire::Writer kind;
  kind.integer(7);
  challenge.message('R', kind);
  co_await client.write_all(challenge.bytes);
  std::array<std::byte, 5> packet;
  co_await client.read_exactly(packet);
  wire::Reader incoming{packet};
  if (incoming.integer(1) != 'p')
    co_await weave::fail(std::errc::bad_message);
  wire::Bytes proof(incoming.integer() - 4);
  co_await client.read_exactly(proof);
  if (proof.empty())
    co_await weave::fail(std::errc::bad_message);
  wire::Writer reply;
  wire::Writer authentication;
  if (mode == "forged-ok") {
    authentication.integer(0);
  } else if (mode == "duplicate") {
    authentication.integer(7);
  } else if (mode == "mixed") {
    authentication.integer(11);
    authentication.raw("x");
  } else {
    authentication.integer(8);
    if (mode == "corrupt")
      authentication.raw("invalid Kerberos proof");
    if (mode == "oversized") {
      wire::Bytes excessive(65537);
      authentication.raw(excessive);
    }
  }
  reply.message('R', authentication);
  co_await client.write_all(reply.bytes);
  std::array<std::byte, 1> eof;
  if (co_await client.read(eof) != 0)
    co_await weave::fail(std::errc::bad_message);
}

static weave::Task<void> exercise(pg::Options options, std::string mode)
{
  pg::Diagnostic diagnostic;
  auto opened = co_await weave::as_result(pg::connect(options, diagnostic));
  if (mode == "missing" || mode == "wrong-service" || mode == "binding" || mode == "policy") {
    co_await check(!opened);
    if (mode == "missing" || mode == "wrong-service")
      co_await check(!diagnostic.message().empty());
    co_return;
  }
  if (!opened) {
    WEAVE_LOG_ERROR("Connect: %s / %s", opened.error().message().c_str(), std::string{diagnostic.message()}.c_str());
    co_await weave::fail(opened.error());
  }
  auto connection = std::move(*opened);
  co_await check(connection.authentication_method() == pg::Authentication::gss);
  co_await check(answer(co_await connection.execute("SELECT 42")));
  co_await connection.prepare("answer", "SELECT 42");
  co_await check(answer(co_await connection.execute_prepared("answer")));
  std::vector<pg::Command> commands{{.sql = "SELECT 42"}, {.sql = "SELECT 42"}};
  auto results = co_await connection.batch(std::move(commands));
  co_await check(results.size() == 2 && results[0].result && results[1].result);
  co_await check(answer(*results[0].result) && answer(*results[1].result));
  co_await connection.reset(options);
  co_await check(connection.authentication_method() == pg::Authentication::gss);
  co_await check(answer(co_await connection.execute("SELECT 42")));
  co_await connection.finish();
}

static weave::Task<void> hostile(weave::Context &ctx, pg::Options options, std::string mode)
{
  auto listener = weave::tcp::listen(ctx, "127.0.0.1", 0);
  if (!listener)
    co_await weave::fail(listener.error());
  options.port = listener->local_port();
  options.hosts[0].port = options.port;
  auto peer = ctx.spawn(malicious(*listener, mode));
  if (!peer)
    co_await weave::fail(peer.error());
  auto connection = co_await weave::as_result(pg::connect(options));
  co_await check(!connection);
  co_await std::move(*peer);
}

static weave::Result<void> blocking(pg::Options options)
{
  pg::Diagnostic diagnostic;
  auto connection = pg::BlockingConnection::connect(options, diagnostic);
  if (!connection) {
    WEAVE_LOG_ERROR("Blocking: %s", std::string{diagnostic.message()}.c_str());
    return std::unexpected(connection.error());
  }
  auto result = connection->execute("SELECT 42");
  if (!result || !answer(*result) || connection->authentication_method() != pg::Authentication::gss)
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  if (auto reset = connection->reset(options); !reset)
    return reset;
  return connection->close();
}

int main(int argc, char **argv)
{
  if (argc != 4)
    return 1;
  auto port = weave::parse_port(argv[1]);
  if (!port)
    return 1;
  std::string mode = argv[2];
  pg::GssContextOptions provider;
  provider.workers = 2;
  provider.capacity = mode == "saturation" ? 2 : 64;
  if (mode != "default" && mode != "capture-default")
    provider.credential_cache = argv[3];
  auto gss = pg::GssContext::create(std::move(provider));
  if (!gss)
    return weave::report_error(gss.error());
#if !defined(_WIN32)
  if (mode == "capture-default")
    setenv("KRB5CCNAME", "FILE:/nonexistent/weave-credential-cache", 1);
#endif
  pg::Options options{.host = "localhost", .port = *port, .user = "client", .database = "postgres", .plaintext = true};
  options.hosts = {{.name = "localhost", .port = *port, .address = *weave::IpAddress::parse("127.0.0.1")}};
  options.gss = *gss;
  options.authentication.methods = {pg::Authentication::gss};
  if (mode == "wrong-service")
    options.gss_service = "absent_service";
  if (mode == "delegate")
    options.gss_delegation = true;
  if (mode == "binding")
    options.channel_binding = pg::ChannelBinding::require;
  if (mode == "policy")
    options.authentication.methods = {pg::Authentication::scram_sha256};
  if (mode == "blocking") {
    auto result = blocking(options);
    return result ? 0 : weave::report_error(result.error());
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  if (mode == "affine" || mode == "stealing" || mode == "saturation" || mode == "runtime-cancel") {
    auto runtime = weave::Runtime::create(
      {.workers = 4,
        .scheduler = mode == "affine" ? weave::Scheduler::worker_affine : weave::Scheduler::work_stealing});
    if (!runtime)
      return weave::report_error(runtime.error());
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 32; ++index) {
      auto job = runtime->spawn(mode == "runtime-cancel" ? cancellation(options, mode) : exercise(options, "normal"));
      if (!job)
        return weave::report_error(job.error());
      jobs.push_back(std::move(*job));
    }
    unsigned rejected = 0;
    unsigned completed = 0;
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!result && mode == "saturation" && result.error() == std::errc::no_buffer_space)
        ++rejected;
      else if (!result)
        return weave::report_error(result.error());
      else
        ++completed;
    }
    if (mode == "saturation" && (rejected == 0 || completed == 0))
      return 1;
    return 0;
  }
#endif
  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());
  const std::array hostile_modes{"forged-ok", "duplicate", "empty", "corrupt", "oversized", "mixed"};
  const bool bad_peer = std::ranges::find(hostile_modes, mode) != hostile_modes.end();
  auto operation = bad_peer                                              ? hostile(*ctx, options, mode)
    : mode == "cancel" || mode == "pre-cancel" || mode == "context-stop" ? cancellation(options, mode)
                                                                         : exercise(options, mode);
  auto result = ctx->run(std::move(operation));
  if (mode == "context-stop")
    return !result && result.error() == std::errc::operation_canceled ? 0 : 1;
  return result ? 0 : weave::report_error(result.error());
}
