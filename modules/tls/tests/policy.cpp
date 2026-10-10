#include <weave/tls.hpp>
#include "tls_certificates.hpp"
#include <openssl/ocsp.h>
#include <thread>
#include <doctest/doctest.h>

struct PolicyCertificates : fixture::Certificates {
  std::string clean_crl = (directory / "clean.crl").string();
  std::string revoked_crl = (directory / "revoked.crl").string();
  std::string client_revoked_crl = (directory / "client-revoked.crl").string();
  std::string good_ocsp = (directory / "good.der").string();
  std::string bad_ocsp = (directory / "bad.der").string();
  std::string stale_ocsp = (directory / "stale.der").string();

  PolicyCertificates()
  {
    auto root_key = fixture::key();
    auto root = fixture::certificate(root_key.get(), nullptr, nullptr, 11, false);
    auto server_key = fixture::key();
    auto server = fixture::certificate(server_key.get(), root.get(), root_key.get(), 12, false);
    auto identity_key = fixture::key();
    auto identity = fixture::certificate(identity_key.get(), root.get(), root_key.get(), 13, false, "clientAuth");
    fixture::write_certificate(ca, root.get());
    fixture::write_certificate(leaf, server.get());
    fixture::write_certificate(client, identity.get());

    const auto write_key = [](const std::string &path, EVP_PKEY *key) {
      fixture::Bio file{BIO_new_file(path.c_str(), "w"), BIO_free};
      fixture::require(file && PEM_write_bio_PrivateKey(file.get(), key, nullptr, nullptr, 0, nullptr, nullptr) == 1);
    };
    write_key(private_key, server_key.get());
    write_key(client_key, identity_key.get());

    const auto write_crl = [&](const std::string &path, X509 *revoked) {
      std::unique_ptr<X509_CRL, decltype(&X509_CRL_free)> crl{X509_CRL_new(), X509_CRL_free};
      fixture::require(crl != nullptr);
      fixture::require(X509_CRL_set_version(crl.get(), 1) == 1);
      fixture::require(X509_CRL_set_issuer_name(crl.get(), X509_get_subject_name(root.get())) == 1);
      auto *before = ASN1_TIME_adj(nullptr, std::time(nullptr), 0, -60);
      auto *after = ASN1_TIME_adj(nullptr, std::time(nullptr), 1, 0);
      fixture::require(before && after);
      fixture::require(X509_CRL_set1_lastUpdate(crl.get(), before) == 1);
      fixture::require(X509_CRL_set1_nextUpdate(crl.get(), after) == 1);
      if (revoked) {
        auto *entry = X509_REVOKED_new();
        fixture::require(entry != nullptr);
        fixture::require(X509_REVOKED_set_serialNumber(entry, X509_get_serialNumber(revoked)) == 1);
        fixture::require(X509_REVOKED_set_revocationDate(entry, before) == 1);
        fixture::require(X509_CRL_add0_revoked(crl.get(), entry) == 1);
      }
      ASN1_TIME_free(before);
      ASN1_TIME_free(after);
      fixture::require(X509_CRL_sort(crl.get()) == 1);
      fixture::require(X509_CRL_sign(crl.get(), root_key.get(), EVP_sha256()) > 0);
      fixture::Bio file{BIO_new_file(path.c_str(), "w"), BIO_free};
      fixture::require(file && PEM_write_bio_X509_CRL(file.get(), crl.get()) == 1);
    };
    write_crl(clean_crl, nullptr);
    write_crl(revoked_crl, server.get());
    write_crl(client_revoked_crl, identity.get());

    const auto write_ocsp = [&](const std::string &path, bool revoked, bool stale) {
      auto *basic = OCSP_BASICRESP_new();
      auto *id = OCSP_cert_to_id(nullptr, server.get(), root.get());
      auto *before = ASN1_TIME_adj(nullptr, std::time(nullptr), 0, stale ? -7200 : -60);
      auto *after = ASN1_TIME_adj(nullptr, std::time(nullptr), 0, stale ? -3600 : 3600);
      fixture::require(basic && id && before && after);
      fixture::require(
        OCSP_basic_add1_status(
          basic,
          id,
          revoked ? V_OCSP_CERTSTATUS_REVOKED : V_OCSP_CERTSTATUS_GOOD,
          revoked ? OCSP_REVOKED_STATUS_KEYCOMPROMISE : 0,
          revoked ? before : nullptr,
          before,
          after) != nullptr);
      fixture::require(OCSP_basic_sign(basic, root.get(), root_key.get(), EVP_sha256(), nullptr, 0) == 1);
      auto *response = OCSP_response_create(OCSP_RESPONSE_STATUS_SUCCESSFUL, basic);
      fixture::require(response != nullptr);
      fixture::Bio file{BIO_new_file(path.c_str(), "wb"), BIO_free};
      fixture::require(file && i2d_OCSP_RESPONSE_bio(file.get(), response) == 1);
      OCSP_RESPONSE_free(response);
      OCSP_BASICRESP_free(basic);
      OCSP_CERTID_free(id);
      ASN1_TIME_free(before);
      ASN1_TIME_free(after);
    };
    write_ocsp(good_ocsp, false, false);
    write_ocsp(bad_ocsp, true, false);
    write_ocsp(stale_ocsp, false, true);
  }

