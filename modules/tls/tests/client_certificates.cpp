#include <weave/tls.hpp>
#include <weave/tls/detail/engine.hpp>
#include "tls_certificates.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <atomic>
#include <thread>
#include <source_location>
#include <cstdio>

static std::atomic<unsigned> checks{0};

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Certificate policy check failed: %s:%u\n", where.file_name(), where.line());
    std::exit(1);
  }
}

using NativeContext = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using Native = std::unique_ptr<SSL, decltype(&SSL_free)>;

static int acknowledge(SSL *, int *, void *)
{
  return SSL_TLSEXT_ERR_OK;
}

static NativeContext server(
  const fixture::Certificates &files,
  weave::TlsVersion version,
  bool request,
  bool incompatible,
  weave::TlsSessionMode sessions)
{
  NativeContext ctx{SSL_CTX_new(TLS_server_method()), SSL_CTX_free};
  check(ctx != nullptr);
  const int protocol = version == weave::TlsVersion::tls12 ? TLS1_2_VERSION : TLS1_3_VERSION;
  check(SSL_CTX_set_min_proto_version(ctx.get(), protocol) == 1);
  check(SSL_CTX_set_max_proto_version(ctx.get(), protocol) == 1);
  check(SSL_CTX_use_certificate_chain_file(ctx.get(), files.leaf.c_str()) == 1);
  check(SSL_CTX_use_PrivateKey_file(ctx.get(), files.private_key.c_str(), SSL_FILETYPE_PEM) == 1);
  check(SSL_CTX_load_verify_locations(ctx.get(), files.ca.c_str(), nullptr) == 1);
  SSL_CTX_set_verify(ctx.get(), request ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
  if (incompatible)
    check(SSL_CTX_set1_client_sigalgs_list(ctx.get(), "rsa_pss_rsae_sha256") == 1);
  const std::array<unsigned char, 3> identity{7, 8, 9};
  check(SSL_CTX_set_session_id_context(ctx.get(), identity.data(), static_cast<unsigned>(identity.size())) == 1);
  SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_SERVER);
  if (sessions == weave::TlsSessionMode::stateful)
    SSL_CTX_set_options(ctx.get(), SSL_OP_NO_TICKET);
  check(SSL_CTX_set_num_tickets(ctx.get(), 1) == 1);
  SSL_CTX_set_tlsext_servername_callback(ctx.get(), acknowledge);
  return ctx;
}

static Native peer(SSL_CTX *ctx)
{
  Native ssl{SSL_new(ctx), SSL_free};
  check(ssl != nullptr);
  auto *input = BIO_new(BIO_s_mem());
  auto *output = BIO_new(BIO_s_mem());
  check(input != nullptr && output != nullptr);
  BIO_set_mem_eof_return(input, -1);
  SSL_set_bio(ssl.get(), input, output);
  SSL_set_accept_state(ssl.get());
  return ssl;
}

static void transfer(weave::detail::TlsEngine &client, SSL *server)
{
  std::array<std::byte, 32768> buffer;
  for (;;) {
    auto size = client.output(buffer);
    check(size.has_value());
    if (!*size)
      return;
    check(BIO_write(SSL_get_rbio(server), buffer.data(), static_cast<int>(*size)) == static_cast<int>(*size));
  }
}

static void transfer(SSL *server, weave::detail::TlsEngine &client)
{
  std::array<std::byte, 32768> buffer;
  while (BIO_ctrl_pending(SSL_get_wbio(server))) {
    const int size = BIO_read(SSL_get_wbio(server), buffer.data(), static_cast<int>(buffer.size()));
    check(size > 0);
    check(client.input(std::span{buffer}.first(static_cast<std::size_t>(size))).has_value());
  }
}

struct Handshake {
  bool client = false;
  bool server = false;
  std::error_code error;
};

