#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/port.hpp>
#include <weave/log.hpp>
#include "tls_certificates.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <filesystem>

namespace pg = weave::pg;
using namespace std::chrono_literals;
static std::atomic<int> checks = 0;

static void check(bool value, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Postgres metadata check failed: %s:%u\n", location.file_name(), location.line());
    std::exit(1);
  }
}

static void metadata(const pg::ConnectionInfo &info, const pg::Options &options, bool local = false)
{
  auto host = options.hosts.empty() ? options.host : options.hosts.front().name;
  check(info.host == host && info.port == options.port);
  check(info.user == options.user);
  check(info.database == (options.database.empty() ? options.user : options.database));
  check(info.server_options == options.server_options);
  check(!info.server_version.empty() && info.backend_process != 0);
  check(info.protocol_version == pg::ProtocolVersion::v32);
  check(info.authentication.method == (local ? pg::Authentication::none : pg::Authentication::scram_sha256));
  check(info.authentication.complete);
  check(info.authentication.password_requested == !local);
  check(!info.authentication.password_missing);
  check(!info.gss_encrypted);
  check(info.endpoint.has_value() != local && info.local_address.has_value() == local);
  if (local) {
    check(info.peer_user.has_value() && info.local_address->ends_with(".s.PGSQL." + std::to_string(options.port)));
  } else {
    check(info.endpoint->port == options.port && !info.peer_user);
    if (options.hosts.front().address)
      check(info.endpoint->address == *options.hosts.front().address);
  }
  check(info.tls.has_value() == !options.plaintext);
  if (info.tls) {
    check(info.tls->library == "OpenSSL" && info.tls->key_bits >= 128 && !info.tls->compression);
    check(!info.tls->cipher.empty() && info.tls->peer && !info.tls->peer->certificate.empty());
    check(info.tls->peer->sha256.size() == 32 && !info.tls->session_reused);
  }
}

static void tls_configuration(const pg::TlsOptionsInfo &info, const weave::TlsClientOptions &options)
{
  check(info.ca_file == options.ca_file && info.ca_directory == options.ca_directory);
  check(info.certificate_file == options.certificate_file && info.private_key_file == options.private_key_file);
  check(info.min_version == options.min_version && info.max_version == options.max_version);
  check(info.alpn == options.alpn && info.crl_file == options.crl_file);
  check(info.revocation == options.revocation && info.ocsp == options.ocsp);
  check(info.session_resumption == options.session_resumption && info.session_lifetime == options.session_lifetime);
  check(info.ocsp_max_age == options.ocsp_max_age && info.ocsp_clock_skew == options.ocsp_clock_skew);
  check(info.limits.buffered_input == options.limits.buffered_input);
  check(info.limits.buffered_output == options.limits.buffered_output);
  check(info.limits.certificate_chain == options.limits.certificate_chain);
  check(info.limits.verification_depth == options.limits.verification_depth);
  check(info.ciphers.tls12 == options.ciphers.tls12 && info.ciphers.tls13 == options.ciphers.tls13);
  check(info.ciphers.groups == options.ciphers.groups && info.ciphers.signatures == options.ciphers.signatures);
  check(info.ciphers.security_level == options.ciphers.security_level);
}

template <class C>
static pg::OptionsInfo configuration(C &connection, const pg::Options &options)
{
  auto info = connection.configuration();
  check(info.has_value());
  check(info->host == options.host && info->port == options.port);
  check(info->user == options.user && info->database == options.database);
  check(info->application_name == options.application_name && info->client_encoding == options.client_encoding);
  check(info->server_options == options.server_options && info->settings == options.settings);
  check(info->hosts.size() == options.hosts.size());
  for (std::size_t index = 0; index < info->hosts.size(); ++index) {
    check(info->hosts[index].name == options.hosts[index].name);
    check(info->hosts[index].port == options.hosts[index].port);
    check(info->hosts[index].address == options.hosts[index].address);
  }
  check(info->tls_context == options.tls.has_value() && info->plaintext == options.plaintext);
  check(info->tls_options.has_value() == options.tls_options.has_value());
  if (info->tls_options) {
    tls_configuration(*info->tls_options, *options.tls_options);
    info->tls_options->ca_file = "mutated-copy";
    check(connection.configuration()->tls_options->ca_file == options.tls_options->ca_file);
    info->tls_options->ca_file = options.tls_options->ca_file;
  }
  check(info->authentication.methods == options.authentication.methods);
  check(info->authentication.exclude == options.authentication.exclude);
  check(info->connect_timeout == options.connect_timeout);
  check(!info->gss_context && !info->oauth && !info->origin);
  return *info;
}

