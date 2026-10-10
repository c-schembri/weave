#include <weave/tls.hpp>
#include <weave/scope.hpp>
#include <weave/timer.hpp>
#include "tls_certificates.hpp"
#ifdef WEAVE_TLS_CLIENT_CERTIFICATE_RUNTIME
#include <weave/runtime.hpp>
#include "runtime_fixture.hpp"
#endif
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <atomic>
#include <source_location>
#include <cstdio>

static std::atomic<unsigned> checks{0};
static std::atomic<unsigned> connections{0};
static std::atomic<unsigned> hellos{0};
static std::atomic<unsigned> resumptions{0};
static std::atomic<unsigned> rejected{0};
static std::atomic<unsigned> verification_rejected{0};

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Certificate stream check failed: %s:%u\n", where.file_name(), where.line());
    std::exit(1);
  }
}

struct Observation {
  std::string_view name;
  bool sni;
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
  const bool present = SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_server_name, &extension, &size) == 1;
  const bool expected = observation->sni && !weave::IpAddress::parse(observation->name);
  check(present == expected);
  check(!present || (extension != nullptr && size > 0));
  ++observation->hellos;
  ++hellos;
  return SSL_CLIENT_HELLO_SUCCESS;
}

static int acknowledge(SSL *, int *, void *)
{
  return SSL_TLSEXT_ERR_OK;
}

static int select_protocol(
  SSL *,
  const unsigned char **out,
  unsigned char *size,
  const unsigned char *offered,
  unsigned length,
  void *)
{
  static constexpr unsigned char protocol[]{10, 's', 't', 'r', 'e', 'a', 'm', '-', 's', 'n', 'i'};
  unsigned char *selected = nullptr;
  const int result = SSL_select_next_proto(&selected, size, protocol, sizeof(protocol), offered, length);
  check(result == OPENSSL_NPN_NEGOTIATED);
  *out = selected;
  return SSL_TLSEXT_ERR_OK;
}

using NativeContext = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using Native = std::unique_ptr<SSL, decltype(&SSL_free)>;

static NativeContext context(const fixture::Certificates &files, weave::TlsVersion version, weave::TlsSessionMode mode)
{
  NativeContext ctx{SSL_CTX_new(TLS_server_method()), SSL_CTX_free};
  check(ctx != nullptr);
  const int protocol = version == weave::TlsVersion::tls12 ? TLS1_2_VERSION : TLS1_3_VERSION;
  check(SSL_CTX_set_min_proto_version(ctx.get(), protocol) == 1);
  check(SSL_CTX_set_max_proto_version(ctx.get(), protocol) == 1);
  check(SSL_CTX_use_certificate_chain_file(ctx.get(), files.leaf.c_str()) == 1);
  check(SSL_CTX_use_PrivateKey_file(ctx.get(), files.private_key.c_str(), SSL_FILETYPE_PEM) == 1);
  check(SSL_CTX_check_private_key(ctx.get()) == 1);
  check(SSL_CTX_load_verify_locations(ctx.get(), files.ca.c_str(), nullptr) == 1);
  SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
  const std::array<unsigned char, 3> identity{4, 5, 6};
  check(SSL_CTX_set_session_id_context(ctx.get(), identity.data(), static_cast<unsigned>(identity.size())) == 1);
  SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_SERVER);
  check(SSL_CTX_set_num_tickets(ctx.get(), 1) == 1);
  if (mode == weave::TlsSessionMode::stateful)
    SSL_CTX_set_options(ctx.get(), SSL_OP_NO_TICKET);
  SSL_CTX_set_client_hello_cb(ctx.get(), hello, nullptr);
  SSL_CTX_set_tlsext_servername_callback(ctx.get(), acknowledge);
  SSL_CTX_set_alpn_select_cb(ctx.get(), select_protocol, nullptr);
  return ctx;
}

