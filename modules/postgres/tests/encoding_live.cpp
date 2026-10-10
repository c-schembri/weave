#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include "tls_certificates.hpp"
#include <array>
#include <iostream>

namespace pg = weave::pg;

struct EncodingSample {
  pg::Encoding encoding;
  std::string_view bytes;
};

static constexpr std::array samples{
  EncodingSample{pg::Encoding::utf8, "\xc3\xa9\xe4\xb8\xad"},
  EncodingSample{pg::Encoding::latin1, "\xe9"},
  EncodingSample{pg::Encoding::win1252, "\x80"},
  EncodingSample{pg::Encoding::sjis, "\x83\x5c"},
  EncodingSample{pg::Encoding::shift_jis_2004, "\x83\x5c"},
  EncodingSample{pg::Encoding::big5, "\xa5\x5c"},
  EncodingSample{pg::Encoding::gbk, "\x81\x5c"},
  EncodingSample{pg::Encoding::uhc, "\x81\x61"},
  EncodingSample{pg::Encoding::gb18030, "\x90\x30\x81\x30"},
  EncodingSample{pg::Encoding::euc_jp, "\x8e\xa1\xa5\xbd"},
  EncodingSample{pg::Encoding::euc_jis_2004, "\x8e\xa1\xa5\xbd"},
  EncodingSample{pg::Encoding::euc_cn, "\xa1\xa1"},
  EncodingSample{pg::Encoding::euc_kr, "\xa1\xa1"},
  EncodingSample{pg::Encoding::euc_tw, "\x8e\xa2\xa1\xa1"},
  EncodingSample{pg::Encoding::johab, "\xd0\xd0"},
};

static weave::Task<void> roundtrip(pg::Connection &connection, std::string_view text)
{
  auto literal = connection.escape_literal(text);
  std::string name = std::string{text} + "\"\\";
  auto identifier = connection.escape_identifier(name);
  if (!literal || !identifier)
    co_await weave::fail(std::errc::bad_message);
  auto result = co_await connection.query("SELECT " + *literal + " AS " + *identifier);
  if (result.size() != 1 || result.front().rows.size() != 1 || result.front().rows.front().front().bytes() != text ||
    result.front().columns.front().name != name) {
    auto client_encoding = connection.parameter("client_encoding");
    auto server_encoding = connection.parameter("server_encoding");
    if (!client_encoding || !server_encoding)
      co_await weave::fail(std::errc::bad_message);
    WEAVE_LOG_ERROR(
      "Encoding roundtrip mismatch: %s / %s",
      client_encoding->value_or("<absent>").c_str(),
      server_encoding->value_or("<absent>").c_str());
    co_await weave::fail(std::errc::bad_message);
  }
}

static weave::Result<void> blocking_roundtrip(pg::Options options)
{
  auto connection = pg::BlockingConnection::connect(std::move(options));
  if (!connection)
    return std::unexpected(connection.error());

  if (auto set = connection->set_client_encoding(pg::Encoding::latin1); !set)
    return set;
  if (connection->client_encoding() != pg::Encoding::latin1)
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  std::string text = "\xe9'\\; SELECT 99; --";
  auto literal = connection->escape_literal(text);
  if (!literal)
    return std::unexpected(literal.error());
  auto rows = connection->query("SELECT " + *literal);
  if (!rows)
    return std::unexpected(rows.error());
  if (rows->size() != 1 || rows->front().rows.size() != 1 || rows->front().rows.front().front().bytes() != text)
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  return connection->finish();
}