static weave::Task<void> failure_reset(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  auto before = configuration(connection, options);
  auto attempted = options;
  attempted.user = "weave_replication";
  attempted.application_name = "must-not-install";
  attempted.password = "deliberately-not-the-fixture-password";
  auto result = co_await weave::as_result(connection.reset(attempted));
  check(!result && !connection.open());
  check(connection.transaction() == pg::Transaction::unknown);
  configuration(connection, options);
  check(connection.last_failure()->error == result.error());
  check(before.user == options.user && before.application_name == options.application_name);
  attempted = options;
  attempted.application_name = "after-recovery";
  co_await connection.reset(attempted);
  check(connection.transaction() == pg::Transaction::idle);
  configuration(connection, attempted);
  check(!connection.last_failure()->error);
  co_await connection.finish();
  check(connection.transaction() == pg::Transaction::unknown);
  configuration(connection, attempted);
}

static weave::Task<void> failure_transport(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  auto before = configuration(connection, options);
  auto result = co_await weave::as_result(connection.query("SELECT pg_terminate_backend(pg_backend_pid())"));
  check(!result && !connection.open());
  check(connection.transaction() == pg::Transaction::unknown);
  configuration(connection, options);
  check(connection.last_failure()->error == result.error());
  check(before.user == options.user && before.application_name == options.application_name);
}

static weave::Task<void> pipeline_status(pg::Connection &connection)
{
  check(connection.pipeline_status() == pg::PipelineStatus::off);
  {
    auto unsent = connection.pipeline();
    check(unsent && unsent->execute({"SELECT 1"}));
    check(connection.pipeline_status() == pg::PipelineStatus::on);
  }
  check(connection.open() && connection.pipeline_status() == pg::PipelineStatus::off);
  check(connection.transaction() == pg::Transaction::idle);

  {
    auto pipeline = connection.pipeline();
    check(pipeline && pipeline->execute({"SELECT 1/0"}));
    co_await pipeline->flush();
    check(connection.open() && connection.pipeline_status() == pg::PipelineStatus::aborted);
    check(pipeline->aborted() && !pipeline->finish());
    check(pipeline->sync().has_value());
    check(connection.pipeline_status() == pg::PipelineStatus::aborted);
    co_await pipeline->flush();
    check(connection.pipeline_status() == pg::PipelineStatus::on && !pipeline->aborted());
    auto error = co_await pipeline->next();
    auto barrier = co_await pipeline->next();
    check(error && error->outcome.error.sqlstate() == "22012");
    check(barrier && barrier->kind == pg::PipelineKind::sync && barrier->transaction == pg::Transaction::idle);
    check(!(co_await pipeline->next()));
    check(pipeline->finish().has_value());
    check(connection.pipeline_status() == pg::PipelineStatus::off);
  }

  {
    auto pipeline = connection.pipeline();
    check(pipeline && pipeline->execute({"BEGIN"}) && pipeline->execute({"SELECT 1/0"}) && pipeline->sync());
    co_await pipeline->flush();
    check(connection.pipeline_status() == pg::PipelineStatus::on);
    check(connection.transaction() == pg::Transaction::failed && !pipeline->aborted());
    unsigned results = 0;
    while (co_await pipeline->next())
      ++results;
    check(results == 3 && pipeline->finish());
  }
  check(connection.pipeline_status() == pg::PipelineStatus::off);
  co_await connection.query("ROLLBACK");
  check(connection.transaction() == pg::Transaction::idle);
}

