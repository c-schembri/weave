#include <weave/tls.hpp>
#include <weave/tls/detail/engine.hpp>
#include "tls_certificates.hpp"
#include <openssl/ssl.h>
#include <atomic>
#include <thread>
#include <source_location>
#include <cstdio>

static std::atomic<unsigned> checks{0};

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "SNI engine check failed: %s:%u\n", where.file_name(), where.line());
    std::exit(1);
  }
}

struct Observation {
  bool expected;
  unsigned hellos = 0;
};

static int observation_index()
{
  static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
  check(index >= 0);
  return index;
}

static int hello(SSL *ssl, int *, void *)
{
  auto *observation = static_cast<Observation *>(SSL_get_ex_data(ssl, observation_index()));
  check(observation != nullptr);
  const unsigned char *extension = nullptr;
  std::size_t size = 0;
  bool present = SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_server_name, &extension, &size) == 1;
  check(present == observation->expected);
  check(!present || (extension != nullptr && size > 0));
  ++observation->hellos;
  return SSL_CLIENT_HELLO_SUCCESS;
}

static int acknowledge(SSL *, int *, void *)
{
  return SSL_TLSEXT_ERR_OK;
}

using NativeContext = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using Native = std::unique_ptr<SSL, decltype(&SSL_free)>;

static NativeContext context(fixture::Certificates &files, weave::TlsVersion version, weave::TlsSessionMode mode)
{
  NativeContext ctx{SSL_CTX_new(TLS_server_method()), SSL_CTX_free};
  check(ctx != nullptr);
  int protocol = version == weave::TlsVersion::tls12 ? TLS1_2_VERSION : TLS1_3_VERSION;
  check(SSL_CTX_set_min_proto_version(ctx.get(), protocol) == 1);
  check(SSL_CTX_set_max_proto_version(ctx.get(), protocol) == 1);
  check(SSL_CTX_use_certificate_chain_file(ctx.get(), files.leaf.c_str()) == 1);
  check(SSL_CTX_use_PrivateKey_file(ctx.get(), files.private_key.c_str(), SSL_FILETYPE_PEM) == 1);
  check(SSL_CTX_check_private_key(ctx.get()) == 1);
  const std::array<unsigned char, 3> identity{1, 2, 3};
  check(SSL_CTX_set_session_id_context(ctx.get(), identity.data(), static_cast<unsigned>(identity.size())) == 1);
  SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_SERVER);
  if (mode == weave::TlsSessionMode::stateful)
    SSL_CTX_set_options(ctx.get(), SSL_OP_NO_TICKET);
  SSL_CTX_set_client_hello_cb(ctx.get(), hello, nullptr);
  SSL_CTX_set_tlsext_servername_callback(ctx.get(), acknowledge);
  return ctx;
}

static Native peer(SSL_CTX *ctx, Observation &observation)
{
  Native ssl{SSL_new(ctx), SSL_free};
  check(ssl != nullptr);
  auto *input = BIO_new(BIO_s_mem());
  auto *output = BIO_new(BIO_s_mem());
  check(input != nullptr && output != nullptr);
  BIO_set_mem_eof_return(input, -1);
  SSL_set_bio(ssl.get(), input, output);
  check(SSL_set_ex_data(ssl.get(), observation_index(), &observation) == 1);
  SSL_set_accept_state(ssl.get());
  return ssl;
}

static void transfer(weave::detail::TlsEngine &client, SSL *server)
{
  std::array<std::byte, 32768> buffer;
  for (;;) {
    auto count = client.output(buffer);
    check(count.has_value());
    if (!*count)
      return;
    check(BIO_write(SSL_get_rbio(server), buffer.data(), static_cast<int>(*count)) == static_cast<int>(*count));
  }
}

static void transfer(SSL *server, weave::detail::TlsEngine &client)
{
  std::array<std::byte, 32768> buffer;
  while (BIO_ctrl_pending(SSL_get_wbio(server))) {
    int size = BIO_read(SSL_get_wbio(server), buffer.data(), static_cast<int>(buffer.size()));
    check(size > 0);
    check(client.input(std::span{buffer}.first(static_cast<std::size_t>(size))).has_value());
  }
}