static Handshake handshake(weave::detail::TlsEngine &client, SSL *server)
{
  Handshake state;
  for (unsigned i = 0; i < 100; ++i) {
    if (!state.client) {
      const auto step = client.handshake();
      if (step.action == weave::detail::TlsAction::failed) {
        state.error = step.error;
        std::array<std::byte, 1> buffer;
        auto output = client.output(buffer);
        check(!output && output.error() == state.error);
        return state;
      }
      state.client = step.action == weave::detail::TlsAction::ready;
    }
    transfer(client, server);
    if (!state.server) {
      ERR_clear_error();
      const int result = SSL_do_handshake(server);
      const int error = result == 1 ? SSL_ERROR_NONE : SSL_get_error(server, result);
      check(result == 1 || error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
      state.server = result == 1;
    }
    transfer(server, client);
    if (state.error || (state.client && state.server))
      return state;
  }
  check(false);
  return state;
}

static weave::TlsClientOptions options(const fixture::Certificates &files, weave::TlsVersion version, bool identity)
{
  weave::TlsClientOptions policy;
  policy.ca_file = files.ca;
  policy.min_version = policy.max_version = version;
  policy.session_resumption = true;
  if (identity) {
    policy.certificate_file = files.client;
    policy.private_key_file = files.client_key;
  }
  return policy;
}

static void round(
  SSL_CTX *server,
  const weave::TlsContext &credentials,
  weave::TlsCertificateMode mode,
  bool identity,
  bool request,
  bool incompatible)
{
  auto native = peer(server);
  auto client = weave::detail::TlsEngine::create(credentials, false, "localhost", nullptr, {}, true, mode);
  check(client.has_value());
  const auto outcome = handshake(*client, native.get());
  const bool used = identity && request && !incompatible && mode != weave::TlsCertificateMode::disable;
  const bool success = mode != weave::TlsCertificateMode::require || used;
  check(outcome.client == success);
  if (success) {
    check(outcome.server && !outcome.error);
    check((SSL_get0_peer_certificate(native.get()) != nullptr) == used);
    check(SSL_get_verify_result(native.get()) == X509_V_OK);
  } else {
    check(outcome.error == weave::TlsError::client_certificate_required);
    check(client->handshake().error == outcome.error);
    check(!client->session());
  }
}

static void resumption(
  SSL_CTX *server,
  const weave::TlsContext &credentials,
  weave::TlsCertificateMode mode,
  const fixture::Certificates &files,
  weave::TlsVersion version,
  weave::TlsSessionMode sessions)
{
  auto native = peer(server);
  auto client = weave::detail::TlsEngine::create(credentials, false, "localhost", nullptr, {}, true, mode);
  check(client.has_value());
  const auto original = handshake(*client, native.get());
  check(original.client && original.server && !original.error);
  std::byte marker{39};
  std::size_t size = 0;
  ERR_clear_error();
  check(SSL_write_ex(native.get(), &marker, 1, &size) == 1 && size == 1);
  transfer(native.get(), *client);
  check(client->read(std::span{&marker, 1}).action == weave::detail::TlsAction::ready);
  auto session = client->session();
  check(session.has_value() && session->available());
  const std::array modes{
    weave::TlsCertificateMode::disable,
    weave::TlsCertificateMode::allow,
    weave::TlsCertificateMode::require};
  for (auto other : modes) {
    if (other == mode)
      continue;
    auto mismatch = weave::detail::TlsEngine::create(credentials, false, "localhost", &*session, {}, true, other);
    check(!mismatch && mismatch.error() == weave::TlsError::session_rejected && session->available());
  }
  auto resumed = weave::detail::TlsEngine::create(credentials, false, "localhost", &*session, {}, true, mode);
  check(resumed.has_value() && !session->available());
  auto repeated = peer(server);
  const auto outcome = handshake(*resumed, repeated.get());
  check(outcome.client && outcome.server && !outcome.error);
  check(resumed->session_reused() && SSL_session_reused(repeated.get()) == 1);
  check((SSL_get0_peer_certificate(repeated.get()) != nullptr) == (mode != weave::TlsCertificateMode::disable));

  auto source = weave::detail::TlsEngine::create(credentials, false, "localhost", nullptr, {}, true, mode);
  auto issuing = peer(server);
  check(source.has_value());
  const auto issued = handshake(*source, issuing.get());
  check(issued.client && issued.server && !issued.error);
  ERR_clear_error();
  check(SSL_write_ex(issuing.get(), &marker, 1, &size) == 1 && size == 1);
  transfer(issuing.get(), *source);
  check(source->read(std::span{&marker, 1}).action == weave::detail::TlsAction::ready);
  auto offer = source->session();
  check(offer.has_value() && offer->available());

  // A fresh server cannot reuse this session and does not request a new client identity.
  auto fresh = ::server(files, version, false, false, sessions);
  auto rejecting = peer(fresh.get());
  auto fallback = weave::detail::TlsEngine::create(credentials, false, "localhost", &*offer, {}, true, mode);
  check(fallback.has_value() && !offer->available());
  const auto retried = handshake(*fallback, rejecting.get());
  check(!fallback->session_reused() && SSL_session_reused(rejecting.get()) == 0);
  if (mode == weave::TlsCertificateMode::require) {
    check(!retried.client && retried.error == weave::TlsError::client_certificate_required);
    check(!fallback->session());
  } else {
    check(retried.client && retried.server && !retried.error);
    check(SSL_get0_peer_certificate(rejecting.get()) == nullptr);
  }
}

int main()
{
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  std::filesystem::path directory;
  {
    static auto storage = std::make_unique<fixture::Certificates>();
    const auto &files = *storage;
    directory = files.directory;
    const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
    const std::array policies{
      weave::TlsCertificateMode::disable,
      weave::TlsCertificateMode::allow,
      weave::TlsCertificateMode::require};
    const std::array flags{false, true};
    const std::array modes{weave::TlsSessionMode::stateful, weave::TlsSessionMode::tickets};
    for (auto version : versions) {
      for (bool identity : flags) {
        auto credentials = weave::TlsContext::client(options(files, version, identity));
        check(credentials.has_value());
        for (bool request : flags) {
          for (bool incompatible : flags) {
            auto accepting = server(files, version, request, incompatible, weave::TlsSessionMode::tickets);
            for (auto policy : policies)
              round(accepting.get(), *credentials, policy, identity, request, incompatible);
          }
        }
      }
      auto credentials = weave::TlsContext::client(options(files, version, true));
      check(credentials.has_value());
      for (auto mode : modes) {
        auto accepting = server(files, version, true, false, mode);
        for (auto policy : policies)
          resumption(accepting.get(), *credentials, policy, files, version, mode);
        std::array<std::thread, 9> workers;
        for (std::size_t i = 0; i < workers.size(); ++i) {
          workers[i] = std::thread([&, i] {
            round(accepting.get(), *credentials, policies[i % policies.size()], true, true, false);
          });
        }
        for (auto &worker : workers)
          worker.join();
      }
    }
    storage.reset();
  }
  check(!std::filesystem::exists(directory));
  std::printf("Certificate policy: %u checks, cleanup=true, %s\n", checks.load(), OpenSSL_version(OPENSSL_VERSION));
}
