#include <weave/tls.hpp>
#include <weave/tls/detail/engine.hpp>
#include "tls_certificates.hpp"
#include <openssl/pem.h>
#include <source_location>
#include <cstdio>
#include <cstdlib>

static int checks = 0;

static void check(bool value, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Engine metadata check failed: %s:%u\n", location.file_name(), location.line());
    std::exit(1);
  }
}

static std::vector<std::byte> certificate(const std::string &path)
{
  fixture::Bio file{BIO_new_file(path.c_str(), "rb"), BIO_free};
  check(static_cast<bool>(file));
  std::unique_ptr<X509, decltype(&X509_free)> cert{PEM_read_bio_X509(file.get(), nullptr, nullptr, nullptr), X509_free};
  check(static_cast<bool>(cert));
  auto size = i2d_X509(cert.get(), nullptr);
  check(size > 0);
  std::vector<std::byte> encoded(static_cast<std::size_t>(size));
  auto *output = reinterpret_cast<unsigned char *>(encoded.data());
  check(i2d_X509(cert.get(), &output) == size);
  return encoded;
}

static void identity(const weave::TlsPeerIdentity &peer, const std::vector<std::byte> &expected)
{
  check(peer.certificate == expected);
  check(!peer.subject.empty() && !peer.issuer.empty());
  std::array<std::byte, 32> digest;
  unsigned int size = 0;
  check(
    EVP_Digest(
      expected.data(),
      expected.size(),
      reinterpret_cast<unsigned char *>(digest.data()),
      &size,
      EVP_sha256(),
      nullptr) == 1);
  check(size == digest.size() && peer.sha256 == std::vector<std::byte>{digest.begin(), digest.end()});
}

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

static void resumed(
  fixture::Certificates &certificates,
  weave::TlsVersion version,
  weave::TlsSessionMode mode,
  const std::vector<std::byte> &leaf,
  const std::vector<std::byte> &client_leaf)
{
  auto server_context = weave::TlsContext::server(
    {.certificate_file = certificates.leaf,
      .private_key_file = certificates.private_key,
      .min_version = version,
      .max_version = version,
      .client_auth = weave::TlsClientAuth::required,
      .ca_file = certificates.ca,
      .sessions = {.mode = mode}});
  auto client_context = weave::TlsContext::client(
    {.ca_file = certificates.ca,
      .min_version = version,
      .max_version = version,
      .certificate_file = certificates.client,
      .private_key_file = certificates.client_key,
      .session_resumption = true});
  check(server_context && client_context);
  auto server = weave::detail::TlsEngine::create(*server_context, true, "");
  auto client = weave::detail::TlsEngine::create(*client_context, false, "localhost");
  check(server && client && handshake(*client, *server));
  std::array<std::byte, 1> byte{std::byte{1}};
  check(server->write(byte).action == weave::detail::TlsAction::ready);
  check(transfer(*server, *client));
  check(client->read(byte).action == weave::detail::TlsAction::ready);
  auto session = client->session();
  check(static_cast<bool>(session));
  auto resumed_server = weave::detail::TlsEngine::create(*server_context, true, "");
  auto resumed_client = weave::detail::TlsEngine::create(*client_context, false, "localhost", &*session);
  check(resumed_server && resumed_client && handshake(*resumed_client, *resumed_server));
  auto local = resumed_client->info();
  auto remote = resumed_server->info();
  check(local && remote && local->session_reused && remote->session_reused);
  check(local->version == version && remote->version == version);
  check(local->peer && remote->peer);
  identity(*local->peer, leaf);
  identity(*remote->peer, client_leaf);
  check(local->negotiated_protocol.empty() && remote->negotiated_protocol.empty());
}

int main()
{
  static fixture::Certificates certificates;
  std::printf("Owned certificate fixture: %s\n", certificates.directory.string().c_str());
  const auto leaf = certificate(certificates.leaf);
  const auto client_leaf = certificate(certificates.client);
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
  const std::array modes{weave::TlsClientAuth::none, weave::TlsClientAuth::optional, weave::TlsClientAuth::required};
  const std::array identities{false, true};
  for (auto version : versions) {
    for (auto mode : modes) {
      for (bool identified : identities) {
        auto server_context = weave::TlsContext::server(
          {.certificate_file = certificates.leaf,
            .private_key_file = certificates.private_key,
            .alpn = {"metadata"},
            .min_version = version,
            .max_version = version,
            .client_auth = mode,
            .ca_file = mode == weave::TlsClientAuth::none ? "" : certificates.ca});
        auto client_context = weave::TlsContext::client(
          {.ca_file = certificates.ca,
            .alpn = {"metadata"},
            .min_version = version,
            .max_version = version,
            .certificate_file = identified ? certificates.client : "",
            .private_key_file = identified ? certificates.client_key : ""});
        check(server_context && client_context);
        auto server = weave::detail::TlsEngine::create(*server_context, true, "");
        auto client = weave::detail::TlsEngine::create(*client_context, false, "localhost");
        check(server && client);
        check(!client->info() && !server->info());
        auto expected = mode != weave::TlsClientAuth::required || identified;
        check(handshake(*client, *server) == expected);
        if (!expected) {
          check(!client->info() || !server->info());
          continue;
        }
        auto info = client->info();
        auto remote = server->info();
        check(info && remote);
        check(info->library == "OpenSSL" && info->version == version);
        check(info->cipher == client->cipher() && info->cipher == remote->cipher);
        check(info->key_bits >= 128 && info->key_bits == remote->key_bits && !info->compression);
        check(info->negotiated_protocol == "metadata" && remote->negotiated_protocol == "metadata");
        check(!info->session_reused && static_cast<bool>(info->peer));
        identity(*info->peer, leaf);
        check(static_cast<bool>(remote->peer) == (identified && mode != weave::TlsClientAuth::none));
        if (remote->peer)
          identity(*remote->peer, client_leaf);
        auto original = client->peer_identity();
        check(original && original->certificate == info->peer->certificate && original->sha256 == info->peer->sha256);
        auto retained = *info;
        info->cipher = "changed";
        check(client->info()->cipher == retained.cipher);
        client->fail(std::make_error_code(std::errc::operation_canceled));
        check(!client->info() && client->info().error() == std::errc::operation_canceled);
        auto moved = std::move(*client);
        check(!client->info() && client->info().error() == weave::make_error_code(weave::TlsError::closed));
        identity(*retained.peer, leaf);
      }
    }
  }
  const std::array sessions{weave::TlsSessionMode::stateful, weave::TlsSessionMode::tickets};
  for (auto version : versions) {
    for (auto mode : sessions)
      resumed(certificates, version, mode, leaf, client_leaf);
  }
  std::printf("TLS metadata controls passed: %d checks\n", checks);
}
