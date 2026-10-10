#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/postgres.hpp>
#include <weave/timer.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include "credential_allocations.hpp"
#include "options.hpp"
#include "wire.hpp"
#include "auth.hpp"
#include <array>
#include <memory>

namespace pg = weave::pg;
namespace wire = weave::pg::detail;
using namespace std::chrono_literals;

static_assert(sizeof(wire::OwnedOptions) == sizeof(pg::Options));
static_assert(!std::is_copy_constructible_v<wire::OwnedOptions>);
static_assert(std::is_nothrow_move_constructible_v<wire::OwnedOptions>);

static pg::Options secret_options()
{
  pg::Options options;
  options.password = std::string(127, 'p');
  std::array<std::byte, 32> client_bytes, server_bytes;
  client_bytes.fill(std::byte{17});
  server_bytes.fill(std::byte{23});
  options.scram_client_key.emplace(client_bytes);
  options.scram_server_key.emplace(server_bytes);
  options.hosts.push_back({.name = "localhost", .password = std::string(137, 'h')});
  options.tls_options.emplace();
  options.tls_options->private_key_password = std::string(147, 't');
  return options;
}

static const std::string client_pattern(32, char{17});
static const std::string server_pattern(32, char{23});

struct Credentials {
  pg::Options options = secret_options();
  fixture::CredentialAllocation password{options.password};
  fixture::CredentialAllocation host{*options.hosts.front().password};
  fixture::CredentialAllocation key{options.tls_options->private_key_password};
  fixture::CredentialPattern client_key{client_pattern};
  fixture::CredentialPattern server_key{server_pattern};

  bool cleansed() const noexcept
  {
    return password.cleansed() && host.cleansed() && key.cleansed() && client_key.dirty_releases() == 0 &&
      server_key.dirty_releases() == 0;
  }
};

static weave::Task<pg::Connection> join(weave::JoinHandle<pg::Connection> job)
{
  co_return co_await std::move(job);
}

static weave::Task<pg::ServerStatus> join_probe(weave::JoinHandle<pg::ServerStatus> job)
{
  co_return co_await std::move(job);
}

static pg::ConnectionReport report_sentinel()
{
  return {.error = std::make_error_code(std::errc::permission_denied), .truncated = true};
}

TEST_CASE("Dropped reported factories cleanse captured credentials without mutating output")
{
  Credentials secrets;
  auto report = report_sentinel();
  {
    auto task = pg::connect(std::move(secrets.options), report);
  }
  CHECK(secrets.cleansed());
  CHECK_FALSE(report.completed);
  CHECK(report.truncated);
  CHECK(report.error == std::errc::permission_denied);
}

TEST_CASE("Reported validation and pre-start cancellation retain their credential and output contracts")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  Credentials invalid;
  auto report = report_sentinel();
  auto rejected = ctx->run(pg::connect(std::move(invalid.options), report));
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == std::errc::invalid_argument);
  CHECK(invalid.cleansed());
  CHECK(report.completed);
  CHECK(report.error == rejected.error());
  REQUIRE(report.attempts.size() == 1);
  CHECK(report.attempts[0].stage == pg::ConnectionStage::validation);

  Credentials cancelled;
  cancelled.options.user = "test";
  auto untouched = report_sentinel();
  weave::CancelSource cancellation;
  cancellation.cancel();
  auto job = ctx->spawn(pg::connect(std::move(cancelled.options), untouched), {.cancel = cancellation.token()});
  REQUIRE(job);
  auto result = ctx->run(join(std::move(*job)));
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
  CHECK(cancelled.cleansed());
  CHECK_FALSE(untouched.completed);
  CHECK(untouched.truncated);
  CHECK(untouched.error == std::errc::permission_denied);
}

TEST_CASE("Rejected reported Context submissions cleanse credentials without modifying output")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  ctx->shutdown();
  Credentials secrets;
  auto report = report_sentinel();
  auto rejected = ctx->spawn(pg::connect(std::move(secrets.options), report));
  CHECK_FALSE(rejected);
  CHECK(secrets.cleansed());
  CHECK_FALSE(report.completed);
  CHECK(report.truncated);
  CHECK(report.error == std::errc::permission_denied);
}

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
TEST_CASE("Rejected reported Runtime submissions cleanse credentials without modifying output")
{
  auto runtime = weave::Runtime::create({.workers = 4});
  REQUIRE(runtime);
  runtime->shutdown();
  Credentials secrets;
  auto report = report_sentinel();
  auto rejected = runtime->spawn(pg::connect(std::move(secrets.options), report));
  CHECK_FALSE(rejected);
  CHECK(secrets.cleansed());
  CHECK_FALSE(report.completed);
  CHECK(report.truncated);
  CHECK(report.error == std::errc::permission_denied);
}
#endif