  ~PolicyCertificates()
  {
    const std::array paths{clean_crl, revoked_crl, client_revoked_crl, good_ocsp, bad_ocsp, stale_ocsp};
    std::error_code error;
    for (const auto &path : paths)
      std::filesystem::remove(path, error);
  }
};

struct KeyFormatCertificates : PolicyCertificates {
  std::filesystem::path server_der = directory / "server.der";
  std::filesystem::path client_der = directory / "client.der";
  std::filesystem::path encrypted = directory / "encrypted-store.pem";
  std::filesystem::path multiple = directory / "multiple-store.pem";

  KeyFormatCertificates()
  {
    const auto convert = [](const std::string &source, const std::filesystem::path &destination) {
      fixture::Bio input{BIO_new_file(source.c_str(), "rb"), BIO_free};
      fixture::Key key{PEM_read_bio_PrivateKey(input.get(), nullptr, nullptr, nullptr), EVP_PKEY_free};
      fixture::Bio output{BIO_new_file(destination.string().c_str(), "wb"), BIO_free};
      fixture::require(key && output && i2d_PrivateKey_bio(output.get(), key.get()) == 1);
    };
    convert(private_key, server_der);
    convert(client_key, client_der);

    fixture::Bio input{BIO_new_file(private_key.c_str(), "rb"), BIO_free};
    fixture::Key key{PEM_read_bio_PrivateKey(input.get(), nullptr, nullptr, nullptr), EVP_PKEY_free};
    fixture::Bio output{BIO_new_file(encrypted.string().c_str(), "wb"), BIO_free};
    std::string password = "store-password";
    fixture::require(
      key && output &&
      PEM_write_bio_PrivateKey(
        output.get(),
        key.get(),
        EVP_aes_256_cbc(),
        reinterpret_cast<unsigned char *>(password.data()),
        static_cast<int>(password.size()),
        nullptr,
        nullptr) == 1);

    fixture::Bio duplicate{BIO_new_file(multiple.string().c_str(), "wb"), BIO_free};
    fixture::require(
      duplicate && PEM_write_bio_PrivateKey(duplicate.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
    fixture::require(PEM_write_bio_PrivateKey(duplicate.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
  }

  ~KeyFormatCertificates()
  {
    const std::array paths{server_der, client_der, encrypted, multiple};
    for (const auto &path : paths) {
      std::error_code error;
      fixture::require(std::filesystem::remove(path, error) && !error);
    }
  }
};

struct CrlDirectories {
  PolicyCertificates certificates;
  std::array<std::filesystem::path, 6> paths{
    certificates.directory / "clean",
    certificates.directory / "revoked",
    certificates.directory / "empty",
    certificates.directory / "malformed",
    certificates.directory / "unhashed",
    certificates.directory / "client-revoked"};
  std::array<char, 32> filename{};

  CrlDirectories()
  {
    fixture::Bio file{BIO_new_file(certificates.ca.c_str(), "r"), BIO_free};
    fixture::Certificate ca{PEM_read_bio_X509(file.get(), nullptr, nullptr, nullptr), X509_free};
    fixture::require(ca != nullptr);
    std::snprintf(filename.data(), filename.size(), "%08lx.r0", X509_NAME_hash(X509_get_subject_name(ca.get())));

    std::error_code error;
    for (const auto &path : paths)
      fixture::require(std::filesystem::create_directory(path, error) && !error);
    const std::array copies{
      std::pair{certificates.clean_crl, paths[0] / filename.data()},
      std::pair{certificates.revoked_crl, paths[1] / filename.data()},
      std::pair{certificates.leaf, paths[3] / filename.data()},
      std::pair{certificates.clean_crl, paths[4] / "unhashed.crl"},
      std::pair{certificates.client_revoked_crl, paths[5] / filename.data()}};
    for (const auto &[source, destination] : copies)
      fixture::require(std::filesystem::copy_file(source, destination, error) && !error);
  }

  ~CrlDirectories()
  {
    std::error_code error;
    for (std::size_t index = 0; index < paths.size(); ++index) {
      if (index != 2) {
        const auto name = index == 4 ? "unhashed.crl" : filename.data();
        fixture::require(std::filesystem::remove(paths[index] / name, error) && !error);
      }
      fixture::require(std::filesystem::remove(paths[index], error) && !error);
    }
  }
};

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
  for (int i = 0; i < 100; ++i) {
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

TEST_CASE("mTLS validates both roles and exposes only authenticated identities")
{
  PolicyCertificates certificates;
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
  const std::array modes{weave::TlsClientAuth::none, weave::TlsClientAuth::optional, weave::TlsClientAuth::required};
  const std::array identities{0, 1, 2};
  for (auto version : versions) {
    for (auto mode : modes) {
      for (auto identity : identities) {
        INFO("version ", int(version), " auth ", int(mode), " identity ", identity);
        auto server_context = weave::TlsContext::server(
          {.certificate_file = certificates.leaf,
            .private_key_file = certificates.private_key,
            .min_version = version,
            .max_version = version,
            .client_auth = mode,
            .ca_file = mode == weave::TlsClientAuth::none ? "" : certificates.ca});
        auto client_context = weave::TlsContext::client(
          {.ca_file = certificates.ca,
            .min_version = version,
            .max_version = version,
            .certificate_file = identity == 0 ? ""
              : identity == 1                 ? certificates.client
                                              : certificates.leaf,
            .private_key_file = identity == 0 ? ""
              : identity == 1                 ? certificates.client_key
                                              : certificates.private_key});
        REQUIRE(server_context);
        REQUIRE(client_context);
        auto server = weave::detail::TlsEngine::create(*server_context, true, "");
        auto client = weave::detail::TlsEngine::create(*client_context, false, "localhost");
        REQUIRE(server);
        REQUIRE(client);
        const bool expected = mode == weave::TlsClientAuth::none || identity == 1 ||
          (mode == weave::TlsClientAuth::optional && identity == 0);
        CHECK(handshake(*client, *server) == expected);
        if (!expected)
          continue;
        CHECK(bool(server->peer_identity()) == (mode != weave::TlsClientAuth::none && identity == 1));
        auto peer = client->peer_identity();
        REQUIRE(peer);
        CHECK(peer->sha256.size() == 32);
        CHECK_FALSE(client->cipher().empty());
        CHECK(client->channel_binding() == server->channel_binding());
        CHECK(
          client->export_keying_material("EXPORTER-weave-test", 32, std::nullopt) ==
          server->export_keying_material("EXPORTER-weave-test", 32, std::nullopt));
        CHECK_FALSE(client->export_keying_material("master secret", 32, std::nullopt));
      }
    }
  }
}

TEST_CASE("CRL and stapled OCSP fail closed")
{
  PolicyCertificates certificates;
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
  const std::array scenarios{0, 1, 2, 3, 4};
  for (auto version : versions) {
    for (auto scenario : scenarios) {
      auto server_context = weave::TlsContext::server(
        {.certificate_file = certificates.leaf,
          .private_key_file = certificates.private_key,
          .min_version = version,
          .max_version = version,
          .ocsp_file = scenario == 3 ? certificates.good_ocsp : ""});
      auto client_context = weave::TlsContext::client(
        {.ca_file = certificates.ca,
          .min_version = version,
          .max_version = version,
          .crl_file = scenario < 2 ? (scenario == 0 ? certificates.clean_crl : certificates.revoked_crl) : "",
          .revocation = scenario < 2 ? weave::TlsRevocation::leaf : weave::TlsRevocation::none,
          .ocsp = scenario >= 2 ? (scenario == 4 ? weave::TlsOcsp::if_present : weave::TlsOcsp::required)
                                : weave::TlsOcsp::none});
      REQUIRE(server_context);
      REQUIRE(client_context);
      auto server = weave::detail::TlsEngine::create(*server_context, true, "");
      auto client = weave::detail::TlsEngine::create(*client_context, false, "localhost");
      REQUIRE(server);
      REQUIRE(client);
      INFO("version ", int(version), " scenario ", scenario);
      CHECK(handshake(*client, *server) == (scenario == 0 || scenario == 3 || scenario == 4));
    }
  }
  const std::array invalid_staples{certificates.bad_ocsp, certificates.stale_ocsp, certificates.leaf};
  for (const auto &path : invalid_staples) {
    CHECK_FALSE(
      weave::TlsContext::server(
        {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key, .ocsp_file = path}));
  }
}

TEST_CASE("Hashed CRL directories reject revoked and missing evidence on both TLS roles")
{
  CrlDirectories files;
  const auto &certificates = files.certificates;
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
  const std::array policies{weave::TlsRevocation::leaf, weave::TlsRevocation::chain};
  const std::array scenarios{0, 1, 2, 3, 4, 5, 6};
  for (auto version : versions) {
    for (auto policy : policies) {
      for (auto scenario : scenarios) {
        INFO("version ", int(version), " revocation ", int(policy), " directory scenario ", scenario);
        auto directory = scenario == 5 ? certificates.directory / "missing" : files.paths[scenario == 6 ? 0 : scenario];
        auto server_context = weave::TlsContext::server(
          {.certificate_file = certificates.leaf,
            .private_key_file = certificates.private_key,
            .min_version = version,
            .max_version = version});
        auto client_context = weave::TlsContext::client(
          {.ca_file = certificates.ca,
            .min_version = version,
            .max_version = version,
            .crl_file = scenario == 6 ? certificates.clean_crl : "",
            .crl_directory = directory.string(),
            .revocation = policy});
        REQUIRE(server_context);
        REQUIRE(client_context);
        if (!server_context || !client_context)
          return;

        auto server = weave::detail::TlsEngine::create(*server_context, true, "");
        auto client = weave::detail::TlsEngine::create(*client_context, false, "localhost");
        REQUIRE(server);
        REQUIRE(client);
        if (!server || !client)
          return;
        CHECK(handshake(*client, *server) == (scenario == 0 || scenario == 6));
      }
    }
    const std::array mtls_directories{0, 2, 5};
    for (auto index : mtls_directories) {
      INFO("version ", int(version), " mTLS directory ", index);
      auto server_context = weave::TlsContext::server(
        {.certificate_file = certificates.leaf,
          .private_key_file = certificates.private_key,
          .min_version = version,
          .max_version = version,
          .client_auth = weave::TlsClientAuth::required,
          .ca_file = certificates.ca,
          .crl_directory = files.paths[index].string(),
          .revocation = weave::TlsRevocation::chain});
      auto client_context = weave::TlsContext::client(
        {.ca_file = certificates.ca,
          .min_version = version,
          .max_version = version,
          .certificate_file = certificates.client,
          .private_key_file = certificates.client_key});
      REQUIRE(server_context);
      REQUIRE(client_context);
      if (!server_context || !client_context)
        return;

      auto server = weave::detail::TlsEngine::create(*server_context, true, "");
      auto client = weave::detail::TlsEngine::create(*client_context, false, "localhost");
      REQUIRE(server);
      REQUIRE(client);
      if (!server || !client)
        return;
      CHECK(handshake(*client, *server) == (index == 0));
    }
  }
}

TEST_CASE("CRL directory configuration requires explicit revocation and applicable peer authentication")
{
  CrlDirectories files;
  const auto &certificates = files.certificates;
  CHECK_FALSE(weave::TlsContext::client({.ca_file = certificates.ca, .crl_directory = files.paths[0].string()}));
  CHECK_FALSE(weave::TlsContext::client({.ca_file = certificates.ca, .revocation = weave::TlsRevocation::chain}));
  CHECK_FALSE(
    weave::TlsContext::client(
      {.ca_file = certificates.ca,
        .crl_directory = std::string{"bad\0path", 8},
        .revocation = weave::TlsRevocation::chain}));
  CHECK_FALSE(
    weave::TlsContext::server(
      {.certificate_file = certificates.leaf,
        .private_key_file = certificates.private_key,
        .crl_directory = files.paths[0].string(),
        .revocation = weave::TlsRevocation::chain}));
  CHECK_FALSE(
    weave::TlsContext::server(
      {.certificate_file = certificates.leaf,
        .private_key_file = certificates.private_key,
        .client_auth = weave::TlsClientAuth::required,
        .crl_directory = files.paths[0].string(),
        .revocation = weave::TlsRevocation::chain}));
}

TEST_CASE("Sessions are opt-in, one-shot and bound to credentials and name")
{
  PolicyCertificates certificates;
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
  const std::array modes{weave::TlsSessionMode::stateful, weave::TlsSessionMode::tickets};
  for (auto version : versions) {
    for (auto mode : modes) {
      INFO("version ", int(version), " mode ", int(mode));
      auto server_context = weave::TlsContext::server(
        {.certificate_file = certificates.leaf,
          .private_key_file = certificates.private_key,
          .min_version = version,
          .max_version = version,
          .sessions = {.mode = mode}});
      auto client_context = weave::TlsContext::client(
        {.ca_file = certificates.ca, .min_version = version, .max_version = version, .session_resumption = true});
      REQUIRE(server_context);
      REQUIRE(client_context);
      auto server = weave::detail::TlsEngine::create(*server_context, true, "");
      auto client = weave::detail::TlsEngine::create(*client_context, false, "localhost");
      REQUIRE(server);
      REQUIRE(client);
      REQUIRE(handshake(*client, *server));
      std::array<std::byte, 1> byte{std::byte{1}};
      REQUIRE(server->write(byte).action == weave::detail::TlsAction::ready);
      REQUIRE(transfer(*server, *client));
      REQUIRE(client->read(byte).action == weave::detail::TlsAction::ready);
      auto session = client->session();
      REQUIRE(session);
      if (!session)
        return;
      REQUIRE(session->available());
      REQUIRE(session->remaining().count() > 0);
      auto alias = client->session();
      REQUIRE(alias);
      if (!alias)
        return;
      CHECK_FALSE(weave::detail::TlsEngine::create(*client_context, false, "127.0.0.1", &*session));
      auto changed_policy = weave::TlsContext::client({.ca_file = certificates.ca, .session_resumption = true});
      REQUIRE(changed_policy);
      CHECK_FALSE(weave::detail::TlsEngine::create(*changed_policy, false, "localhost", &*session));
      auto resumed_server = weave::detail::TlsEngine::create(*server_context, true, "");
      auto resumed_client = weave::detail::TlsEngine::create(*client_context, false, "localhost", &*session);
      REQUIRE(resumed_server);
      REQUIRE(resumed_client);
      REQUIRE(handshake(*resumed_client, *resumed_server));
      CHECK(resumed_client->session_reused());
      CHECK(resumed_server->session_reused());
      CHECK_FALSE(session->available());
      CHECK_FALSE(alias->available());
      CHECK_FALSE(weave::detail::TlsEngine::create(*client_context, false, "localhost", &*alias));
      CHECK(client_context->session_stats().handshakes == 2);
      CHECK(client_context->session_stats().resumed == 1);
      CHECK(bool(server_context->clear_sessions()) == (mode == weave::TlsSessionMode::stateful));
    }
  }
}

TEST_CASE("Stateful cache invalidation and mTLS resumption preserve verification")
{
  PolicyCertificates certificates;
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
  for (auto version : versions) {
    auto server_context = weave::TlsContext::server(
      {.certificate_file = certificates.leaf,
        .private_key_file = certificates.private_key,
        .min_version = version,
        .max_version = version,
        .client_auth = weave::TlsClientAuth::required,
        .ca_file = certificates.ca,
        .sessions = {.mode = weave::TlsSessionMode::stateful}});
    auto client_context = weave::TlsContext::client(
      {.ca_file = certificates.ca,
        .min_version = version,
        .max_version = version,
        .certificate_file = certificates.client,
        .private_key_file = certificates.client_key,
        .session_resumption = true});
    REQUIRE(server_context);
    REQUIRE(client_context);
    auto server = weave::detail::TlsEngine::create(*server_context, true, "");
    auto client = weave::detail::TlsEngine::create(*client_context, false, "localhost");
    REQUIRE(server);
    REQUIRE(client);
    REQUIRE(handshake(*client, *server));
    std::array<std::byte, 1> byte{std::byte{1}};
    REQUIRE(server->write(byte).action == weave::detail::TlsAction::ready);
    REQUIRE(transfer(*server, *client));
    REQUIRE(client->read(byte).action == weave::detail::TlsAction::ready);
    auto session = client->session();
    REQUIRE(session);
    auto resumed_server = weave::detail::TlsEngine::create(*server_context, true, "");
    auto resumed_client = weave::detail::TlsEngine::create(*client_context, false, "localhost", &*session);
    REQUIRE(resumed_server);
    REQUIRE(resumed_client);
    REQUIRE(handshake(*resumed_client, *resumed_server));
    CHECK(resumed_server->session_reused());
    auto authenticated = server->peer_identity();
    auto resumed_identity = resumed_server->peer_identity();
    REQUIRE(authenticated);
    REQUIRE(resumed_identity);
    CHECK(resumed_identity->sha256 == authenticated->sha256);
    REQUIRE(resumed_server->write(byte).action == weave::detail::TlsAction::ready);
    REQUIRE(transfer(*resumed_server, *resumed_client));
    REQUIRE(resumed_client->read(byte).action == weave::detail::TlsAction::ready);
    auto next_session = resumed_client->session();
    REQUIRE(next_session);
    REQUIRE(server_context->clear_sessions());
    auto fresh_server = weave::detail::TlsEngine::create(*server_context, true, "");
    auto fresh_client = weave::detail::TlsEngine::create(*client_context, false, "localhost", &*next_session);
    REQUIRE(fresh_server);
    REQUIRE(fresh_client);
    REQUIRE(handshake(*fresh_client, *fresh_server));
    CHECK_FALSE(fresh_server->session_reused());
    auto fresh_identity = fresh_server->peer_identity();
    REQUIRE(fresh_identity);
    CHECK(fresh_identity->sha256 == authenticated->sha256);
  }
}

TEST_CASE("Malformed peer records and input bounds produce bounded terminal failures")
{
  PolicyCertificates certificates;
  auto context = weave::TlsContext::server(
    {.certificate_file = certificates.leaf,
      .private_key_file = certificates.private_key,
      .limits = {.buffered_input = 65536, .buffered_output = 65536}});
  REQUIRE(context);

  auto bounded = weave::detail::TlsEngine::create(*context, true, "");
  REQUIRE(bounded);
  std::vector<std::byte> excessive(65537);
  auto input = bounded->input(excessive);
  REQUIRE_FALSE(input);
  CHECK(input.error() == weave::TlsError::resource_limit);
  CHECK(bounded->handshake().error == weave::TlsError::resource_limit);

  weave::u32 random = 0x72839415;
  for (int trial = 0; trial < 512; ++trial) {
    auto server = weave::detail::TlsEngine::create(*context, true, "");
    REQUIRE(server);
    std::vector<std::byte> bytes(static_cast<std::size_t>(trial % 128) + 5);
    for (auto &byte : bytes) {
      random ^= random << 13;
      random ^= random >> 17;
      random ^= random << 5;
      byte = static_cast<std::byte>(random & 0xff);
    }
    if (trial % 2 == 0) {
      bytes[0] = std::byte{22};
      bytes[1] = std::byte{3};
      bytes[2] = std::byte{3};
      bytes[3] = std::byte{0};
      bytes[4] = static_cast<std::byte>(bytes.size() - 5);
    }
    REQUIRE(server->input(bytes));
    auto step = server->handshake();
    CHECK(step.action != weave::detail::TlsAction::ready);
    if (step.action == weave::detail::TlsAction::input) {
      REQUIRE(server->input({}));
      CHECK(server->handshake().action == weave::detail::TlsAction::failed);
    }
  }
}

TEST_CASE("Handshake deadlines cancel and drain a silent transport peer")
{
  using namespace std::chrono_literals;
  PolicyCertificates certificates;
  auto credentials = weave::TlsContext::client({.ca_file = certificates.ca});
  auto ctx = weave::Context::create();
  REQUIRE(credentials);
  REQUIRE(ctx);
  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  bool timed_out = false;
  auto silent = [&]() -> weave::Task<void> {
    auto peer = co_await listener->accept();
    co_await weave::sleep_for(30ms);
  };
  auto connecting = [&]() -> weave::Task<void> {
    auto transport = co_await weave::tcp::connect("127.0.0.1", listener->local_port());
    auto result = co_await weave::as_result(
      weave::tls::client(std::move(transport), *credentials, "localhost", {.timeout = 2ms}));
    timed_out = !result && result.error() == std::errc::timed_out;
  };
  REQUIRE(ctx->run(weave::timeout(5s, weave::when_all(silent(), connecting()))));
  CHECK(timed_out);
}

TEST_CASE("Captured sessions cannot be offered after their lifetime expires")
{
  PolicyCertificates certificates;
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
  for (auto version : versions) {
    CAPTURE(version);
    // Native expiry is in whole seconds; allow capture to cross a second boundary safely.
    constexpr auto lifetime = std::chrono::seconds{3};
    auto server_context = weave::TlsContext::server(
      {.certificate_file = certificates.leaf,
        .private_key_file = certificates.private_key,
        .min_version = version,
        .max_version = version,
        .sessions = {.mode = weave::TlsSessionMode::stateful, .lifetime = lifetime}});
    auto client_context = weave::TlsContext::client(
      {.ca_file = certificates.ca,
        .min_version = version,
        .max_version = version,
        .session_resumption = true,
        .session_lifetime = lifetime});
    REQUIRE(server_context);
    REQUIRE(client_context);
    if (!server_context || !client_context)
      return;

    auto server = weave::detail::TlsEngine::create(*server_context, true, "");
    auto client = weave::detail::TlsEngine::create(*client_context, false, "localhost");
    REQUIRE(server);
    REQUIRE(client);
    if (!server || !client)
      return;

    REQUIRE(handshake(*client, *server));

    std::array<std::byte, 1> byte{std::byte{1}};
    REQUIRE(server->write(byte).action == weave::detail::TlsAction::ready);
    REQUIRE(transfer(*server, *client));
    REQUIRE(client->read(byte).action == weave::detail::TlsAction::ready);
    auto session = client->session();
    REQUIRE(session);
    if (!session)
      return;

    // OpenSSL session expiry uses wall-clock seconds, not Context's timer clock.
    std::this_thread::sleep_for(lifetime + std::chrono::milliseconds{100});
    CHECK_FALSE(session->available());
    CHECK(session->remaining().count() == 0);
    auto expired = weave::detail::TlsEngine::create(*client_context, false, "localhost", &*session);
    CHECK_FALSE(expired);
    if (!expired)
      CHECK(expired.error() == weave::TlsError::session_rejected);
  }
}

TEST_CASE("Lazy TLS factories own credentials before execution starts")
{
  using Stream = weave::TlsStream<weave::TcpStream>;
  fixture::Certificates certificates;
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  // Serve both localhost families so this lifetime test does not wait for native refusal retries.
  auto listener = weave::tcp::listen(*ctx, {weave::IpAddress::any_v6(), 0}, {.ipv6_only = false});
  REQUIRE(listener);

  std::optional<weave::Task<Stream>> connecting;
  {
    auto credentials = weave::TlsContext::client({.ca_file = certificates.ca});
    REQUIRE(credentials);
    connecting.emplace(weave::tls::connect(*credentials, "localhost", listener->local_port()));
  }

  auto accepted = [&]() -> weave::Task<void> {
    auto transport = co_await listener->accept();
    std::optional<weave::Task<Stream>> upgrading;
    {
      auto credentials = weave::TlsContext::server(
        {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key});
      REQUIRE(credentials);
      upgrading.emplace(weave::tls::server(std::move(transport), *credentials));
    }

    auto peer = co_await std::move(*upgrading);
    std::array<std::byte, 4> message;
    co_await peer.read_exactly(message);
    co_await peer.write_all(message);
    co_await peer.shutdown();
  };
  auto connected = [&]() -> weave::Task<void> {
    auto peer = co_await std::move(*connecting);
    const std::array message{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    std::array<std::byte, 4> reply;
    co_await peer.write_all(message);
    co_await peer.read_exactly(reply);
    CHECK(reply == message);
    co_await peer.shutdown();
  };
  auto result = ctx->run(weave::timeout(std::chrono::seconds{5}, weave::when_all(accepted(), connected())));
  CAPTURE(result ? std::string{} : result.error().message());
  CAPTURE(result ? 0 : result.error().value());
  REQUIRE(result);
}

TEST_CASE("Session captures survive graceful shutdown but not failure or unclean destruction")
{
  fixture::Certificates certificates;
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
  const std::array scenarios{0, 1, 2, 3, 4, 5};
  for (auto version : versions) {
    auto server_context = weave::TlsContext::server(
      {.certificate_file = certificates.leaf,
        .private_key_file = certificates.private_key,
        .min_version = version,
        .max_version = version,
        .sessions = {.mode = weave::TlsSessionMode::stateful}});
    auto client_context = weave::TlsContext::client(
      {.ca_file = certificates.ca, .min_version = version, .max_version = version, .session_resumption = true});
    REQUIRE(server_context);
    REQUIRE(client_context);

    for (auto scenario : scenarios) {
      INFO("version ", int(version), " scenario ", scenario);
      std::optional<weave::TlsSession> saved;
      {
        auto server = weave::detail::TlsEngine::create(*server_context, true, "");
        auto client = weave::detail::TlsEngine::create(*client_context, false, "localhost");
        REQUIRE(server);
        REQUIRE(client);
        REQUIRE(handshake(*client, *server));
        std::array<std::byte, 1> byte{std::byte{1}};
        REQUIRE(server->write(byte).action == weave::detail::TlsAction::ready);
        REQUIRE(transfer(*server, *client));
        REQUIRE(client->read(byte).action == weave::detail::TlsAction::ready);
        auto session = client->session();
        REQUIRE(session);
        saved.emplace(std::move(*session));

        if (scenario == 1 || scenario == 3) {
          const auto error = scenario == 1 ? weave::make_error_code(weave::TlsError::protocol)
                                           : std::make_error_code(std::errc::operation_canceled);
          client->fail(error);
          CHECK_FALSE(saved->available());
          CHECK_FALSE(client->session());
        }
        if (scenario == 2) {
          REQUIRE(client->shutdown(false).action == weave::detail::TlsAction::ready);
          REQUIRE(transfer(*client, *server));
          REQUIRE(server->shutdown(false).action == weave::detail::TlsAction::ready);
          REQUIRE(transfer(*server, *client));
          REQUIRE(client->shutdown(true).action == weave::detail::TlsAction::ready);
          REQUIRE(server->shutdown(true).action == weave::detail::TlsAction::ready);
        }
        if (scenario == 4) {
          auto resuming = weave::detail::TlsEngine::create(*client_context, false, "localhost", &*saved);
          auto accepting = weave::detail::TlsEngine::create(*server_context, true, "");
          REQUIRE(resuming);
          REQUIRE(accepting);
          client->fail(weave::make_error_code(weave::TlsError::protocol));
          CHECK_FALSE(handshake(*resuming, *accepting));
          CHECK(resuming->handshake().error == weave::TlsError::session_rejected);
        }
        if (scenario == 5) {
          REQUIRE(server->write(byte).action == weave::detail::TlsAction::ready);
          std::array<std::byte, 16384> ciphertext;
          auto count = server->output(ciphertext);
          REQUIRE(count);
          REQUIRE(*count > 0);
          ciphertext[*count - 1] ^= std::byte{1};
          REQUIRE(client->input(std::span{ciphertext}.first(*count)));
          CHECK(client->read(byte).action == weave::detail::TlsAction::failed);
          CHECK_FALSE(saved->available());
        }
      }
      CHECK(saved->available() == (scenario == 2));
    }
  }
}

TEST_CASE("PEM, DER and provider STORE keys authenticate both mTLS roles")
{
  KeyFormatCertificates files;
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
  const std::array formats{
    weave::TlsPrivateKeyFormat::pem,
    weave::TlsPrivateKeyFormat::der,
    weave::TlsPrivateKeyFormat::store};
  for (auto version : versions) {
    for (auto format : formats) {
      std::string server_key = files.server_der.string();
      std::string client_key = files.client_der.string();
      if (format == weave::TlsPrivateKeyFormat::pem) {
        server_key = files.private_key;
        client_key = files.client_key;
      } else if (format == weave::TlsPrivateKeyFormat::store) {
        server_key = files.server_der.generic_string();
        if (!server_key.starts_with('/'))
          server_key.insert(0, "/");
        server_key.insert(0, "file:");
      }

      auto server = weave::TlsContext::server(
        {.certificate_file = files.leaf,
          .private_key_file = server_key,
          .min_version = version,
          .max_version = version,
          .client_auth = weave::TlsClientAuth::required,
          .ca_file = files.ca,
          .private_key_format = format});
      auto client = weave::TlsContext::client(
        {.ca_file = files.ca,
          .min_version = version,
          .max_version = version,
          .certificate_file = files.client,
          .private_key_file = client_key,
          .private_key_format = format});
      REQUIRE(server);
      REQUIRE(client);
      if (!server || !client)
        continue;

      auto server_engine = weave::detail::TlsEngine::create(*server, true, "");
      auto client_engine = weave::detail::TlsEngine::create(*client, false, "localhost");
      REQUIRE(server_engine);
      REQUIRE(client_engine);
      if (!server_engine || !client_engine)
        continue;
      CHECK(handshake(*client_engine, *server_engine));
    }
  }
}

TEST_CASE("STORE loaders never retain password callbacks or accept ambiguous identities")
{
  KeyFormatCertificates files;
  unsigned calls = 0;
  std::weak_ptr<int> lifetime;
  std::optional<weave::TlsContext> server;
  {
    auto owner = std::make_shared<int>(0);
    lifetime = owner;
    auto provider = weave::TlsPasswordProvider::create(
      [owner, &calls](std::string_view) noexcept -> weave::Result<std::string> {
        ++calls;
        return "store-password";
      });
    REQUIRE(provider);
    if (!provider)
      return;
    auto credentials = weave::TlsContext::server(
      {.certificate_file = files.leaf,
        .private_key_file = files.encrypted.string(),
        .private_key_password_provider = *provider,
        .private_key_format = weave::TlsPrivateKeyFormat::store});
    REQUIRE(credentials);
    if (!credentials)
      return;
    server = *credentials;
  }
  CHECK(lifetime.expired());
  CHECK(calls > 0);

  auto client = weave::TlsContext::client({.ca_file = files.ca});
  REQUIRE(client);
  if (!client)
    return;
  auto accepting = weave::detail::TlsEngine::create(*server, true, "");
  auto initiating = weave::detail::TlsEngine::create(*client, false, "localhost");
  REQUIRE(accepting);
  REQUIRE(initiating);
  if (accepting && initiating)
    CHECK(handshake(*initiating, *accepting));

  CHECK_FALSE(
    weave::TlsContext::server(
      {.certificate_file = files.leaf,
        .private_key_file = files.encrypted.string(),
        .private_key_password = "wrong",
        .private_key_format = weave::TlsPrivateKeyFormat::store}));
  CHECK_FALSE(
    weave::TlsContext::server(
      {.certificate_file = files.leaf,
        .private_key_file = files.ca,
        .private_key_format = weave::TlsPrivateKeyFormat::store}));
  CHECK_FALSE(
    weave::TlsContext::server(
      {.certificate_file = files.leaf,
        .private_key_file = files.multiple.string(),
        .private_key_format = weave::TlsPrivateKeyFormat::store}));
  CHECK_FALSE(
    weave::TlsContext::server(
      {.certificate_file = files.leaf,
        .private_key_file = "not-a-provider:key",
        .private_key_format = weave::TlsPrivateKeyFormat::store}));
  CHECK_FALSE(
    weave::TlsContext::server(
      {.certificate_file = files.leaf,
        .private_key_file = files.private_key,
        .private_key_format = static_cast<weave::TlsPrivateKeyFormat>(255)}));
}
