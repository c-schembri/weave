#include "gss_startup_support.hpp"
#include "gss_acceptor.hpp"
#include "wire.hpp"
#include "tls_certificates.hpp"
#include <weave/port.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif

namespace pg = weave::pg;
namespace wire = weave::pg::detail;
using namespace std::chrono_literals;
using pg::test::answer;
using pg::test::cancellation;
using pg::test::check;

static weave::Task<wire::Bytes> packet(weave::TcpStream &client)
{
  std::array<std::byte, 4> header;
  co_await client.read_exactly(header);
  wire::Reader reader{header};
  const auto size = reader.integer();
  co_await check(size && size <= 65532);
  wire::Bytes bytes(size);
  co_await client.read_exactly(bytes);
  co_return bytes;
}

static weave::Task<void> native_peer(weave::TcpListener &listener, std::string mode)
{
  auto client = co_await listener.accept();
  std::array<std::byte, 8> request;
  co_await client.read_exactly(request);
  wire::Reader reader{request};
  co_await check(reader.integer() == 8 && reader.integer() == 80877104);
  wire::Writer reply;
  if (mode == "decline-prefer" || mode == "decline-require") {
    reply.integer('N', 1);
    co_await client.write_all(reply.bytes);
    if (mode == "decline-prefer") {
      std::array<std::byte, 4> header;
      co_await client.read_exactly(header);
      wire::Reader startup_size{header};
      wire::Bytes startup(startup_size.integer() - 4);
      co_await client.read_exactly(startup);
      co_await check(startup.size() > 4);
      wire::Writer authentication;
      authentication.integer(0);
      reply.bytes.clear();
      reply.message('R', authentication);
      wire::Writer ready;
      ready.integer('I', 1);
      reply.message('Z', ready);
      co_await client.write_all(reply.bytes);
      std::array<std::byte, 5> terminate;
      co_await client.read_exactly(terminate);
      co_await check(terminate[0] == std::byte{'X'});
    }
  } else if (mode == "error-response" || mode == "invalid-response") {
    reply.integer(mode == "error-response" ? 'E' : 'S', 1);
    reply.raw("untrusted diagnostic must not escape");
    co_await client.write_all(reply.bytes);
  } else {
    reply.integer('G', 1);
    co_await client.write_all(reply.bytes);
    auto token = co_await packet(client);
    reply.bytes.clear();
    if (mode == "native-zero" || mode == "native-oversized" || mode == "native-corrupt") {
      reply.integer(mode == "native-zero" ? 0 : mode == "native-oversized" ? 65533 : 3);
      if (mode == "native-corrupt")
        reply.raw("bad");
      co_await client.write_all(reply.bytes);
    } else {
      pg::test::GssAcceptor server;
      co_await check(server.start());
      auto proof = server.accept(token);
      if (!proof)
        co_await weave::fail(proof.error());
      co_await check(server.complete && !proof->empty());
      reply.integer(static_cast<weave::u32>(proof->size()));
      reply.raw(*proof);
      co_await client.write_all(reply.bytes);
      auto protected_startup = co_await packet(client);
      gss_buffer_desc input{protected_startup.size(), protected_startup.data()};
      pg::test::NativeBytes output;
      OM_uint32 minor = 0;
      int confidential = 0;
      auto major = gss_unwrap(&minor, server.context, &input, &output.value, &confidential, nullptr);
      co_await check(major == GSS_S_COMPLETE && confidential && output.value.length > 4);

      wire::Writer authentication;
      authentication.integer(0);
      wire::Writer message;
      message.message('R', authentication);
      reply.bytes.clear();
      if (mode == "record-zero" || mode == "record-oversized") {
        reply.integer(mode == "record-zero" ? 0 : 16381);
      } else if (mode == "partial-header") {
        reply.integer(12, 2);
      } else if (mode == "partial-body") {
        reply.integer(12);
        reply.raw("bad");
      } else {
        auto record = server.wrap(message.bytes, mode != "integrity-only");
        if (!record)
          co_await weave::fail(record.error());
        if (mode == "gap")
          record = server.wrap(message.bytes);
        if (!record)
          co_await weave::fail(record.error());
        if (mode == "tamper")
          record->back() ^= std::byte{1};
        reply.integer(static_cast<weave::u32>(record->size()));
        reply.raw(*record);
        if (mode == "replay") {
          reply.integer(static_cast<weave::u32>(record->size()));
          reply.raw(*record);
        }
      }
      co_await client.write_all(reply.bytes);
      if (mode == "partial-header" || mode == "partial-body") {
        if (auto status = client.shutdown_send(); !status)
          co_await weave::fail(status.error());
      }
    }
  }
  std::array<std::byte, 1> eof;
  auto closed = co_await weave::as_result(client.read(eof));
  co_await check((closed && *closed == 0) || (!closed && closed.error() == std::errc::connection_reset));
}

