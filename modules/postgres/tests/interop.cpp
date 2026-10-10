#include <weave/postgres.hpp>
#include <weave/io.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include "tls_certificates.hpp"
#include <cstdio>
#include <iostream>
#include <array>
#include <algorithm>

static weave::Task<void> check(weave::pg::Options options, bool expected_failure)
{
  auto connection = co_await weave::as_result(weave::pg::connect(std::move(options)));
  if (expected_failure) {
    if (connection ||
      (connection.error() != weave::pg::Error::authentication &&
        connection.error() != weave::pg::Error::unsupported_authentication &&
        connection.error() != weave::pg::Error::protocol && weave::pg::sqlstate(connection.error()) != "28P01"))
      co_await weave::fail(std::errc::bad_message);

    co_return;
  }

  if (!connection)
    co_await weave::fail(connection.error());

  if (connection->authentication_method() != weave::pg::Authentication::scram_sha256 ||
    connection->protocol_version() != weave::pg::ProtocolVersion::v32)
    co_await weave::fail(std::errc::bad_message);

  auto result = co_await connection->query("SELECT 42");
  if (result.size() != 1 || result[0].rows.size() != 1 || result[0].rows[0][0].integer<int>() != 42)
    co_await weave::fail(std::errc::bad_message);

  co_await connection->finish();
}

int main(int argc, char **argv)
{
  if (argc != 2)
    return 2;

  std::string mode = argv[1];
  fixture::Certificates certificates;
  std::printf("%s\n%s\n%s\n", certificates.ca.c_str(), certificates.leaf.c_str(), certificates.private_key.c_str());
  std::fflush(stdout);
  std::string line;
  std::getline(std::cin, line);
  auto port = weave::parse_port(line);
  if (!port)
    return 2;

  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  weave::pg::Options options;
  options.host = "127.0.0.1";
  options.port = *port;
  options.user = "weave";
  options.password = mode == "unicode" ? "I\xc2\xadX" : "pencil";
  options.plaintext = mode != "tls" && mode != "binding" && mode != "keys_tls";
  if (mode == "allow" || mode == "exclude") {
    options.authentication.methods = {weave::pg::Authentication::scram_sha256};
    options.authentication.exclude = mode == "exclude";
  } else if (mode == "deny") {
    options.authentication.methods = {weave::pg::Authentication::password, weave::pg::Authentication::md5};
  } else if (mode == "optional") {
    options.authentication.methods = {weave::pg::Authentication::none};
    options.authentication.exclude = true;
  }
  if (mode.starts_with("keys_")) {
    auto client_key = weave::pg::ScramKey::parse("Codc/xpog7iN42jX2M/Kzb0EYyzwnhS38XdLARZUmGc=");
    auto server_key = weave::pg::ScramKey::parse("2MBPkZDzn+Pg6MZjW44WbB3uM/DpW2q3cpTa0wpqb8g=");
    if (!client_key || !server_key)
      return 2;
    options.scram_client_key = std::move(*client_key);
    options.scram_server_key = std::move(*server_key);
    options.password.clear();
    if (mode == "keys_wrong_password")
      options.password = "not-the-user-password";
    if (mode == "keys_client_password" || mode == "keys_no_server_no_password") {
      options.scram_server_key.reset();
      if (mode == "keys_client_password")
        options.password = "pencil";
    }
    if (mode == "keys_server_password") {
      options.scram_client_key.reset();
      options.password = "pencil";
    }
    const std::array<std::byte, 32> wrong{};
    if (mode == "keys_wrong_client")
      options.scram_client_key.emplace(wrong);
    if (mode == "keys_wrong_server")
      options.scram_server_key.emplace(wrong);
    if (mode == "keys_deny")
      options.authentication.methods = {weave::pg::Authentication::password};
    if (mode == "keys_parse") {
      auto parsed = weave::pg::Options::parse(
        "host=127.0.0.1 user=weave sslmode=disable "
        "require_auth=scram-sha-256 "
        "scram_client_key=Codc/xpog7iN42jX2M/Kzb0EYyzwnhS38XdLARZUmGc= "
        "scram_server_key=2MBPkZDzn+Pg6MZjW44WbB3uM/DpW2q3cpTa0wpqb8g=");
      if (!parsed)
        return weave::report_error(parsed.error());
      options = std::move(*parsed);
      options.port = *port;
      options.hosts.front().port = *port;
    }
  }
  if (!options.plaintext) {
    auto tls = weave::TlsContext::client({.ca_file = certificates.ca});
    if (!tls)
      return weave::report_error(tls.error());

    options.tls = *tls;
    options.channel_binding = weave::pg::ChannelBinding::require;
  }

  const std::array successful_modes{
    "plain",
    "tls",
    "unicode",
    "allow",
    "optional",
    "keys_plain",
    "keys_tls",
    "keys_wrong_password",
    "keys_client_password",
    "keys_server_password",
    "keys_parse",
    "keys_changed_salt"};
  bool expected_failure = std::ranges::find(successful_modes, mode) == successful_modes.end();
  auto result = ctx->run(check(std::move(options), expected_failure));
  return result ? 0 : weave::report_error(result.error());
}