static void pipeline_status(pg::BlockingConnection &connection)
{
  check(connection.pipeline_status() == pg::PipelineStatus::off);
  {
    auto unsent = connection.pipeline();
    check(unsent && unsent->execute({"SELECT 1"}));
    check(connection.pipeline_status() == pg::PipelineStatus::on);
  }
  check(connection.open() && connection.pipeline_status() == pg::PipelineStatus::off);
  check(connection.transaction() == pg::Transaction::idle);

  {
    auto pipeline = connection.pipeline();
    check(pipeline && pipeline->execute({"SELECT 1/0"}) && pipeline->flush());
    check(connection.open() && connection.pipeline_status() == pg::PipelineStatus::aborted);
    check(pipeline->aborted() && !pipeline->finish());
    check(pipeline->sync().has_value());
    check(connection.pipeline_status() == pg::PipelineStatus::aborted);
    check(pipeline->flush().has_value());
    check(connection.pipeline_status() == pg::PipelineStatus::on && !pipeline->aborted());
    auto error = pipeline->next();
    auto barrier = pipeline->next();
    check(error && *error && (**error).outcome.error.sqlstate() == "22012");
    check(barrier && *barrier && (**barrier).kind == pg::PipelineKind::sync);
    check((**barrier).transaction == pg::Transaction::idle);
    auto end = pipeline->next();
    check(end && !*end && pipeline->finish());
    check(connection.pipeline_status() == pg::PipelineStatus::off);
  }

  {
    auto pipeline = connection.pipeline();
    check(pipeline && pipeline->execute({"BEGIN"}) && pipeline->execute({"SELECT 1/0"}) && pipeline->sync());
    check(pipeline->flush().has_value());
    check(connection.pipeline_status() == pg::PipelineStatus::on);
    check(connection.transaction() == pg::Transaction::failed && !pipeline->aborted());
    unsigned results = 0;
    for (;;) {
      auto next = pipeline->next();
      check(next.has_value());
      if (!*next)
        break;
      ++results;
    }
    check(results == 3 && pipeline->finish());
  }
  check(connection.pipeline_status() == pg::PipelineStatus::off);
  check(connection.query("ROLLBACK").has_value());
  check(connection.transaction() == pg::Transaction::idle);
}

static weave::Task<void> session(pg::Options options, bool local = false)
{
  auto connection = co_await pg::connect(options);
  check(connection.transaction() == pg::Transaction::idle);
  auto info = connection.info();
  check(static_cast<bool>(info));
  metadata(*info, options, local);
  auto configured = configuration(connection, options);
  auto retained = *info;
  auto query = co_await connection.execute(
    "SELECT current_user, current_database(), pg_backend_pid(), current_setting('server_version'), "
    "current_setting('server_version_num')");
  check(query.rows.size() == 1 && query.rows[0].size() == 5);
  check(query.rows[0][0].bytes() == info->user && query.rows[0][1].bytes() == info->database);
  check(query.rows[0][2].integer<weave::u32>() == weave::Result<weave::u32>{info->backend_process});
  check(query.rows[0][3].bytes() == info->server_version);
  check(query.rows[0][4].integer<weave::u32>() == weave::Result<weave::u32>{info->server_version_number});
  co_await connection.set_client_encoding(pg::Encoding::latin1);
  check(connection.parameter("client_encoding")->value() == "LATIN1");
  check(connection.configuration()->client_encoding == configured.client_encoding);
  {
    auto pending = connection.query("SELECT 1");
    check(connection.transaction() == pg::Transaction::idle);
    auto busy = connection.info();
    check(!busy && busy.error() == pg::make_error_code(pg::Error::busy));
    auto rejected = connection.configuration();
    check(!rejected && rejected.error() == pg::Error::busy);
  }
  check(static_cast<bool>(connection.info()));
  {
    auto pipeline = connection.pipeline();
    check(static_cast<bool>(pipeline));
    check(connection.transaction() == pg::Transaction::idle);
    auto busy = connection.info();
    check(!busy && busy.error() == pg::make_error_code(pg::Error::busy));
    auto rejected = connection.configuration();
    check(!rejected && rejected.error() == pg::Error::busy);
    check(static_cast<bool>(pipeline->finish()));
  }
  co_await pipeline_status(connection);
  check(connection
      .on_trace(
        {.handler =
            [&](const pg::TraceMessage &message) noexcept {
              const bool request = message.direction == pg::TraceDirection::frontend && message.kind != 'X';
              const bool ready = message.direction == pg::TraceDirection::backend && message.kind == 'Z';
              if (request || ready)
                check(connection.transaction() == pg::Transaction::in_progress);
            }})
      .has_value());

  co_await connection.query("BEGIN");
  check(connection.info()->transaction == pg::Transaction::active);
  check(connection.transaction() == pg::Transaction::active);
  auto bad = co_await weave::as_result(connection.query("SELECT undefined_metadata_column"));
  check(!bad && connection.info()->transaction == pg::Transaction::failed);
  check(connection.transaction() == pg::Transaction::failed);
  co_await connection.query("ROLLBACK");
  check(connection.info()->transaction == pg::Transaction::idle);
  check(connection.transaction() == pg::Transaction::idle);

  co_await connection.start_rows("SELECT 'value'");
  check(connection.transaction() == pg::Transaction::in_progress);
  check(connection.info()->transaction == pg::Transaction::in_progress);
  auto row = co_await connection.read_row();
  check(row && row->size() == 1 && (*row)[0].bytes() == "value");
  check(connection.transaction() == pg::Transaction::in_progress);
  check(!(co_await connection.read_row()));
  check(connection.transaction() == pg::Transaction::idle);

  co_await connection.start_copy("COPY (SELECT 'value') TO STDOUT");
  check(connection.transaction() == pg::Transaction::in_progress);
  check(connection.info()->transaction == pg::Transaction::in_progress);
  check((co_await connection.read_copy()).has_value());
  check(connection.transaction() == pg::Transaction::in_progress);
  check(!(co_await connection.read_copy()));
  check(connection.transaction() == pg::Transaction::idle);
  check(connection.copy_result().has_value());
  check(connection.on_trace({}).has_value());

  co_await connection.finish();
  check(connection.transaction() == pg::Transaction::unknown);
  check(!connection.info() && connection.info().error() == pg::make_error_code(pg::Error::closed));
  metadata(retained, options, local);
  configuration(connection, options);
}