static weave::Task<void> hostile_transport(weave::Context &context, pg::Options options)
{
  const std::array modes{
    "decline-prefer",
    "decline-require",
    "error-response",
    "invalid-response",
    "native-zero",
    "native-oversized",
    "native-corrupt",
    "record-zero",
    "record-oversized",
    "partial-header",
    "partial-body",
    "tamper",
    "replay",
    "gap",
    "integrity-only"};
  for (const auto &mode : modes) {
    std::fprintf(stderr, "hostile=%s\n", mode);
    auto listener = weave::tcp::listen(context, "127.0.0.1", 0);
    if (!listener)
      co_await weave::fail(listener.error());
    options.hosts[0].port = listener->local_port();
    options.gss_encryption = std::string_view{mode} == "decline-require" ? pg::GssEncryption::require
                                                                         : pg::GssEncryption::prefer;
    options.authentication.methods.clear();
    auto peer = context.spawn(native_peer(*listener, mode));
    if (!peer)
      co_await weave::fail(peer.error());
    pg::Diagnostic diagnostic;
    auto connection = co_await weave::as_result(pg::connect(options, diagnostic));
    if (std::string_view{mode} == "decline-prefer") {
      if (!connection)
        co_await weave::fail(connection.error());
      co_await check(!connection->gss_encrypted());
      co_await connection->finish();
    } else {
      co_await check(!connection);
      co_await check(diagnostic.message().find("untrusted") == std::string_view::npos);
    }
    co_await std::move(*peer);
  }
}

static weave::Task<void> fallback_peer(weave::TcpListener &listener, weave::TlsContext credentials)
{
  auto socket = co_await listener.accept();
  std::array<std::byte, 8> request;
  co_await socket.read_exactly(request);
  wire::Reader gss{request};
  co_await check(gss.integer() == 8 && gss.integer() == 80877104);
  const std::array declined{std::byte{'N'}};
  co_await socket.write_all(declined);
  co_await socket.read_exactly(request);
  wire::Reader ssl{request};
  co_await check(ssl.integer() == 8 && ssl.integer() == 80877103);
  const std::array accepted{std::byte{'S'}};
  co_await socket.write_all(accepted);
  auto secured = co_await weave::tls::server(std::move(socket), credentials);
  std::array<std::byte, 4> header;
  co_await secured.read_exactly(header);
  wire::Reader length{header};
  wire::Bytes startup(length.integer() - 4);
  co_await secured.read_exactly(startup);
  wire::Writer reply;
  wire::Writer authentication;
  authentication.integer(0);
  reply.message('R', authentication);
  wire::Writer ready;
  ready.integer('I', 1);
  reply.message('Z', ready);
  co_await secured.write_all(reply.bytes);
  std::array<std::byte, 5> terminate;
  co_await secured.read_exactly(terminate);
  co_await check(terminate[0] == std::byte{'X'});
  co_await secured.shutdown();
}

static weave::Task<void> fallback(weave::Context &context, pg::GssContext provider)
{
  fixture::Certificates certificates;
  auto server = weave::TlsContext::server(
    {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key});
  if (!server)
    co_await weave::fail(server.error());
  auto listener = weave::tcp::listen(context, "127.0.0.1", 0);
  if (!listener)
    co_await weave::fail(listener.error());
  auto peer = context.spawn(fallback_peer(*listener, *server));
  if (!peer)
    co_await weave::fail(peer.error());
  pg::Options options{.host = "localhost", .port = listener->local_port(), .user = "client"};
  options.hosts = {{.name = "localhost", .port = options.port, .address = *weave::IpAddress::parse("127.0.0.1")}};
  options.gss = provider;
  options.gss_encryption = pg::GssEncryption::prefer;
  options.tls_options = weave::TlsClientOptions{.ca_file = certificates.ca};
  auto connection = co_await pg::connect(options);
  co_await check(!connection.gss_encrypted());
  co_await connection.finish();
  co_await std::move(*peer);
}

