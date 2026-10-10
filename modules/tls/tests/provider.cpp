#include <doctest/doctest.h>
#include <weave/tls.hpp>
#include "tls_encrypted_certificates.hpp"
#include "credential_allocations.hpp"
#include <atomic>
#include <thread>

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

static void factories(fixture::Certificates &files, const std::string &client_key, const std::string &server_key)
{
  check(!weave::TlsPasswordProvider::create({}));
  std::atomic<int> calls{0};
  std::unique_ptr<fixture::CredentialAllocation> allocation;
  auto provider = weave::TlsPasswordProvider::create(
    [owned = std::make_unique<int>(127), &calls, &allocation](
      std::string_view file) noexcept -> weave::Result<std::string> {
      check(!file.empty());
      ++calls;
      std::string secret(static_cast<std::size_t>(*owned), 'p');
      allocation = std::make_unique<fixture::CredentialAllocation>(secret);
      return secret;
    });
  check(provider.has_value());

  auto client = client_options(files, client_key);
  client.private_key_password_provider = *provider;
  auto context = weave::TlsContext::client(client);
  check(context.has_value());
  check(calls == 1);
  check(allocation->cleansed());
  allocation.reset();
  auto server = server_options(files, server_key);
  server.private_key_password_provider = *provider;
  check(weave::TlsContext::server(server).has_value());
  check(calls == 2);
  check(allocation->cleansed());
  allocation.reset();

  client.private_key_file = files.client_key;
  check(weave::TlsContext::client(client).has_value());
  check(calls == 2);
  server.private_key_file = files.private_key;
  check(weave::TlsContext::server(server).has_value());
  check(calls == 2);

  client.private_key_file = client_key;
  client.private_key_password = "conflict";
  check(!weave::TlsContext::client(client));
  check(calls == 2);
  client.private_key_password.clear();
  client.private_key_file.push_back('\0');
  check(!weave::TlsContext::client(client));
  check(calls == 2);
  client.private_key_file.clear();
  client.certificate_file.clear();
  check(!weave::TlsContext::client(client));
  check(calls == 2);
  client = client_options(files, client_key);
  client.private_key_password_provider = *provider;
  client.revocation = static_cast<weave::TlsRevocation>(255);
  check(!weave::TlsContext::client(client));
  check(calls == 2);
  server.private_key_file = server_key;
  server.sessions.capacity = 0;
  check(!weave::TlsContext::server(server));
  check(calls == 2);

  auto moved = std::move(*provider);
  client = client_options(files, files.client_key);
  client.private_key_password_provider = *provider;
  check(!weave::TlsContext::client(client));
  check(calls == 2);

  const std::array errors{
    std::error_code{},
    std::make_error_code(std::errc::timed_out),
    std::make_error_code(std::errc::connection_reset),
    std::make_error_code(std::errc::invalid_argument)};
  for (auto error : errors) {
    auto failing = weave::TlsPasswordProvider::create([error](std::string_view) noexcept -> weave::Result<std::string> {
      return std::unexpected(error);
    });
    check(failing.has_value());
    client = client_options(files, client_key);
    client.private_key_password_provider = *failing;
    auto failed = weave::TlsContext::client(client);
    check(!failed && failed.error() == error);
    server = server_options(files, server_key);
    server.private_key_password_provider = *failing;
    auto server_failed = weave::TlsContext::server(server);
    check(!server_failed && server_failed.error() == error);
  }

  const std::array lengths{std::size_t{127}, std::size_t{4096}};
  for (auto length : lengths) {
    fixture::CredentialPattern pattern{"wwwwwwwwwwwwwwwwwwwwwwwwwwwwwwww"};
    auto wrong = weave::TlsPasswordProvider::create(
      [&allocation, length](std::string_view) noexcept -> weave::Result<std::string> {
        std::string secret(length, 'w');
        allocation = std::make_unique<fixture::CredentialAllocation>(secret);
        return secret;
      });
    client = client_options(files, client_key);
    client.private_key_password_provider = *wrong;
    auto failed = weave::TlsContext::client(client);
    check(!failed);
    if (length == 4096)
      check(failed.error() == weave::make_error_code(weave::TlsError::resource_limit));
    if (length < 4096)
      check(allocation->cleansed());
    check(pattern.dirty_releases() == 0);
    allocation.reset();
  }

  client = client_options(files, client_key);
  client.private_key_password = std::string(127, 'p');
  fixture::CredentialAllocation direct{client.private_key_password};
  check(weave::TlsContext::client(std::move(client)).has_value());
  check(direct.cleansed());

  std::atomic<int> concurrent_calls{0};
  auto shared = weave::TlsPasswordProvider::create(
    [&concurrent_calls](std::string_view) noexcept -> weave::Result<std::string> {
      ++concurrent_calls;
      return std::string(127, 'p');
    });
  auto task = [&]() {
    for (int i = 0; i < 16; ++i) {
      auto options = client_options(files, client_key);
      options.private_key_password_provider = *shared;
      check(weave::TlsContext::client(std::move(options)).has_value());
    }
  };
  std::array<std::thread, 4> threads;
  for (auto &thread : threads)
    thread = std::thread(task);
  for (auto &thread : threads)
    thread.join();
  check(concurrent_calls == 64);

  auto lifetime = std::make_shared<int>(1);
  std::weak_ptr<int> weak = lifetime;
  auto owned = weave::TlsPasswordProvider::create([lifetime](std::string_view) noexcept -> weave::Result<std::string> {
    return std::string(127, 'p');
  });
  client = client_options(files, client_key);
  client.private_key_password_provider = std::move(*owned);
  lifetime.reset();
  auto retained = weave::TlsContext::client(std::move(client));
  check(retained.has_value());
  check(weak.expired());

  const std::array passwords{
    std::string{},
    std::string{"short"},
    std::string{"left\0right", 10},
    std::string(2000, 'b')};
  for (const auto &password : passwords) {
    auto key = (files.directory / "edge-key.pem").string();
    fixture::EncryptedCertificates::encrypt(files.client_key, key, password);
    auto edge = weave::TlsPasswordProvider::create(
      [&password](std::string_view) noexcept -> weave::Result<std::string> {
        return password;
      });
    client = client_options(files, key);
    client.private_key_password_provider = *edge;
    auto decoded = weave::TlsContext::client(std::move(client));
    // Oversized secrets must fail without truncation, not decrypt using a prefix.
    if (password.size() == 2000)
      check(!decoded && decoded.error() == weave::make_error_code(weave::TlsError::resource_limit));
    else
      check(decoded.has_value());
  }
}

TEST_CASE("TLS private-key providers preserve ownership, errors, secret cleanup and native decoding")
{
  std::filesystem::path directory;
  {
    fixture::EncryptedCertificates files{std::string(127, 'p')};
    directory = files.certificates.directory;
    factories(files.certificates, files.client_key, files.server_key);
  }
  CHECK_FALSE(std::filesystem::exists(directory));
}