static bool handshake(weave::detail::TlsEngine &client, SSL *server)
{
  bool connected = false;
  bool accepted = false;
  for (int i = 0; i < 100; ++i) {
    if (!connected) {
      auto step = client.handshake();
      if (step.action == weave::detail::TlsAction::failed)
        return false;
      connected = step.action == weave::detail::TlsAction::ready;
    }
    transfer(client, server);
    if (!accepted) {
      int result = SSL_do_handshake(server);
      int error = SSL_get_error(server, result);
      check(result == 1 || error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
      accepted = result == 1;
    }
    transfer(server, client);
    if (connected && accepted)
      return true;
  }
  check(false);
  return false;
}

static void round(SSL_CTX *server, const weave::TlsContext &credentials, std::string name, bool sni)
{
  bool dns = !weave::IpAddress::parse(name);
  Observation observation{sni && dns};
  auto accepted = peer(server, observation);
  auto client = weave::detail::TlsEngine::create(credentials, false, name, nullptr, {}, sni);
  check(client.has_value());
  check(handshake(*client, accepted.get()));
  check(observation.hellos == 1);
  auto *selected = SSL_get_servername(accepted.get(), TLSEXT_NAMETYPE_host_name);
  check(observation.expected ? selected && std::string_view(selected) == name : selected == nullptr);

  std::byte byte{std::byte{7}};
  std::size_t written = 0;
  check(SSL_write_ex(accepted.get(), &byte, 1, &written) == 1 && written == 1);
  transfer(accepted.get(), *client);
  check(client->read(std::span{&byte, 1}).action == weave::detail::TlsAction::ready);
  auto session = client->session();
  check(session.has_value() && session->available());

  auto mismatch = weave::detail::TlsEngine::create(credentials, false, name, &*session, {}, !sni);
  check(!mismatch && mismatch.error() == weave::TlsError::session_rejected && session->available());
  auto wrong_name = weave::detail::TlsEngine::create(credentials, false, "wrong.invalid", &*session, {}, sni);
  check(!wrong_name && wrong_name.error() == weave::TlsError::session_rejected && session->available());

  Observation repeated{sni && dns};
  auto resumed_peer = peer(server, repeated);
  auto resumed = weave::detail::TlsEngine::create(credentials, false, name, &*session, {}, sni);
  check(resumed.has_value() && !session->available());
  check(handshake(*resumed, resumed_peer.get()));
  check(resumed->session_reused() && SSL_session_reused(resumed_peer.get()) == 1);
  check(repeated.hellos == 1);
  auto duplicate = weave::detail::TlsEngine::create(credentials, false, name, &*session, {}, sni);
  check(!duplicate && duplicate.error() == weave::TlsError::session_rejected);
}

int main()
{
  static fixture::Certificates files;
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
  const std::array modes{weave::TlsSessionMode::stateful, weave::TlsSessionMode::tickets};
  const std::array names{std::string("localhost"), std::string("127.0.0.1"), std::string("::1")};
  const std::array policies{false, true};

  for (auto version : versions) {
    for (auto mode : modes) {
      auto server = context(files, version, mode);
      auto credentials = weave::TlsContext::client(
        {.ca_file = files.ca, .min_version = version, .max_version = version, .session_resumption = true});
      check(credentials.has_value());
      for (const auto &name : names) {
        for (bool policy : policies)
          round(server.get(), *credentials, name, policy);
      }

      std::array<std::thread, 8> workers;
      for (std::size_t i = 0; i < workers.size(); ++i) {
        workers[i] = std::thread([&, i] {
          round(server.get(), *credentials, "localhost", i % 2 == 0);
        });
      }
      for (auto &worker : workers)
        worker.join();

      const std::array wrong_names{std::string("wrong.invalid"), std::string("127.0.0.2"), std::string("::2")};
      for (const auto &name : wrong_names) {
        for (bool policy : policies) {
          Observation observed{policy && !weave::IpAddress::parse(name)};
          auto accepted = peer(server.get(), observed);
          auto client = weave::detail::TlsEngine::create(*credentials, false, name, nullptr, {}, policy);
          check(client.has_value());
          check(!handshake(*client, accepted.get()));
          check(client->handshake().error == weave::TlsError::certificate_verification);
          check(observed.hellos == 1);
        }
      }
    }
  }
  std::printf("SNI engine: %u checks\n", checks.load());
}