static weave::Task<void> stalled_peer(weave::TcpListener &listener)
{
  auto client = co_await listener.accept();
  std::array<std::byte, 8> request;
  co_await client.read_exactly(request);
  const std::array response{std::byte{'G'}};
  co_await client.write_all(response);
  auto token = co_await packet(client);
  co_await check(!token.empty());
  std::array<std::byte, 1> eof;
  co_await check(co_await client.read(eof) == 0);
}

static weave::Task<void> second_peer(weave::TcpListener &listener)
{
  auto client = co_await listener.accept();
}

static weave::Task<void> no_failover(weave::Context &context, pg::GssContext provider)
{
  auto stalled = weave::tcp::listen(context, "127.0.0.1", 0);
  auto other = weave::tcp::listen(context, "127.0.0.1", 0);
  if (!stalled || !other)
    co_await weave::fail(std::errc::io_error);
  auto first = context.spawn(stalled_peer(*stalled));
  auto second = context.spawn(second_peer(*other));
  if (!first || !second)
    co_await weave::fail(std::errc::io_error);
  pg::Options options{.user = "client", .plaintext = true, .connect_timeout = 100ms};
  auto address = *weave::IpAddress::parse("127.0.0.1");
  options.hosts = {
    {.name = "localhost", .port = stalled->local_port(), .address = address},
    {.name = "localhost", .port = other->local_port(), .address = address}};
  options.gss = provider;
  options.gss_encryption = pg::GssEncryption::prefer;
  auto connected = co_await weave::as_result(pg::connect(options));
  second->cancel();
  auto attempted = co_await weave::as_result(std::move(*second));
  co_await std::move(*first);
  if (connected)
    co_await check(false);
  std::fprintf(stderr, "connect=%s second-attempt=%d\n", connected.error().message().c_str(), attempted.has_value());
  co_await check(connected.error() == std::errc::timed_out);
  co_await check(!attempted && attempted.error() == std::errc::operation_canceled);
}

static weave::Task<void> encrypted_login(pg::Options options, bool full)
{
  auto connection = co_await pg::connect(options);
  co_await check(connection.gss_encrypted() && connection.authentication_method() == pg::Authentication::gss);
  auto status = co_await connection.execute("SELECT encrypted FROM pg_stat_gssapi WHERE pid=pg_backend_pid()");
  co_await check(status.rows.size() == 1 && status.rows[0][0].bytes() == "t");

  auto statement = "SELECT repeat('x',200000) /*" + std::string(100000, 'q') + "*/";
  auto large = co_await connection.execute(std::move(statement));
  co_await check(large.rows.size() == 1 && large.rows[0][0].bytes() == std::string(200000, 'x'));
  std::vector<pg::Command> commands;
  for (unsigned index = 0; index < 16; ++index)
    commands.push_back({.sql = "SELECT repeat('b',8192)"});
  auto batch = co_await connection.batch(std::move(commands));
  co_await check(batch.size() == 16);
  for (const auto &outcome : batch)
    co_await check(outcome.result && outcome.result->rows[0][0].bytes() == std::string(8192, 'b'));

  if (full) {
    auto parameter = std::string(300000, 'p');
    auto value = co_await connection.execute("SELECT $1::text", {{.data = parameter}});
    co_await check(value.rows.size() == 1 && value.rows[0][0].bytes() == parameter);
    co_await connection.execute("CREATE TEMP TABLE protected_copy(value text)");
    co_await connection.start_copy("COPY protected_copy FROM STDIN");
    auto lines = std::string(100000, 'c') + "\n" + std::string(100000, 'd') + "\n";
    co_await connection.write_copy(std::as_bytes(std::span{lines}));
    co_await connection.end_copy();
    co_await connection.start_copy("COPY protected_copy TO STDOUT");
    std::string copied;
    while (auto chunk = co_await connection.read_copy())
      copied.append(reinterpret_cast<const char *>(chunk->data()), chunk->size());
    co_await check(copied == lines);
    co_await connection.reset(options);
    co_await check(connection.gss_encrypted());
    co_await check(answer(co_await connection.execute("SELECT 42")));
  }
  co_await connection.finish();
}

static weave::Task<void> interrupted(pg::Connection &connection)
{
  auto result = co_await weave::as_result(connection.execute("SELECT pg_sleep(10)"));
  co_await check(!result && pg::sqlstate(result.error()) == "57014");
}

static weave::Task<void> dispatch(pg::CancelHandle handle)
{
  co_await weave::sleep_for(30ms);
  co_await handle.request();
}