TEST_CASE("Unstarted PostgreSQL availability Tasks own credential cleanup")
{
  Credentials secrets;
  {
    auto task = pg::ping(std::move(secrets.options));
  }
  CHECK(secrets.cleansed());
}

TEST_CASE("Availability probes cleanse credentials on validation and pre-start cancellation")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  Credentials invalid;
  auto rejected = ctx->run(pg::ping(std::move(invalid.options)));
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == std::errc::invalid_argument);
  CHECK(invalid.cleansed());

  Credentials cancelled;
  cancelled.options.user = "test";
  weave::CancelSource cancellation;
  cancellation.cancel();
  auto job = ctx->spawn(pg::ping(std::move(cancelled.options)), {.cancel = cancellation.token()});
  REQUIRE(job);
  auto result = ctx->run(join_probe(std::move(*job)));
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
  CHECK(cancelled.cleansed());
}

TEST_CASE("Rejected Context submissions cleanse unstarted PostgreSQL availability Tasks")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  ctx->shutdown();
  Credentials secrets;
  auto rejected = ctx->spawn(pg::ping(std::move(secrets.options)));
  CHECK_FALSE(rejected);
  CHECK(secrets.cleansed());
}

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
TEST_CASE("Rejected Runtime submissions cleanse unstarted PostgreSQL availability Tasks")
{
  auto runtime = weave::Runtime::create({.workers = 4});
  REQUIRE(runtime);
  runtime->shutdown();
  Credentials secrets;
  auto rejected = runtime->spawn(pg::ping(std::move(secrets.options)));
  CHECK_FALSE(rejected);
  CHECK(secrets.cleansed());
}
#endif

TEST_CASE("Blocking availability probes cleanse credentials on validation failure")
{
  Credentials secrets;
  auto rejected = pg::ping_blocking(std::move(secrets.options));
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == std::errc::invalid_argument);
  CHECK(secrets.cleansed());
}

TEST_CASE("SCRAM key destruction cleanses owned allocations before release")
{
  std::array<std::byte, 32> raw;
  raw.fill(std::byte{37});
  auto key = std::make_unique<pg::ScramKey>(raw);
  auto bytes = key->bytes();
  fixture::CredentialAllocation witness{{reinterpret_cast<const char *>(bytes.data()), bytes.size()}};
  key.reset();
  CHECK(witness.cleansed());
}

TEST_CASE("Unstarted PostgreSQL connect Tasks own credential cleanup")
{
  const std::array diagnostics{false, true};
  for (bool observed : diagnostics) {
    Credentials secrets;
    pg::Diagnostic diagnostic;
    {
      auto task = observed ? pg::connect(std::move(secrets.options), diagnostic)
                           : pg::connect(std::move(secrets.options));
    }
    CHECK(secrets.cleansed());
  }
}

TEST_CASE("PostgreSQL credentials are cleansed on validation and pre-start cancellation")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  Credentials invalid;
  auto rejected = ctx->run(pg::connect(std::move(invalid.options)));
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == std::errc::invalid_argument);
  CHECK(invalid.cleansed());

  Credentials cancelled;
  cancelled.options.user = "test";
  weave::CancelSource cancellation;
  cancellation.cancel();
  auto job = ctx->spawn(pg::connect(std::move(cancelled.options)), {.cancel = cancellation.token()});
  REQUIRE(job);
  auto result = ctx->run(join(std::move(*job)));
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
  CHECK(cancelled.cleansed());
}

TEST_CASE("Rejected Context submissions destroy and cleanse unstarted PostgreSQL Tasks")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  ctx->shutdown();
  Credentials secrets;
  auto rejected = ctx->spawn(pg::connect(std::move(secrets.options)));
  CHECK_FALSE(rejected);
  CHECK(secrets.cleansed());
}

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
TEST_CASE("Rejected Runtime submissions destroy and cleanse unstarted PostgreSQL Tasks")
{
  auto runtime = weave::Runtime::create({.workers = 4});
  REQUIRE(runtime);
  runtime->shutdown();
  Credentials secrets;
  auto rejected = runtime->spawn(pg::connect(std::move(secrets.options)));
  CHECK_FALSE(rejected);
  CHECK(secrets.cleansed());
}
#endif

TEST_CASE("Blocking PostgreSQL setup cleanses credentials on validation failure")
{
  Credentials secrets;
  auto rejected = pg::BlockingConnection::connect(std::move(secrets.options));
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == std::errc::invalid_argument);
  CHECK(secrets.cleansed());
}

