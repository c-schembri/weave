#include "authentication_live.hpp"
#include <array>

static constexpr std::array versions{weave::pg::ProtocolVersion::v30, weave::pg::ProtocolVersion::v32};
static constexpr std::array methods{
  weave::pg::Authentication::scram_sha256,
  weave::pg::Authentication::md5,
  weave::pg::Authentication::password};

static weave::pg::Options auth_options(
  weave::pg::Options options,
  weave::pg::ProtocolVersion version,
  weave::pg::Authentication method)
{
  if (method == weave::pg::Authentication::md5)
    options.user = "weave_md5";
  else if (method == weave::pg::Authentication::password)
    options.user = "weave_password";

  bool scram = method == weave::pg::Authentication::scram_sha256;
  options.channel_binding = scram ? weave::pg::ChannelBinding::require : weave::pg::ChannelBinding::prefer;
  options.allow_cleartext_password = method == weave::pg::Authentication::password;
  options.allow_md5_password = method == weave::pg::Authentication::md5;
  options.authentication.methods = {method};
  options.min_protocol = version;
  options.max_protocol = version;
  return options;
}

static weave::Task<void> check_metadata(weave::pg::Options options, weave::pg::Authentication method)
{
  auto connection = co_await weave::pg::connect(options);
  if (connection.protocol_version() != options.max_protocol || connection.authentication_method() != method)
    co_await weave::fail(std::errc::bad_message);

  auto rows = co_await connection.query("SELECT current_user");
  if (rows.size() != 1 || rows[0].rows.size() != 1 || rows[0].rows[0][0].bytes() != options.user)
    co_await weave::fail(std::errc::bad_message);
  co_await connection.finish();
}

weave::Task<void> fixture::authentication(weave::pg::Options options)
{
  for (auto version : versions) {
    for (auto method : methods) {
      auto candidate = auth_options(options, version, method);
      co_await check_metadata(candidate, method);

      auto denied = candidate;
      denied.authentication.methods = {method};
      denied.authentication.exclude = true;
      auto rejected = co_await weave::as_result(weave::pg::connect(std::move(denied)));
      if (rejected || rejected.error() != weave::pg::Error::authentication)
        co_await weave::fail(std::errc::bad_message);

      auto wrong_password = candidate;
      wrong_password.password = "intentionally_wrong_password";
      auto invalid = co_await weave::as_result(weave::pg::connect(std::move(wrong_password)));
      if (invalid ||
        (invalid.error() != weave::pg::Error::authentication && weave::pg::sqlstate(invalid.error()) != "28P01"))
        co_await weave::fail(std::errc::bad_message);

      if (method != weave::pg::Authentication::scram_sha256) {
        candidate.allow_cleartext_password = false;
        candidate.allow_md5_password = false;
        auto disabled = co_await weave::as_result(weave::pg::connect(std::move(candidate)));
        if (disabled || disabled.error() != weave::pg::Error::unsupported_authentication)
          co_await weave::fail(std::errc::bad_message);
      }
    }
  }
}

weave::Result<void> fixture::authentication_blocking(weave::pg::Options options)
{
  for (auto version : versions) {
    for (auto method : methods) {
      auto candidate = auth_options(options, version, method);
      auto connection = weave::pg::BlockingConnection::connect(candidate);
      if (!connection)
        return std::unexpected(connection.error());
      if (connection->protocol_version() != version || connection->authentication_method() != method)
        return std::unexpected(std::make_error_code(std::errc::bad_message));

      auto rows = connection->query("SELECT current_user");
      if (!rows)
        return std::unexpected(rows.error());
      if (rows->size() != 1 || (*rows)[0].rows.size() != 1 || (*rows)[0].rows[0][0].bytes() != candidate.user)
        return std::unexpected(std::make_error_code(std::errc::bad_message));
      if (auto finished = connection->finish(); !finished)
        return finished;
    }
  }
  return {};
}
