#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/tls.hpp>
#include <weave/tls/detail/engine.hpp>
#include "tls_certificates.hpp"
#include <fstream>
#include <sstream>
#include <thread>
#include <atomic>
#include <algorithm>

static bool transfer(weave::detail::TlsEngine &from, weave::detail::TlsEngine &to)
{
  std::array<std::byte, 32768> buffer;
  for (;;) {
    auto count = from.output(buffer);
    if (!count)
      return false;
    if (!*count)
      return true;
    if (!to.input(std::span{buffer}.first(*count)))
      return false;
  }
}

static bool handshake(weave::detail::TlsEngine &client, weave::detail::TlsEngine &server)
{
  bool connected = false;
  bool accepted = false;
  for (int index = 0; index < 100; ++index) {
    if (!connected) {
      auto step = client.handshake();
      if (step.action == weave::detail::TlsAction::failed)
        return false;
      connected = step.action == weave::detail::TlsAction::ready;
    }
    if (!transfer(client, server))
      return false;
    if (!accepted) {
      auto step = server.handshake();
      if (step.action == weave::detail::TlsAction::failed)
        return false;
      accepted = step.action == weave::detail::TlsAction::ready;
    }
    if (!transfer(server, client))
      return false;
    if (connected && accepted)
      return true;
  }
  return false;
}

static bool hex(std::string_view text)
{
  return !text.empty() && text.size() % 2 == 0 && std::ranges::all_of(text, [](char byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
  });
}

TEST_CASE("TLS verification is explicit and unverified certificates are not authenticated identities")
{
  fixture::Certificates files;
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
  const std::array policies{
    weave::TlsVerification::none,
    weave::TlsVerification::certificate,
    weave::TlsVerification::hostname};
  for (auto version : versions) {
    for (auto policy : policies) {
      weave::TlsClientOptions options;
      options.ca_file = files.ca;
      options.min_version = options.max_version = version;
      options.verification = policy;
      auto context = weave::TlsContext::client(options);
      REQUIRE(context);
      CHECK(context->verification() == policy);
      auto server = weave::TlsContext::server(
        {.certificate_file = files.leaf,
          .private_key_file = files.private_key,
          .min_version = version,
          .max_version = version});
      REQUIRE(server);
      auto client = weave::detail::TlsEngine::create(*context, false, "wrong.invalid");
      auto peer = weave::detail::TlsEngine::create(*server, true, "");
      REQUIRE(client);
      REQUIRE(peer);
      const bool ready = handshake(*client, *peer);
      CHECK(ready == (policy != weave::TlsVerification::hostname));
      if (ready) {
        auto info = client->info();
        REQUIRE(info);
        CHECK_FALSE(info->hostname_verified);
        CHECK(info->certificate_verified == (policy != weave::TlsVerification::none));
        CHECK(info->peer.has_value() == (policy != weave::TlsVerification::none));
        CHECK(client->peer_identity().has_value() == (policy != weave::TlsVerification::none));
        CHECK(client->channel_binding().has_value());
      }
    }
  }
}

TEST_CASE("TLS trust failures remain errors in certificate-only and hostname modes")
{
  fixture::Certificates files;
  auto server = weave::TlsContext::server({.certificate_file = files.leaf, .private_key_file = files.private_key});
  REQUIRE(server);
  CHECK(server->verification() == weave::TlsVerification::none);
  auto authenticated = weave::TlsContext::server(
    {.certificate_file = files.leaf,
      .private_key_file = files.private_key,
      .client_auth = weave::TlsClientAuth::required,
      .ca_file = files.ca});
  REQUIRE(authenticated);
  CHECK(authenticated->verification() == weave::TlsVerification::certificate);
  const std::array policies{weave::TlsVerification::certificate, weave::TlsVerification::hostname};
  for (auto policy : policies) {
    auto context = weave::TlsContext::client({.ca_file = files.untrusted, .verification = policy});
    REQUIRE(context);
    auto client = weave::detail::TlsEngine::create(*context, false, "localhost");
    auto peer = weave::detail::TlsEngine::create(*server, true, "");
    REQUIRE(client);
    REQUIRE(peer);
    CHECK_FALSE(handshake(*client, *peer));
    CHECK(client->handshake().error == weave::TlsError::certificate_verification);
  }
  CHECK_FALSE(
    weave::TlsContext::client(
      {.revocation = weave::TlsRevocation::chain, .verification = weave::TlsVerification::none}));
  CHECK_FALSE(weave::TlsContext::client({.session_resumption = true, .verification = weave::TlsVerification::none}));
  CHECK_FALSE(weave::TlsContext::client({.verification = static_cast<weave::TlsVerification>(99)}));
}