static weave::Task<void> exercise(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  if (connection.client_encoding() != pg::Encoding::utf8)
    co_await weave::fail(std::errc::bad_message);
  auto invalid = co_await weave::as_result(connection.set_client_encoding(static_cast<pg::Encoding>(255)));
  if (invalid || invalid.error() != std::errc::invalid_argument || !connection.open())
    co_await weave::fail(std::errc::bad_message);
  {
    auto deferred = connection.set_client_encoding(pg::Encoding::latin1);
    auto reset = co_await weave::as_result(connection.reset(options));
    if (reset || reset.error() != pg::Error::busy || !connection.open())
      co_await weave::fail(std::errc::bad_message);
  }
  for (weave::u32 index = 0; index < 42; ++index) {
    auto encoding = static_cast<pg::Encoding>(index);
    auto set = co_await weave::as_result(connection.set_client_encoding(encoding));
    if (!set) {
      if (encoding != pg::Encoding::mule_internal || pg::sqlstate(set.error()) != "0A000" || !connection.open()) {
        WEAVE_LOG_ERROR("Encoding SET %u: %s", index, set.error().message().c_str());
        co_await weave::fail(set.error());
      }
      continue;
    }
    if (connection.client_encoding() != encoding)
      co_await weave::fail(std::errc::bad_message);
    co_await roundtrip(connection, "value'\\\"; SELECT 99; --");
  }
  const std::array settings{"on", "off"};
  for (const auto &sample : samples) {
    co_await connection.set_client_encoding(sample.encoding);
    auto info = pg::encoding_info(sample.encoding);
    if (!info)
      co_await weave::fail(info.error());
    auto server_encoding = connection.parameter("server_encoding");
    if (!server_encoding)
      co_await weave::fail(server_encoding.error());
    bool ascii_restricted = *server_encoding == "SQL_ASCII" && !info->server;
    for (auto setting : settings) {
      co_await connection.query(std::string{"SET standard_conforming_strings="} + setting);
      auto text = std::string{sample.bytes} + "'\\\"; SELECT 99; --";
      if (ascii_restricted) {
        auto quoted = connection.escape_literal(text);
        if (!quoted)
          co_await weave::fail(quoted.error());
        auto rejected = co_await weave::as_result(connection.query("SELECT " + *quoted));
        if (rejected || pg::sqlstate(rejected.error()) != "22021" || !connection.open())
          co_await weave::fail(std::errc::bad_message);
      } else
        co_await roundtrip(connection, text);
    }
  }
  co_await connection.set_client_encoding(pg::Encoding::utf8);
  co_await connection.finish();
  auto closed = connection.client_encoding();
  if (closed || closed.error() != pg::Error::closed)
    co_await weave::fail(std::errc::bad_message);
}

int main()
{
  fixture::Certificates certificates;
  std::printf("%s\n%s\n%s\n", certificates.ca.c_str(), certificates.leaf.c_str(), certificates.private_key.c_str());
  std::fflush(stdout);
  std::string port_text, password, standby, address, local;
  std::getline(std::cin, port_text);
  std::getline(std::cin, password);
  std::getline(std::cin, standby);
  std::getline(std::cin, address);
  std::getline(std::cin, local);
  auto port = weave::parse_port(port_text);
  auto ip = weave::IpAddress::parse(address);
  if (!port || !ip)
    return 1;
  auto credentials = weave::TlsContext::client(
    {.ca_file = certificates.ca, .certificate_file = certificates.client, .private_key_file = certificates.client_key});
  if (!credentials)
    return weave::report_error(credentials.error());
  pg::Options plain{
    .host = "localhost",
    .port = *port,
    .user = "weave",
    .database = "postgres",
    .password = password,
    .plaintext = true};
  plain.hosts = {{plain.host, *port, *ip}};
  auto secured = plain;
  secured.plaintext = false;
  secured.tls = *credentials;
  secured.channel_binding = pg::ChannelBinding::require;
  const std::array profiles{plain, secured};
  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());
  const std::array databases{"postgres", "encoding_ascii"};
  for (auto database : databases) {
    for (auto options : profiles) {
      options.database = database;
      if (auto result = ctx->run(weave::timeout(std::chrono::seconds{20}, exercise(options))); !result)
        return weave::report_error(result.error());
      if (auto result = blocking_roundtrip(std::move(options)); !result)
        return weave::report_error(result.error());
    }
  }
  if (!local.empty()) {
    auto options = plain;
    options.host = local;
    options.hosts.clear();
    for (auto database : databases) {
      options.database = database;
      if (auto result = ctx->run(weave::timeout(std::chrono::seconds{20}, exercise(options))); !result)
        return weave::report_error(result.error());
    }
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
#if defined(_WIN32)
  const std::array layouts{weave::IoLayout::sharded, weave::IoLayout::shared};
#else
  const std::array layouts{weave::IoLayout::sharded};
#endif
  for (auto layout : layouts) {
    for (auto scheduler : schedulers) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
      if (!runtime)
        return weave::report_error(runtime.error());

      std::vector<weave::JoinHandle<void>> jobs;
      for (weave::u32 index = 0; index < 16; ++index) {
        auto options = profiles[index % profiles.size()];
        options.database = databases[(index / profiles.size()) % databases.size()];
        auto job = runtime->spawn(weave::timeout(std::chrono::seconds{20}, exercise(std::move(options))));
        if (!job)
          return weave::report_error(job.error());
        jobs.push_back(std::move(*job));
      }
      for (auto &job : jobs) {
        auto result = std::move(job).get();
        if (!result)
          return weave::report_error(result.error());
      }
    }
  }
#endif
  std::puts("Encoding roundtrips, SQL_ASCII rejection, blocking and available four-worker policies passed");
}
