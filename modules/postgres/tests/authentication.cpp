#include <weave/postgres.hpp>
#include <weave/io.hpp>
#include <weave/log.hpp>
#include "tls_certificates.hpp"
#include <cstdio>
#include <iostream>
#include <array>
#include <charconv>

static weave::Task<void> check(weave::pg::Options options, std::string expected, int method, int version)
{
  auto connection = co_await weave::as_result(weave::pg::connect(std::move(options)));
  if (expected != "success") {
    std::error_code error;
    if (expected == "authentication")
      error = weave::pg::Error::authentication;
    else if (expected == "protocol")
      error = weave::pg::Error::protocol;
    else if (expected == "version")
      error = std::make_error_code(std::errc::protocol_not_supported);
    else
      error = weave::pg::Error::unsupported_authentication;
    if (connection || connection.error() != error)
      co_await weave::fail(std::errc::bad_message);
    co_return;
  }
  if (!connection)
    co_await weave::fail(connection.error());
  if (connection->authentication_method() != static_cast<weave::pg::Authentication>(method) ||
    static_cast<int>(connection->protocol_version()) != version)
    co_await weave::fail(std::errc::bad_message);
  auto rows = co_await connection->query("SELECT 42");
  if (rows.size() != 1 || rows[0].rows.size() != 1 || rows[0].rows[0][0].integer<int>() != 42)
    co_await weave::fail(std::errc::bad_message);
  co_await connection->finish();
}

int main(int argc, char **argv)
{
  if (argc != 5)
    return 2;

  int method = 0, version = 0;
  auto method_text = std::string_view{argv[2]};
  auto version_text = std::string_view{argv[3]};
  auto parsed_method = std::from_chars(method_text.data(), method_text.data() + method_text.size(), method);
  auto parsed_version = std::from_chars(version_text.data(), version_text.data() + version_text.size(), version);
  if (parsed_method.ec != std::errc{} || parsed_method.ptr != method_text.data() + method_text.size() ||
    parsed_version.ec != std::errc{} || parsed_version.ptr != version_text.data() + version_text.size())
    return 2;
  fixture::Certificates certificates;
  std::printf("%s\n%s\n%s\n", certificates.ca.c_str(), certificates.leaf.c_str(), certificates.private_key.c_str());
  std::fflush(stdout);
  std::string line;
  std::getline(std::cin, line);
  auto options = weave::pg::Options::parse(line);
  if (!options)
    return weave::report_error(options.error());
  if (!options->plaintext) {
    auto credentials = weave::TlsContext::client({.ca_file = certificates.ca});
    if (!credentials)
      return weave::report_error(credentials.error());
    options->tls = *credentials;
  }
  options->allow_cleartext_password = std::string_view{argv[4]} == "allow";
  options->allow_md5_password = options->allow_cleartext_password;
  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());
  auto result = ctx->run(check(std::move(*options), argv[1], method, version));
  return result ? 0 : weave::report_error(result.error());
}
