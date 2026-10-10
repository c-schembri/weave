#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/tls.hpp>
#include <weave/timer.hpp>
#include <weave/scope.hpp>
#include "tls_encrypted_certificates.hpp"
#ifdef WEAVE_TLS_PASSWORD_RUNTIME
#include <weave/runtime.hpp>
#include "runtime_fixture.hpp"
#endif

static void check(bool value)
{
  REQUIRE(value);
}

static weave::TlsClientOptions client_options(const fixture::Certificates &files, const std::string &key)
{
  weave::TlsClientOptions options;
  options.ca_file = files.ca;
  options.certificate_file = files.client;
  options.private_key_file = key;
  return options;
}

static weave::TlsServerOptions server_options(const fixture::Certificates &files, const std::string &key)
{
  weave::TlsServerOptions options;
  options.certificate_file = files.leaf;
  options.private_key_file = key;
  return options;
}

static weave::Task<void> echo(weave::TcpStream socket, const weave::TlsContext &credentials)
{
  auto stream = co_await weave::tls::server(std::move(socket), credentials);
  std::array<std::byte, 1024> buffer;
  while (auto size = co_await stream.read(buffer))
    co_await stream.write_all(std::span{buffer}.first(size));
  co_await stream.shutdown();
}

static weave::Task<void> echo_client(weave::u16 port, const weave::TlsContext &credentials)
{
  auto socket = co_await weave::tcp::connect("127.0.0.1", port);
  auto stream = co_await weave::tls::client(std::move(socket), credentials, "localhost");
  std::array<std::byte, 1024> sent;
  sent.fill(std::byte{59});
  std::array<std::byte, 1024> received;
  co_await stream.write_all(sent);
  co_await stream.read_exactly(received);
  check(received == sent);
  auto info = stream.info();
  check(info.has_value() && info->peer.has_value());
  co_await stream.shutdown();
}

static weave::Task<void> exchanges(const weave::TlsContext &server, const weave::TlsContext &client)
{
  co_await weave::scope([&](weave::TaskScope &children) -> weave::Task<void> {
    auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
    auto port = listener.local_port();
    for (int i = 0; i < 8; ++i) {
      auto job = children.spawn(echo_client(port, client));
      if (!job)
        co_await weave::fail(job.error());
    }
    for (int i = 0; i < 8; ++i) {
      auto socket = co_await listener.accept();
      auto job = children.spawn(echo(std::move(socket), server));
      if (!job)
        co_await weave::fail(job.error());
    }
    co_await children.join();
  });
}

static void streams(fixture::Certificates &files, const std::string &client_key, const std::string &server_key)
{
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};

  for (auto version : versions) {
    auto lifetime = std::make_shared<int>(1);
    std::weak_ptr<int> weak = lifetime;
    auto provider = weave::TlsPasswordProvider::create(
      [lifetime](std::string_view) noexcept -> weave::Result<std::string> {
        return std::string(127, 'p');
      });
    auto server = server_options(files, server_key);
    server.client_auth = weave::TlsClientAuth::required;
    server.ca_file = files.ca;
    server.min_version = server.max_version = version;
    server.private_key_password_provider = *provider;
    auto client = client_options(files, client_key);
    client.min_version = client.max_version = version;
    client.private_key_password_provider = std::move(*provider);
    lifetime.reset();
    auto accepting = weave::TlsContext::server(std::move(server));
    auto connecting = weave::TlsContext::client(std::move(client));
    check(accepting.has_value() && connecting.has_value());
    check(weak.expired());
    auto ctx = weave::Context::create();
    check(ctx.has_value());
    check(ctx->run(weave::timeout(std::chrono::seconds{20}, exchanges(*accepting, *connecting))).has_value());
#ifdef WEAVE_TLS_PASSWORD_RUNTIME
    const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
    for (auto layout : support::io_layouts) {
      for (auto scheduler : schedulers) {
        auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
        check(runtime.has_value());
        check(runtime->run(weave::timeout(std::chrono::seconds{20}, exchanges(*accepting, *connecting))).has_value());
      }
    }
#endif
  }
}

TEST_CASE("Provider-loaded mTLS credentials outlive their callbacks on Context and four-worker runtimes")
{
  std::filesystem::path directory;
  {
    fixture::EncryptedCertificates files{std::string(127, 'p')};
    directory = files.certificates.directory;
    streams(files.certificates, files.client_key, files.server_key);
  }
  CHECK_FALSE(std::filesystem::exists(directory));
}