static weave::Task<void> flush(weave::TcpStream &socket, SSL *ssl)
{
  std::array<std::byte, 32768> buffer;
  while (BIO_ctrl_pending(SSL_get_wbio(ssl))) {
    const int size = BIO_read(SSL_get_wbio(ssl), buffer.data(), static_cast<int>(buffer.size()));
    check(size > 0);
    co_await socket.write_all(std::span{buffer}.first(static_cast<std::size_t>(size)));
  }
}

static weave::Task<bool> input(weave::TcpStream &socket, SSL *ssl)
{
  std::array<std::byte, 32768> buffer;
  const auto size = co_await socket.read(buffer);
  if (size == 0)
    co_return false;
  check(BIO_write(SSL_get_rbio(ssl), buffer.data(), static_cast<int>(size)) == static_cast<int>(size));
  co_return true;
}

static weave::Task<void> peer(weave::TcpStream socket, SSL_CTX *ctx, std::string name, bool sni, unsigned phase)
{
  ++connections;
  const bool accepted = phase == 0 || phase == 6;
  const bool invalid_identity = phase >= 8;
  if (!accepted && !invalid_identity) {
    std::array<std::byte, 1> buffer;
    check((co_await socket.read(buffer)) == 0);
    ++rejected;
    co_return;
  }

  Observation observation{name, sni};
  Native ssl{SSL_new(ctx), SSL_free};
  check(ssl != nullptr);
  auto *incoming = BIO_new(BIO_s_mem());
  auto *outgoing = BIO_new(BIO_s_mem());
  check(incoming != nullptr && outgoing != nullptr);
  BIO_set_mem_eof_return(incoming, -1);
  SSL_set_bio(ssl.get(), incoming, outgoing);
  check(SSL_set_ex_data(ssl.get(), observation_index(), &observation) == 1);
  SSL_set_accept_state(ssl.get());

  for (;;) {
    ERR_clear_error();
    const int result = SSL_do_handshake(ssl.get());
    const int error = result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl.get(), result);
    if (error == SSL_ERROR_SSL) {
      check(invalid_identity && observation.hellos == 1);
      ++verification_rejected;
      co_return;
    }
    check(result == 1 || error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
    co_await flush(socket, ssl.get());
    if (result == 1) {
      check(!invalid_identity);
      break;
    }
    if (error == SSL_ERROR_WANT_READ && !(co_await input(socket, ssl.get()))) {
      check(invalid_identity && observation.hellos == 1);
      ++verification_rejected;
      co_return;
    }
  }

  check(observation.hellos == 1);
  const auto *selected = SSL_get_servername(ssl.get(), TLSEXT_NAMETYPE_host_name);
  const bool expected = sni && !weave::IpAddress::parse(name);
  check(expected ? selected && std::string_view(selected) == name : selected == nullptr);
  check(SSL_get_verify_result(ssl.get()) == X509_V_OK);
  check(SSL_get0_peer_certificate(ssl.get()) != nullptr);
  const bool resumed = SSL_session_reused(ssl.get()) == 1;
  check(resumed == (phase == 6));
  if (resumed)
    ++resumptions;

  const std::byte marker{91};
  std::size_t written = 0;
  ERR_clear_error();
  check(SSL_write_ex(ssl.get(), &marker, 1, &written) == 1 && written == 1);
  co_await flush(socket, ssl.get());

  for (;;) {
    ERR_clear_error();
    const int result = SSL_shutdown(ssl.get());
    const int error = result >= 0 ? SSL_ERROR_NONE : SSL_get_error(ssl.get(), result);
    check(result >= 0 || error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
    co_await flush(socket, ssl.get());
    if (result == 1)
      break;
    if (result == 0 || error == SSL_ERROR_WANT_READ)
      check(co_await input(socket, ssl.get()));
  }
}

static weave::Task<weave::TcpStream> connect(weave::u16 port)
{
  auto socket = co_await weave::tcp::connect("127.0.0.1", port);
  check(socket.no_delay().has_value());
  co_return std::move(socket);
}

static weave::Task<void> client(
  weave::u16 port,
  weave::TlsContext credentials,
  weave::TlsContext different_credentials,
  weave::TlsContext untrusted_credentials,
  std::string name,
  bool sni,
  weave::TlsVersion version)
{
  const weave::TlsHandshakeOptions options{
    .required_protocol = "stream-sni",
    .server_name_indication = sni,
    .client_certificate = weave::TlsCertificateMode::require};
  std::vector<weave::TlsSession> sessions;
  {
    auto socket = co_await connect(port);
    auto stream = co_await weave::tls::client(std::move(socket), credentials, name, options);
    check(!stream.session_reused());
    check(stream.version() == version && stream.negotiated_protocol() == "stream-sni");
    std::array<std::byte, 1> buffer;
    co_await stream.read_exactly(buffer);
    check(buffer[0] == std::byte{91});
    for (unsigned i = 0; i < 7; ++i) {
      auto session = stream.session();
      check(session.has_value() && session->available());
      sessions.push_back(std::move(*session));
    }
    co_await stream.shutdown();
  }
  check(sessions.back().available());

  {
    auto socket = co_await connect(port);
    auto unstarted = weave::tls::client(std::move(socket), credentials, name, std::move(sessions[0]), options);
  }
  check(sessions.back().available());

  for (unsigned phase = 2; phase <= 5; ++phase) {
    auto socket = co_await connect(port);
    auto changed = options;
    if (phase == 2)
      changed.client_certificate = weave::TlsCertificateMode::allow;
    if (phase == 4)
      changed.required_protocol = "different";
    const std::string changed_name = phase == 3 ? "wrong.invalid" : name;
    auto changed_credentials = phase == 5 ? different_credentials : credentials;
    auto result = co_await weave::as_result(
      weave::tls::client(
        std::move(socket),
        std::move(changed_credentials),
        changed_name,
        std::move(sessions[phase - 1]),
        changed));
    check(!result && result.error() == weave::TlsError::session_rejected);
    check(sessions.back().available());
  }

  {
    auto socket = co_await connect(port);
    auto stream = co_await weave::tls::client(std::move(socket), credentials, name, std::move(sessions[5]), options);
    check(stream.session_reused() && !sessions.back().available());
    auto info = stream.info();
    check(info.has_value() && info->peer.has_value() && info->session_reused);
    check(stream.version() == version && stream.negotiated_protocol() == "stream-sni");
    std::array<std::byte, 1> buffer;
    co_await stream.read_exactly(buffer);
    check(buffer[0] == std::byte{91});
    co_await stream.shutdown();
  }

  auto socket = co_await connect(port);
  auto duplicate = co_await weave::as_result(
    weave::tls::client(std::move(socket), credentials, name, std::move(sessions[6]), options));
  check(!duplicate && duplicate.error() == weave::TlsError::session_rejected);

  const std::array invalid_names{std::string("wrong.invalid"), std::string("127.0.0.2"), name};
  for (std::size_t i = 0; i < invalid_names.size(); ++i) {
    auto transport = co_await connect(port);
    auto selected_credentials = i == 2 ? untrusted_credentials : credentials;
    auto failure = co_await weave::as_result(
      weave::tls::client(std::move(transport), std::move(selected_credentials), invalid_names[i], options));
    check(!failure && failure.error() == weave::TlsError::certificate_verification);
  }
}

static weave::Task<void> chain(
  SSL_CTX *server,
  weave::TlsContext credentials,
  weave::TlsContext different_credentials,
  weave::TlsContext untrusted_credentials,
  std::string name,
  bool sni,
  weave::TlsVersion version)
{
  co_await weave::scope([&](weave::TaskScope &children) -> weave::Task<void> {
    auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
    auto job = children.spawn(
      client(listener.local_port(), credentials, different_credentials, untrusted_credentials, name, sni, version));
    if (!job)
      co_await weave::fail(job.error());

    const std::array invalid_names{std::string("wrong.invalid"), std::string("127.0.0.2"), name};
    for (unsigned phase = 0; phase < 11; ++phase) {
      auto socket = co_await listener.accept({.no_delay = true});
      const auto &verification_name = phase >= 8 ? invalid_names[phase - 8] : name;
      co_await peer(std::move(socket), server, verification_name, sni, phase);
    }
    co_await children.join();
  });
}

static weave::Task<void> concurrent(
  SSL_CTX *server,
  weave::TlsContext credentials,
  weave::TlsContext different_credentials,
  weave::TlsContext untrusted_credentials,
  weave::TlsVersion version)
{
  co_await weave::scope([&](weave::TaskScope &children) -> weave::Task<void> {
    for (unsigned i = 0; i < 8; ++i) {
      auto job = children.spawn(
        chain(server, credentials, different_credentials, untrusted_credentials, "localhost", i % 2 == 0, version));
      if (!job)
        co_await weave::fail(job.error());
    }
    co_await children.join();
  });
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
    const std::array modes{weave::TlsSessionMode::stateful, weave::TlsSessionMode::tickets};
    const std::array names{std::string("localhost"), std::string("127.0.0.1"), std::string("::1")};
    const std::array policies{false, true};

    for (auto version : versions) {
      for (auto mode : modes) {
        auto server = context(files, version, mode);
        weave::TlsClientOptions options;
        options.ca_file = files.ca;
        options.certificate_file = files.client;
        options.private_key_file = files.client_key;
        options.min_version = options.max_version = version;
        options.session_resumption = true;
        auto credentials = weave::TlsContext::client(options);
        auto different_credentials = weave::TlsContext::client(options);
        options.ca_file = files.untrusted;
        auto untrusted_credentials = weave::TlsContext::client(std::move(options));
        check(credentials.has_value() && different_credentials.has_value() && untrusted_credentials.has_value());
        auto ctx = weave::Context::create();
        check(ctx.has_value());
        for (const auto &name : names) {
          for (bool sni : policies) {
            auto exchange = chain(
              server.get(),
              *credentials,
              *different_credentials,
              *untrusted_credentials,
              name,
              sni,
              version);
            auto result = ctx->run(weave::timeout(std::chrono::seconds{10}, std::move(exchange)));
            check(result.has_value());
          }
        }
#ifdef WEAVE_TLS_CLIENT_CERTIFICATE_RUNTIME
        const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
        for (auto layout : support::io_layouts) {
          for (auto scheduler : schedulers) {
            auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
            check(runtime.has_value());
            auto exchange = concurrent(
              server.get(),
              *credentials,
              *different_credentials,
              *untrusted_credentials,
              version);
            auto result = runtime->run(weave::timeout(std::chrono::seconds{20}, std::move(exchange)));
            check(result.has_value());
          }
        }
#endif
      }
    }
    unsigned expected_chains = 24;
#ifdef WEAVE_TLS_CLIENT_CERTIFICATE_RUNTIME
    expected_chains += 4 * static_cast<unsigned>(support::io_layouts.size()) * 2 * 8;
#endif
    check(resumptions == expected_chains);
    check(connections == resumptions * 11 && rejected == resumptions * 6);
    check(hellos == resumptions * 5 && verification_rejected == resumptions * 3);
    storage.reset();
    check(!std::filesystem::exists(directory));
  }
  check(!std::filesystem::exists(directory));
  std::printf(
    "{\"checks\":%u,\"connections\":%u,\"hellos\":%u,\"resumptions\":%u,\"rejected\":%u,"
    "\"verification_rejected\":%u,\"cleanup\":true,\"openssl\":"
    "\"%s\"}\n",
    checks.load(),
    connections.load(),
    hellos.load(),
    resumptions.load(),
    rejected.load(),
    verification_rejected.load(),
    OpenSSL_version(OPENSSL_VERSION));
}