static weave::Task<void> peer(weave::TcpListener &listener)
{
  auto socket = co_await listener.accept();
  std::array<std::byte, 4> header;
  co_await socket.read_exactly(header);
  wire::Reader reader{header};
  auto size = reader.integer();
  if (size < 8 || size > 65536)
    co_await weave::fail(std::errc::bad_message);

  wire::Bytes body(size - 4);
  co_await socket.read_exactly(body);
  wire::Writer authentication;
  authentication.integer(0);
  wire::Writer ready;
  ready.integer('I', 1);
  wire::Writer response;
  response.message('R', authentication);
  response.message('Z', ready);
  co_await socket.write_all(response.bytes);
  static_cast<void>(co_await weave::as_result(socket.read(header)));
}

static weave::Task<void> reset_case(weave::Context &ctx, bool observed, bool execute)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
  auto server = ctx.spawn(peer(listener));
  if (!server)
    co_await weave::fail(server.error());

  pg::Options initial;
  initial.host = "127.0.0.1";
  initial.port = listener.local_port();
  initial.user = "test";
  initial.plaintext = true;
  auto connection = co_await pg::connect(std::move(initial));
  Credentials secrets;
  pg::Diagnostic diagnostic;
  {
    auto task = observed ? connection.reset(std::move(secrets.options), diagnostic)
                         : connection.reset(std::move(secrets.options));
    if (execute) {
      auto invalid = co_await weave::as_result(std::move(task));
      if (invalid || invalid.error() != std::errc::invalid_argument)
        co_await weave::fail(std::errc::bad_message);
    }
  }
  CHECK(secrets.cleansed());

  if (auto closed = connection.close(); !closed)
    co_await weave::fail(closed.error());
  co_await std::move(*server);
}

TEST_CASE("PostgreSQL reset cleanses credentials for dropped and invalid Tasks in both overloads")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  const std::array flags{false, true};
  for (bool observed : flags) {
    for (bool execute : flags) {
      CAPTURE(observed);
      CAPTURE(execute);
      auto result = ctx->run(weave::timeout(5s, reset_case(*ctx, observed, execute)));
      REQUIRE(result);
    }
  }
}

static bool inline_cleansed(const std::string &text, const char *previous, std::size_t size)
{
  auto address = reinterpret_cast<std::uintptr_t>(std::addressof(text));
  auto pointer = reinterpret_cast<std::uintptr_t>(previous);
  if (pointer < address || pointer - address > sizeof(text) || size > sizeof(text) - (pointer - address))
    return false;

  // Only inspect known character bytes in a still-live object, never freed storage or padding.
  auto *representation = reinterpret_cast<const unsigned char *>(std::addressof(text));
  for (std::size_t index = 0; index < size; ++index) {
    if (representation[pointer - address + index] != 0)
      return false;
  }
  return true;
}

TEST_CASE("Owned PostgreSQL options preserve values and cleanse retained inline move sources")
{
  pg::Options options;
  options.password = "password";
  options.tls_options.emplace();
  options.tls_options->private_key_password = "passphrase";
  options.tls_options->private_key_file = "uri:secret";
  options.tls_options->private_key_format = weave::TlsPrivateKeyFormat::store;
  auto *password = options.password.data();
  auto *key = options.tls_options->private_key_password.data();
  auto *locator = options.tls_options->private_key_file.data();
  wire::OwnedOptions first{std::move(options)};
  CHECK(inline_cleansed(options.password, password, 8));
  CHECK(inline_cleansed(options.tls_options->private_key_password, key, 10));
  CHECK(inline_cleansed(options.tls_options->private_key_file, locator, 10));

  password = first.value.password.data();
  key = first.value.tls_options->private_key_password.data();
  locator = first.value.tls_options->private_key_file.data();
  wire::OwnedOptions second{std::move(first)};
  CHECK(inline_cleansed(first.value.password, password, 8));
  CHECK(inline_cleansed(first.value.tls_options->private_key_password, key, 10));
  CHECK(inline_cleansed(first.value.tls_options->private_key_file, locator, 10));

  password = second.value.password.data();
  key = second.value.tls_options->private_key_password.data();
  locator = second.value.tls_options->private_key_file.data();
  auto result = std::move(second).take();
  wire::OptionsCleanup cleanup{result};
  CHECK(result.password == "password");
  CHECK(result.tls_options->private_key_password == "passphrase");
  CHECK(result.tls_options->private_key_file == "uri:secret");
  CHECK(result.info().tls_options->private_key_file.empty());
  CHECK(inline_cleansed(second.value.password, password, 8));
  CHECK(inline_cleansed(second.value.tls_options->private_key_password, key, 10));
  CHECK(inline_cleansed(second.value.tls_options->private_key_file, locator, 10));
}

TEST_CASE("Credential pattern observation detects an uncleansed live allocation before release")
{
  const std::string_view marker = "credential-witness-0123456789-credential-witness-0123456789";
  fixture::CredentialPattern observer{marker};
  {
    std::string ordinary{marker};
    CHECK(ordinary == marker);
    CHECK(observer.dirty_releases() == 0);
  }
  CHECK(observer.dirty_releases() == 1);
}