static weave::Task<void> reset(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  auto before = connection.info();
  auto configured = configuration(connection, options);
  check(static_cast<bool>(before));
  auto next = options;
  next.user = "weave_replication";
  co_await connection.reset(next);
  auto after = connection.info();
  auto changed = configuration(connection, next);
  check(changed.user == next.user && configured.user == options.user);
  check(after && after->user == next.user && before->user == options.user);
  check(after->backend_process != before->backend_process);
  auto moved = std::move(connection);
  check(connection.transaction() == pg::Transaction::unknown);
  check(moved.transaction() == pg::Transaction::idle);
  check(!connection.info() && connection.info().error() == pg::make_error_code(pg::Error::closed));
  metadata(*moved.info(), next);
  co_await moved.finish();
  check(moved.transaction() == pg::Transaction::unknown);
}

int main()
{
  static fixture::Certificates certificates;
  std::printf("%s\n%s\n%s\n", certificates.ca.c_str(), certificates.leaf.c_str(), certificates.private_key.c_str());
  std::fflush(stdout);
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  std::string port_text, password, standby, address_text, local;
  std::getline(std::cin, port_text);
  std::getline(std::cin, password);
  std::getline(std::cin, standby);
  std::getline(std::cin, address_text);
  std::getline(std::cin, local);
  std::printf("Owned certificate fixture: %s\n", certificates.directory.string().c_str());
  std::printf("OpenSSL: %s\n", OpenSSL_version(OPENSSL_VERSION));
  auto port = weave::parse_port(port_text);
  auto address = weave::IpAddress::parse(address_text);
  check(port && address);
  auto credentials = weave::TlsContext::client(
    {.ca_file = certificates.ca, .certificate_file = certificates.client, .private_key_file = certificates.client_key});
  check(static_cast<bool>(credentials));
  pg::Options plain;
  plain.host = "unused.invalid";
  plain.hosts = {{"localhost", *port, *address}};
  plain.port = *port;
  plain.user = "weave";
  plain.database = "postgres";
  plain.password = password;
  plain.plaintext = true;
  plain.server_options = "-c statement_timeout=0";
  auto secured = plain;
  secured.plaintext = false;
  secured.tls = *credentials;
  secured.channel_binding = pg::ChannelBinding::require;
  auto files = plain;
  files.plaintext = false;
  files.channel_binding = pg::ChannelBinding::require;
  files.tls_options = weave::TlsClientOptions{
    .ca_file = certificates.ca,
    .certificate_file = certificates.client,
    .private_key_file = certificates.client_key,
    .session_resumption = true,
    .session_lifetime = 237s,
    .ocsp_max_age = 1234s,
    .ocsp_clock_skew = 23s,
    .limits = {123456, 234567, 34567, 7},
    .ciphers = {
      .tls12 = "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES128-GCM-SHA256",
      .tls13 = "TLS_AES_128_GCM_SHA256",
      .groups = "X25519:P-256",
      .security_level = 2}};

  auto encrypted = files;
  auto encrypted_path = certificates.directory / "client-encrypted.pem";

  struct KeyFile {
    std::filesystem::path path;

    ~KeyFile()
    {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  };

  static KeyFile key_file{encrypted_path};

  {
    std::unique_ptr<BIO, decltype(&BIO_free)> input{BIO_new_file(certificates.client_key.c_str(), "r"), BIO_free};
    check(input != nullptr);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{
      PEM_read_bio_PrivateKey(input.get(), nullptr, nullptr, nullptr),
      EVP_PKEY_free};
    check(key != nullptr);
    std::unique_ptr<BIO, decltype(&BIO_free)> output{BIO_new_file(encrypted_path.string().c_str(), "w"), BIO_free};
    check(output != nullptr);
    encrypted.tls_options->private_key_password = "fixture-only-key-passphrase";
    auto &passphrase = encrypted.tls_options->private_key_password;
    check(
      PEM_write_bio_PrivateKey(
        output.get(),
        key.get(),
        EVP_aes_256_cbc(),
        reinterpret_cast<const unsigned char *>(passphrase.data()),
        static_cast<int>(passphrase.size()),
        nullptr,
        nullptr) == 1);
  }
  encrypted.tls_options->private_key_file = encrypted_path.string();
  auto ctx = weave::Context::create();
  check(static_cast<bool>(ctx));
  const std::array profiles{plain, secured, files, encrypted};
  std::printf("Configuration profiles: plaintext, opaque-mtls, file-mtls, encrypted-key-mtls\n");
  for (const auto &profile : profiles) {
    auto result = ctx->run(weave::timeout(30s, session(profile)));
    if (!result)
      return weave::report_error(result.error());
    result = ctx->run(weave::timeout(30s, reset(profile)));
    if (!result)
      return weave::report_error(result.error());
    result = ctx->run(weave::timeout(30s, failure_reset(profile)));
    if (!result)
      return weave::report_error(result.error());
    result = ctx->run(weave::timeout(30s, failure_transport(profile)));
    if (!result)
      return weave::report_error(result.error());
    auto blocking = pg::BlockingConnection::connect(profile);
    check(static_cast<bool>(blocking));
    auto info = blocking->info();
    check(static_cast<bool>(info));
    metadata(*info, profile);
    configuration(*blocking, profile);
    pipeline_status(*blocking);
    auto moved = std::move(*blocking);
    check(blocking->transaction() == pg::Transaction::unknown);
    check(moved.transaction() == pg::Transaction::idle);
    check(!blocking->info() && blocking->info().error() == pg::make_error_code(pg::Error::closed));
    check(static_cast<bool>(moved.finish()));
    check(moved.transaction() == pg::Transaction::unknown);
    metadata(*info, profile);
    check(!moved.info());
    configuration(moved, profile);
  }
  if (!local.empty()) {
    auto socket = plain;
    socket.host = local;
    socket.hosts.clear();
    auto result = ctx->run(weave::timeout(30s, session(socket, true)));
    if (!result)
      return weave::report_error(result.error());
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    std::printf(
      "Configuration runtime: workers=4 roots=64 scheduler=%s io=sharded profiles=4\n",
      scheduler == weave::Scheduler::worker_affine ? "affine" : "stealing");
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    check(static_cast<bool>(runtime));
    std::vector<weave::JoinHandle<void>> jobs;
    for (std::size_t index = 0; index < 64; ++index) {
      auto job = runtime->spawn(weave::timeout(30s, session(profiles[index % profiles.size()])));
      check(static_cast<bool>(job));
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!result)
        return weave::report_error(result.error());
    }
  }
#endif
  std::printf("Postgres metadata live controls passed: %d checks\n", checks.load());
}