static weave::Task<void> encrypted_cancel(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  auto handle = connection.cancel_handle();
  if (!handle)
    co_await weave::fail(handle.error());
  co_await weave::when_all(interrupted(connection), dispatch(*handle));
  co_await handle->request();
  auto pending = connection.request_cancel();
  co_await connection.reset(options);
  co_await std::move(pending);
  co_await check(answer(co_await connection.execute("SELECT 42")));
  co_await connection.finish();
}

static weave::Task<weave::Task<void>> owning_cancel(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  auto pending = connection.request_cancel();
  co_await connection.finish();
  co_return std::move(pending);
}

static weave::Task<void> rejected_login(pg::Options options, std::string mode)
{
  pg::Diagnostic diagnostic;
  auto result = co_await weave::as_result(pg::connect(options, diagnostic));
  co_await check(!result);
  if (mode == "missing" || mode == "wrong-service")
    co_await check(!diagnostic.message().empty());
}

static weave::Task<void> single_slot(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  co_await connection.reset(options);
  co_await check(answer(co_await connection.execute("SELECT 42")));
  if (auto closed = connection.close(); !closed)
    co_await weave::fail(closed.error());
  co_await connection.reset(options);
  co_await check(connection.gss_encrypted());
  co_await connection.finish();
}

using RecordCount = unsigned (*)();

static weave::Task<void> interrupt_record(weave::CancelSource &source, RecordCount count, unsigned baseline, bool stop)
{
  for (unsigned attempt = 0; attempt < 1000; ++attempt) {
    if (count() > baseline) {
      if (stop)
        weave::detail::current_context->request_stop();
      else
        source.cancel();
      co_return;
    }
    co_await weave::sleep_for(1ms);
  }
  source.cancel();
  co_await weave::fail(std::errc::timed_out);
}

static weave::Task<void> await_query(weave::Task<pg::ResultSet> query)
{
  co_await std::move(query);
  co_await weave::fail(std::errc::bad_message);
}

static weave::Task<void> record_cancellation(pg::Options options, std::string mode)
{
  auto connection = co_await pg::connect(options);
  const bool unwrap = mode.starts_with("unwrap-");
  const bool stop = mode.ends_with("stop");
  auto count = reinterpret_cast<RecordCount>(
    dlsym(RTLD_DEFAULT, unwrap ? "weave_test_gss_unwrapped" : "weave_test_gss_wrapped"));
  co_await check(count != nullptr);
  const auto baseline = count();
  weave::CancelSource source;
  auto query = connection.execute("SELECT repeat('x',200000)");
  weave::detail::TaskAccess::bind(query, source.token());
  const auto before = std::chrono::steady_clock::now();
  auto result = co_await weave::as_result(
    weave::when_all(await_query(std::move(query)), interrupt_record(source, count, baseline, stop)));
  co_await check(!result && result.error() == std::errc::operation_canceled);
  co_await check(std::chrono::steady_clock::now() - before >= 90ms);
  if (!stop) {
    co_await connection.reset(options);
    co_await check(answer(co_await connection.execute("SELECT 42")));
    co_await connection.finish();
  }
}

static int protected_suite(weave::u16 port, const char *cache)
{
  auto provider = pg::GssContext::create({.workers = 2, .capacity = 64, .credential_cache = cache});
  if (!provider)
    return weave::report_error(provider.error());
  pg::Options options{.host = "localhost", .port = port, .user = "client", .database = "postgres", .plaintext = true};
  options.hosts = {{.name = "localhost", .port = port, .address = *weave::IpAddress::parse("127.0.0.1")}};
  options.gss = *provider;
  options.gss_encryption = pg::GssEncryption::require;
  options.authentication.methods = {pg::Authentication::gss};
  {
    auto context = weave::Context::create();
    if (!context)
      return weave::report_error(context.error());
    auto result = context->run(encrypted_login(options, true));
    if (!result)
      return weave::report_error(result.error());
    result = context->run(encrypted_cancel(options));
    if (!result)
      return weave::report_error(result.error());
  }
  {
    pg::Diagnostic diagnostic;
    auto connection = pg::BlockingConnection::connect(options, diagnostic);
    if (!connection) {
      std::fprintf(stderr, "Blocking native diagnostic: %s\n", std::string(diagnostic.message()).c_str());
      return weave::report_error(connection.error());
    }
    if (!connection->gss_encrypted())
      return 1;
    auto result = connection->execute("SELECT 42");
    if (!result || !answer(*result))
      return 1;
    if (!connection->reset(options) || !connection->finish())
      return 1;
  }
  std::optional<weave::Task<void>> pending;
  {
    auto context = weave::Context::create();
    if (!context)
      return weave::report_error(context.error());
    auto owned = context->run(owning_cancel(options));
    if (!owned)
      return weave::report_error(owned.error());
    pending.emplace(std::move(*owned));
  }
  {
    auto context = weave::Context::create();
    if (!context)
      return weave::report_error(context.error());
    auto result = context->run(std::move(*pending));
    if (!result)
      return weave::report_error(result.error());
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    if (!runtime)
      return weave::report_error(runtime.error());
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 32; ++index) {
      auto job = runtime->spawn(encrypted_login(options, false));
      if (!job)
        return weave::report_error(job.error());
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!result)
        return weave::report_error(result.error());
    }
  }