TEST_CASE("Authentication storage cleanses growth, discarded capacity and final release")
{
  wire::SecretText value(64, 'p');
  fixture::CredentialAllocation grown{{value.data(), value.size()}};
  value.reserve(value.capacity() + 1);
  value.push_back('x');
  CHECK(grown.cleansed());
  CHECK(value.front() == 'p');
  CHECK(value.back() == 'x');

  fixture::CredentialAllocation discarded{{value.data(), value.size()}};
  value.resize(4);
  {
    auto owned = std::move(value);
    CHECK(owned.size() == 4);
  }
  CHECK(discarded.cleansed());
}

TEST_CASE("MD5 authentication releases no plaintext or intermediate hex copies")
{
  const std::string_view password = "secret-password-0123456789-secret-password-0123456789";
  const std::string_view first = "d0889bdf379b56b2b76cbd367ce3c6ee";
  const std::string_view second = "158fd916a2ec5349afe59da6c1795792";
  fixture::CredentialPattern plaintext{password};
  fixture::CredentialPattern first_hash{first};
  fixture::CredentialPattern second_hash{second};
  {
    std::array<std::byte, 4> salt{};
    auto response = wire::md5_password(std::string(137, 'u'), password, salt);
    REQUIRE(response);
    CHECK((std::string_view{response->data(), response->size()} == "md5158fd916a2ec5349afe59da6c1795792"));
    auto invalid = wire::md5_password("user", password, {});
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error() == pg::Error::protocol);
  }
  CHECK(plaintext.dirty_releases() == 0);
  CHECK(first_hash.dirty_releases() == 0);
  CHECK(second_hash.dirty_releases() == 0);
}

TEST_CASE("Authentication writers cleanse terminator growth and packet destruction")
{
  static_assert(std::is_same_v<decltype(wire::Writer{}.bytes), wire::Bytes>);
  const std::string_view password = "secret-password-0123456789-secret-password-0123456789";
  fixture::CredentialPattern plaintext{password};
  {
    wire::SecretWriter body;
    body.string(password);
    wire::SecretWriter packet;
    packet.message('p', body);
    wire::Reader reader{packet.bytes};
    CHECK(reader.integer(1) == 'p');
    CHECK(reader.integer() == password.size() + 5);
    auto encoded = reader.take(password.size() + 1);
    REQUIRE(encoded.size() == password.size() + 1);
    CHECK((std::string_view{reinterpret_cast<const char *>(encoded.data()), password.size()} == password));
    CHECK(encoded.back() == std::byte{});
    CHECK(reader.empty());
  }
  CHECK(plaintext.dirty_releases() == 0);
}

TEST_CASE("SCRAM setup and rejected challenges cleanse normalized password storage")
{
  const std::string_view password = "secret-password-0123456789-secret-password-0123456789";
  pg::Options options;
  options.password = password;
  fixture::CredentialPattern plaintext{password};
  {
    wire::Scram scram;
    const std::string_view mechanisms{"SCRAM-SHA-256\0\0", 15};
    auto response = scram.start(options, std::as_bytes(std::span{mechanisms.data(), mechanisms.size()}), {});
    REQUIRE(response);
    const std::string_view challenge = "r=invalid,s=MTIzNA==,i=4096";
    auto rejected = scram.challenge(std::as_bytes(std::span{challenge.data(), challenge.size()}), 4096);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error() == pg::Error::authentication);
  }
  CHECK(plaintext.dirty_releases() == 0);
}

TEST_CASE("Cryptographic arrays cleanse moves, replaced values and destruction")
{
  using Secret = wire::SecretArray<32>;
  static_assert(!std::is_copy_constructible_v<Secret>);
  Secret first;
  first.bytes.fill(42);
  Secret second{std::move(first)};
  CHECK(std::ranges::all_of(first.bytes, [](auto value) {
    return value == 0;
  }));
  CHECK(std::ranges::all_of(second.bytes, [](auto value) {
    return value == 42;
  }));

  first.bytes.fill(7);
  first = std::move(second);
  CHECK(std::ranges::all_of(first.bytes, [](auto value) {
    return value == 42;
  }));
  CHECK(std::ranges::all_of(second.bytes, [](auto value) {
    return value == 0;
  }));

  alignas(Secret) std::array<unsigned char, sizeof(Secret)> storage{};
  auto *secret = std::construct_at(reinterpret_cast<Secret *>(storage.data()));
  secret->bytes.fill(31);
  std::destroy_at(secret);
  CHECK(std::ranges::all_of(storage, [](auto byte) {
    return byte == 0;
  }));
}