TEST_CASE("TLS traffic keys are opt-in private append-only NSS records across concurrent handshakes")
{
  fixture::Certificates files;
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
  for (auto version : versions) {
    const auto path = (files.directory / "traffic.keys").string();
    const auto server_path = (files.directory / "server.keys").string();
    {
      auto client = weave::TlsContext::client(
        {.ca_file = files.ca, .min_version = version, .max_version = version, .key_log_file = path});
      auto server = weave::TlsContext::server(
        {.certificate_file = files.leaf,
          .private_key_file = files.private_key,
          .min_version = version,
          .max_version = version,
          .key_log_file = server_path});
      REQUIRE(client);
      REQUIRE(server);
      auto second = weave::TlsContext::client(
        {.ca_file = files.ca, .min_version = version, .max_version = version, .key_log_file = path});
      REQUIRE(second);
      std::atomic<unsigned> completed{0};
      std::vector<std::thread> threads;
      for (int index = 0; index < 8; ++index) {
        threads.emplace_back([&, index] {
          auto connection = weave::detail::TlsEngine::create(index % 2 ? *client : *second, false, "localhost");
          auto peer = weave::detail::TlsEngine::create(*server, true, "");
          if (connection && peer && handshake(*connection, *peer))
            ++completed;
        });
      }
      for (auto &thread : threads)
        thread.join();
      CHECK(completed == 8);
    }
    const std::array paths{path, server_path};
    for (const auto &file : paths) {
      std::ifstream input(file);
      REQUIRE(input);
      unsigned records = 0;
      std::string line;
      while (std::getline(input, line)) {
        std::istringstream record(line);
        std::string label, random, secret, extra;
        REQUIRE(static_cast<bool>(record >> label >> random >> secret));
        CHECK_FALSE(static_cast<bool>(record >> extra));
        CHECK(
          (version == weave::TlsVersion::tls12 ? label == "CLIENT_RANDOM" : label.find("SECRET") != std::string::npos));
        CHECK(random.size() == 64);
        CHECK(hex(random));
        CHECK(hex(secret));
        ++records;
      }
      CHECK(records >= (version == weave::TlsVersion::tls12 ? 8u : 8u * 5));
      input.close();
      std::error_code error;
      CHECK(std::filesystem::remove(file, error));
      CHECK_FALSE(error);
    }
    CHECK_FALSE(weave::TlsContext::client({.key_log_file = files.directory.string()}));
    CHECK_FALSE(weave::TlsContext::client({.key_log_file = std::string{"bad\0path", 8}}));
#ifndef _WIN32
    const auto unsafe = files.directory / "unsafe.keys";
    {
      std::ofstream output(unsafe);
    }
    std::error_code error;
    std::filesystem::permissions(
      unsafe,
      std::filesystem::perms::owner_all | std::filesystem::perms::group_read,
      std::filesystem::perm_options::replace,
      error);
    REQUIRE_FALSE(error);
    CHECK_FALSE(weave::TlsContext::client({.key_log_file = unsafe.string()}));
    CHECK(std::filesystem::remove(unsafe, error));
    CHECK_FALSE(error);
#endif
  }
}