#endif
  auto single = pg::GssContext::create({.workers = 1, .capacity = 1, .credential_cache = cache});
  if (!single)
    return weave::report_error(single.error());
  options.gss = *single;
  auto context = weave::Context::create();
  if (!context)
    return weave::report_error(context.error());
  auto result = context->run(single_slot(options));
  if (!result)
    return weave::report_error(result.error());
  std::puts("Protected real-server transfers, batching, COPY, owning cancellation and single-slot reset passed");
  return 0;
}

int main(int argc, char **argv)
{
  if (argc != 4)
    return 2;
  std::string mode = argv[2];
  if (mode.starts_with("enc-")) {
    mode.erase(0, 4);
    auto provider = pg::GssContext::create(
      {.workers = 2, .capacity = mode == "saturation" ? 2u : 64u, .credential_cache = argv[3]});
    if (!provider)
      return weave::report_error(provider.error());
    auto port = weave::parse_port(argv[1]);
    if (!port)
      return 2;
    pg::Options
      options{.host = "localhost", .port = *port, .user = "client", .database = "postgres", .plaintext = true};
    options.hosts = {{.name = "localhost", .port = *port, .address = *weave::IpAddress::parse("127.0.0.1")}};
    options.gss = *provider;
    options.gss_encryption = mode == "prefer" ? pg::GssEncryption::prefer : pg::GssEncryption::require;
    options.authentication.methods = {pg::Authentication::gss};
    if (mode == "tls-priority" || mode == "binding")
      options.plaintext = false;
    if (mode == "binding")
      options.channel_binding = pg::ChannelBinding::require;
    if (mode == "wrong-service")
      options.gss_service = "absent_service";
    if (mode == "policy")
      options.authentication.methods = {pg::Authentication::scram_sha256};
    if (mode == "none")
      options.authentication.methods = {pg::Authentication::none};
    if (mode == "excluded")
      options.authentication = {.methods = {pg::Authentication::gss}, .exclude = true};
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
    if (mode == "saturation" || mode == "runtime-cancel") {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = weave::Scheduler::work_stealing});
      if (!runtime)
        return weave::report_error(runtime.error());
      std::vector<weave::JoinHandle<void>> jobs;
      for (unsigned index = 0; index < 32; ++index) {
        auto job = runtime->spawn(
          mode == "runtime-cancel" ? cancellation(options, mode) : encrypted_login(options, false));
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
      return mode == "saturation" && (!rejected || !completed) ? 1 : 0;
    }
#endif
    auto context = weave::Context::create();
    if (!context)
      return weave::report_error(context.error());
    const bool cancelled = mode == "cancel" || mode == "pre-cancel" || mode == "context-stop";
    const bool rejected = mode == "missing" || mode == "wrong-service" || mode == "binding" || mode == "policy";
    if (mode == "normal")
      return protected_suite(*port, argv[3]);
    const bool record = mode.starts_with("wrap-") || mode.starts_with("unwrap-");
    auto operation = mode == "no-failover" ? no_failover(*context, *provider)
      : mode == "fallback"                 ? fallback(*context, *provider)
      : record                             ? record_cancellation(options, mode)
      : mode == "hostile"                  ? hostile_transport(*context, options)
      : cancelled                          ? cancellation(options, mode)
      : rejected                           ? rejected_login(options, mode)
                                           : encrypted_login(options, false);
    auto result = context->run(std::move(operation));
    if (mode == "context-stop" || mode == "wrap-stop" || mode == "unwrap-stop")
      return !result && result.error() == std::errc::operation_canceled ? 0 : 1;
    return result ? 0 : weave::report_error(result.error());
  }
  return 2;
}
