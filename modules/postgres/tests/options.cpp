#include <weave/postgres/connection.hpp>
#include <doctest/doctest.h>
#include <array>
#include <algorithm>
#include <memory>

namespace pg = weave::pg;

TEST_CASE("SCRAM keys own fixed-size decoded credentials and clear moved or destroyed storage")
{
  static_assert(sizeof(pg::ScramKey) == 32);
  static_assert(std::is_nothrow_move_constructible_v<pg::ScramKey>);
  static_assert(std::is_nothrow_move_assignable_v<pg::ScramKey>);
  std::array<std::byte, 32> raw;
  raw.fill(std::byte{37});
  pg::ScramKey first{raw};
  auto copy = first;
  auto moved = std::move(first);
  CHECK(std::ranges::equal(copy.bytes(), raw));
  CHECK(std::ranges::equal(moved.bytes(), raw));
  CHECK(std::ranges::all_of(first.bytes(), [](auto value) {
    return value == std::byte{};
  }));

  first = std::move(moved);
  CHECK(std::ranges::equal(first.bytes(), raw));
  CHECK(std::ranges::all_of(moved.bytes(), [](auto value) {
    return value == std::byte{};
  }));
  moved = copy;
  CHECK(std::ranges::equal(moved.bytes(), raw));
  raw.fill(std::byte{});
  CHECK(copy.bytes().front() == std::byte{37});

  alignas(pg::ScramKey) std::array<unsigned char, sizeof(pg::ScramKey)> storage{};
  auto *key = std::construct_at(
    reinterpret_cast<pg::ScramKey *>(storage.data()),
    std::span<const std::byte, 32>{copy.bytes()});
  std::destroy_at(key);
  CHECK(std::ranges::all_of(storage, [](auto value) {
    return value == 0;
  }));
}

TEST_CASE("SCRAM key parsing rejects incorrect size, alphabet and padding")
{
  constexpr std::string_view encoded = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
  auto key = pg::ScramKey::parse(encoded);
  REQUIRE(key);
  for (std::size_t index = 0; index < key->bytes().size(); ++index)
    CHECK(key->bytes()[index] == std::byte(index));

  const std::array invalid{
    std::string{},
    std::string(encoded.substr(0, 43)),
    std::string(encoded) + "=",
    " " + std::string(encoded.substr(1)),
    std::string(encoded.substr(0, 43)) + "A",
    std::string(encoded.substr(0, 12)) + "=" + std::string(encoded.substr(13)),
    std::string(encoded.substr(0, 12)) + '\0' + std::string(encoded.substr(13)),
    std::string(encoded.substr(0, 12)) + '\n' + std::string(encoded.substr(13)),
    std::string(encoded.substr(0, 12)) + '\t' + std::string(encoded.substr(13))};
  for (const auto &text : invalid) {
    auto rejected = pg::ScramKey::parse(text);
    CHECK((!rejected && rejected.error() == std::errc::invalid_argument));
  }

  auto options = pg::Options::parse("scram_client_key=" + std::string(encoded));
  REQUIRE(options);
  REQUIRE(options->scram_client_key);
  CHECK(std::ranges::equal(options->scram_client_key->bytes(), key->bytes()));
  CHECK_FALSE(options->scram_server_key);
  options = pg::Options::parse("postgres://localhost/?scram_server_key=" + std::string(encoded));
  REQUIRE(options);
  REQUIRE(options->scram_server_key);
  CHECK(std::ranges::equal(options->scram_server_key->bytes(), key->bytes()));
  CHECK_FALSE(options->scram_client_key);

  const std::array malformed{"scram_client_key=''", "scram_server_key=invalid"};
  for (auto text : malformed) {
    auto rejected = pg::Options::parse(text);
    CHECK((!rejected && rejected.error() == std::errc::invalid_argument));
  }
}

TEST_CASE("PostgreSQL authentication policies restrict methods without enabling weak credentials")
{
  auto options = pg::Options::parse("require_auth=password,md5,scram-sha-256,md5");
  REQUIRE(options);
  CHECK(
    options->authentication.methods ==
    std::vector{pg::Authentication::password, pg::Authentication::md5, pg::Authentication::scram_sha256});
  CHECK_FALSE(options->authentication.exclude);
  CHECK_FALSE(options->allow_cleartext_password);
  CHECK_FALSE(options->allow_md5_password);

  options = pg::Options::parse("postgres://localhost/postgres?require_auth=!gss,!sspi,!oauth,!none");
  REQUIRE(options);
  CHECK(options->authentication.exclude);
  CHECK(options->authentication.methods.size() == 4);
  options = pg::Options::parse("require_auth=''");
  REQUIRE(options);
  CHECK(options->authentication.methods.empty());

  const std::array invalid_fields{
    "require_auth=!",
    "require_auth=none,!md5",
    "require_auth=!none,md5",
    "require_auth=unknown",
    "require_auth=none,",
    "require_auth=',none'",
    "require_auth=SCRAM-SHA-256",
    "require_auth='none, md5'"};
  for (auto input : invalid_fields) {
    auto parsed = pg::Options::parse(input);
    CHECK((!parsed && parsed.error() == std::errc::invalid_argument));
  }
  auto oauth = pg::Options::parse("require_auth=oauth");
  REQUIRE(oauth);
  CHECK(oauth->authentication.methods == std::vector{pg::Authentication::oauth});
  CHECK_FALSE(oauth->oauth);
  const std::array native{"require_auth=gss", "require_auth=sspi", "require_auth=gss,none"};
  for (auto input : native) {
    auto parsed = pg::Options::parse(input);
#if defined(_WIN32) || defined(WEAVE_POSTGRES_TEST_GSSAPI)
    REQUIRE(parsed);
    CHECK_FALSE(parsed->gss);
#else
    CHECK((!parsed && parsed.error() == std::errc::operation_not_supported));
#endif
  }

  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  options->user = "test";
  options->host = "not a host";
  options->authentication.methods = {pg::Authentication::gss};
  auto unsupported = ctx->run(pg::connect(*options));
  CHECK((!unsupported && unsupported.error() == std::errc::operation_not_supported));
  CHECK(ctx->metrics().submitted == 0);
  options->authentication.methods = {static_cast<pg::Authentication>(-1)};
  auto invalid = ctx->run(pg::connect(*options));
  CHECK((!invalid && invalid.error() == std::errc::invalid_argument));
  CHECK(ctx->metrics().submitted == 0);
  options->authentication.methods.clear();
  options->authentication.exclude = true;
  auto empty = ctx->run(pg::connect(*options));
  CHECK((!empty && empty.error() == std::errc::invalid_argument));
  CHECK(ctx->metrics().submitted == 0);
}

TEST_CASE("PostgreSQL GSS options describe policy without creating provider workers")
{
  auto options = pg::Options::parse("krbsrvname=custom_service gssdelegation=1");
  REQUIRE(options);
  CHECK(options->gss_service == "custom_service");
  CHECK(options->gss_delegation);
  CHECK(options->gss_mutual);
  CHECK_FALSE(options->gss);
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  options->user = "client";
  auto missing = ctx->run(pg::connect(*options));
  CHECK((!missing && missing.error() == std::errc::invalid_argument));
  CHECK(ctx->metrics().submitted == 0);

  const std::array invalid{
    "krbsrvname=service/host",
    "krbsrvname=service@realm",
    "gssdelegation=true",
    "gsslib=unknown"};
  for (auto input : invalid) {
    auto rejected = pg::Options::parse(input);
    CHECK((!rejected && rejected.error() == std::errc::invalid_argument));
  }
#if defined(_WIN32)
  CHECK(pg::Options::parse("gsslib=sspi"));
  auto other = pg::Options::parse("gsslib=gssapi");
#else
  CHECK(pg::Options::parse("gsslib=gssapi"));
  auto other = pg::Options::parse("gsslib=sspi");
#endif
  CHECK((!other && other.error() == std::errc::operation_not_supported));
}

TEST_CASE("PostgreSQL protocol bounds are validated before transport setup")
{
  auto options = pg::Options::parse("min_protocol_version=3.2 max_protocol_version=latest");
  REQUIRE(options);
  CHECK(options->min_protocol == pg::ProtocolVersion::v32);
  CHECK(options->max_protocol == pg::ProtocolVersion::v32);
  options = pg::Options::parse("min_protocol_version=3.0 max_protocol_version=3.0");
  REQUIRE(options);
  CHECK(options->min_protocol == pg::ProtocolVersion::v30);
  CHECK(options->max_protocol == pg::ProtocolVersion::v30);
  options = pg::Options::parse("min_protocol_version='' max_protocol_version=''");
  REQUIRE(options);
  CHECK(options->min_protocol == pg::ProtocolVersion::v30);
  CHECK(options->max_protocol == pg::ProtocolVersion::v32);

  const std::array invalid_fields{
    "min_protocol_version=3.1",
    "max_protocol_version=3",
    "min_protocol_version=3.2 max_protocol_version=3.0",
    "min_protocol_version=4.0",
    "max_protocol_version=3.2junk"};
  for (auto input : invalid_fields) {
    auto parsed = pg::Options::parse(input);
    CHECK((!parsed && parsed.error() == std::errc::invalid_argument));
  }
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  options->user = "test";
  options->host = "not a host";
  options->min_protocol = static_cast<pg::ProtocolVersion>(196609);
  auto invalid = ctx->run(pg::connect(*options));
  CHECK((!invalid && invalid.error() == std::errc::invalid_argument));
  CHECK(ctx->metrics().submitted == 0);
  options->min_protocol = pg::ProtocolVersion::v32;
  options->max_protocol = pg::ProtocolVersion::v30;
  auto reversed = ctx->run(pg::connect(*options));
  CHECK((!reversed && reversed.error() == std::errc::invalid_argument));
  CHECK(ctx->metrics().submitted == 0);
}

TEST_CASE("PostgreSQL connection strings own decoded values without loading configuration")
{
  auto options = pg::Options::parse(
    "postgresql://a%20b:p%2B%3A%40@[::1]:5544,localhost/db%2Fname?application_name=x+y&sslmode=disable");
  REQUIRE(options);
  CHECK(options->user == "a b");
  CHECK(options->password == "p+:@");
  CHECK(options->database == "db/name");
  CHECK(options->application_name == "x+y");
  REQUIRE(options->hosts.size() == 2);
  CHECK(options->hosts[0].name == "::1");
  CHECK(options->hosts[0].port == 5544);
  CHECK(options->hosts[1].name == "localhost");
  CHECK(options->hosts[1].port == 5432);
  CHECK(options->plaintext);

  options = pg::Options::parse(
    "user = 'a b' password='a\\'b\\\\c' host=one,two port=5544 application_name=old application_name=new");
  REQUIRE(options);
  CHECK(options->user == "a b");
  CHECK(options->password == "a'b\\c");
  CHECK(options->application_name == "new");
  REQUIRE(options->hosts.size() == 2);
  CHECK(options->hosts[1].port == 5544);

  options = pg::Options::parse("postgres://[a:1:2:3:4:5:6:7]:5432/db");
  REQUIRE(options);
  CHECK(options->hosts.front().name == "a:1:2:3:4:5:6:7");

  options = pg::Options::parse(
    "postgres://one/db?host=localhost&hostaddr=127.0.0.1&sslrootcert=not-a-file&ssl_min_protocol_version=TLSv1.3");
  REQUIRE(options);
  REQUIRE(options->hosts.size() == 1);
  CHECK(options->hosts.front().name == "localhost");
  REQUIRE(options->hosts.front().address);
  CHECK(options->hosts.front().address->to_string() == "127.0.0.1");
  REQUIRE(options->tls_options);
  CHECK(options->tls_options->ca_file == "not-a-file");
  CHECK(options->tls_options->min_version == weave::TlsVersion::tls13);
  CHECK_FALSE(options->tls);
  CHECK_FALSE(options->plaintext);

  std::string temporary = "user=owned password=secret";
  options = pg::Options::parse(temporary);
  temporary.assign(temporary.size(), 'x');
  REQUIRE(options);
  CHECK(options->user == "owned");
  CHECK(options->password == "secret");
}

TEST_CASE("PostgreSQL options convert typed defaults, host lists and secure policies")
{
  auto options = pg::Options::parse("");
  REQUIRE(options);
  CHECK(options->host == "localhost");
  CHECK(options->hosts.empty());
  CHECK(options->user.empty());
  CHECK(options->connect_timeout == std::chrono::seconds{30});
  CHECK(options->host_balance == pg::HostBalance::ordered);
  CHECK_FALSE(options->plaintext);

  options = pg::Options::parse("hostaddr=::1,127.0.0.1 port=,5544");
  REQUIRE(options);
  REQUIRE(options->hosts.size() == 2);
  CHECK(options->hosts[0].name == "::1");
  CHECK(options->hosts[0].port == 5432);
  CHECK(options->hosts[1].port == 5544);

  options = pg::Options::parse("host=,localhost hostaddr=127.0.0.1, port=5544");
  REQUIRE(options);
  CHECK(options->hosts[0].name == "127.0.0.1");
  CHECK_FALSE(options->hosts[1].address);
  options = pg::Options::parse("postgres://,localhost:5544/db");
  REQUIRE(options);
  CHECK(options->hosts[0].name == "localhost");
  CHECK(options->hosts[1].port == 5544);

  options = pg::Options::parse(
    "fallback_application_name=fallback application_name='' client_encoding=LATIN1 options='-c search_path=pg_catalog' "
    "connect_timeout=86400 channel_binding=require target_session_attrs=prefer-standby gssencmode=disable "
    "load_balance_hosts=disable sslmode=verify-full sslrootcert=system sslcert=certificate.pem sslkey=key.pem "
    "sslpassword='key password' sslcrl=crl.pem ssl_min_protocol_version=TLSv1.2 ssl_max_protocol_version=TLSv1.3");
  REQUIRE(options);
  CHECK(options->application_name == "fallback");
  CHECK(options->client_encoding == "LATIN1");
  CHECK(options->server_options == "-c search_path=pg_catalog");
  CHECK(options->connect_timeout == std::chrono::hours{24});
  CHECK(options->channel_binding == pg::ChannelBinding::require);
  CHECK(options->target_session == pg::TargetSession::prefer_standby);
  REQUIRE(options->tls_options);
  CHECK(options->tls_options->ca_file.empty());
  CHECK(options->tls_options->certificate_file == "certificate.pem");
  CHECK(options->tls_options->private_key_file == "key.pem");
  CHECK(options->tls_options->private_key_password == "key password");
  CHECK(options->tls_options->revocation == weave::TlsRevocation::chain);
}

TEST_CASE("PostgreSQL CRL directories parse without I/O and appear in nonsecret snapshots")
{
  const std::array inputs{
    "sslcrldir='directory with spaces' sslcrl=other.crl",
    "postgres://localhost?sslcrldir=directory%20with%20spaces&sslcrl=other.crl"};
  for (const auto *input : inputs) {
    auto options = pg::Options::parse(input);
    REQUIRE(options);
    if (!options)
      return;
    REQUIRE(options->tls_options);
    if (!options->tls_options)
      return;

    CHECK(options->tls_options->crl_directory == "directory with spaces");
    CHECK(options->tls_options->crl_file == "other.crl");
    CHECK(options->tls_options->revocation == weave::TlsRevocation::chain);
    auto info = options->info();
    REQUIRE(info.tls_options);
    if (!info.tls_options)
      return;
    CHECK(info.tls_options->crl_directory == "directory with spaces");
    CHECK(info.tls_options->crl_file == "other.crl");
  }
}

TEST_CASE("PostgreSQL parsing rejects malformed input and unsupported compatibility without downgrade")
{
  const std::array malformed{
    "host=one,two hostaddr=127.0.0.1",
    "host=one,two port=1,2,3",
    "port=1,2",
    "port=0",
    "port=65536",
    "port=-1",
    "user='unclosed",
    "password=trailing\\",
    "user='a'b",
    "user",
    "postgres://[::1]x",
    "postgres://::1",
    "postgres://[127.0.0.1]",
    "postgres://x/db#fragment",
    "postgres://x?user=%00",
    "postgres://x?user=%q1",
    "postgres://x?user=%",
    "postgres://x?user=one&",
    "postgres://x?user",
    "postgres://a@b@c",
    "postgres://x/a/b",
    "postgres://x?user=a b",
    "unknown=x",
    "connect_timeout=0",
    "connect_timeout=86401",
    "sslmode=garbage",
    "channel_binding=garbage",
    "target_session_attrs=garbage",
    "load_balance_hosts=garbage",
    "replication=garbage",
    "sslmode=disable channel_binding=require",
    "sslmode=disable sslrootcert=x",
    "ssl_min_protocol_version=TLSv1.3 ssl_max_protocol_version=TLSv1.2"};
  for (auto input : malformed) {
    auto result = pg::Options::parse(input);
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::invalid_argument);
  }

  const std::array unsupported{
    "requirepeer=someone",
    "service=x",
    "passfile=x",
    "client_encoding=auto",
    "ssl_min_protocol_version=TLSv1.1"};
  for (auto input : unsupported) {
    auto result = pg::Options::parse(input);
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::operation_not_supported);
  }

  CHECK_FALSE(pg::Options::parse(std::string{"user=one\0password=two", 21}));
  CHECK_FALSE(pg::Options::parse(std::string(1024 * 1024 + 1, ' ')));
  CHECK(pg::Options::parse("password=" + std::string(65536, 'a')));
  CHECK_FALSE(pg::Options::parse("password=" + std::string(65537, 'a')));

  std::string hosts = "host=localhost";
  for (unsigned index = 1; index < 64; ++index)
    hosts += ",localhost";
  CHECK(pg::Options::parse(hosts));
  CHECK_FALSE(pg::Options::parse(hosts + ",localhost"));
}

TEST_CASE("PostgreSQL GSS encryption parsing is explicit and independent of provider creation")
{
  const std::array modes{
    std::pair{"disable", pg::GssEncryption::disable},
    std::pair{"prefer", pg::GssEncryption::prefer},
    std::pair{"require", pg::GssEncryption::require}};
  for (const auto &[name, mode] : modes) {
    auto options = pg::Options::parse(std::string{"gssencmode="} + name);
    REQUIRE(options);
    CHECK(options->gss_encryption == mode);
    CHECK_FALSE(options->gss);
  }
  auto invalid = pg::Options::parse("gssencmode=unknown");
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error() == std::errc::invalid_argument);
}

TEST_CASE("PostgreSQL local host parsing owns explicit directories and validates complete native paths")
{
  const std::array filesystem{
    "host=/tmp sslmode=disable",
    "postgres://%2Ftmp/postgres?sslmode=disable",
    "postgres://localhost/postgres?host=%2Ftmp&sslmode=disable"};
  for (auto input : filesystem) {
    auto options = pg::Options::parse(input);
    REQUIRE(options);
    CHECK(options->host == "/tmp");
    CHECK(options->plaintext);
    REQUIRE(options->hosts.size() == 1);
    CHECK(options->hosts.front().name == "/tmp");
    CHECK_FALSE(options->hosts.front().address);
  }
  auto trailing = pg::Options::parse("host=/tmp/ port=5544 sslmode=disable");
  REQUIRE(trailing);
  CHECK(trailing->host == "/tmp/");
  CHECK(trailing->port == 5544);
  auto ipv6 = pg::Options::parse("postgres://[a::1]/postgres");
  REQUIRE(ipv6);
  CHECK(ipv6->host == "a::1");

  const std::array invalid{
    "host=@",
    "host=/tmp hostaddr=127.0.0.1",
    "host=C:relative",
    "postgres://x/postgres?host=%2Ftmp%00hidden"};
  for (auto input : invalid) {
    auto options = pg::Options::parse(input);
    CHECK((!options && options.error() == std::errc::invalid_argument));
  }
  constexpr std::size_t suffix = std::string_view{"/.s.PGSQL.5432"}.size();
  std::string maximum = "/" + std::string(106 - suffix, 'x');
  CHECK(pg::Options::parse("host=" + maximum));
  auto excessive = pg::Options::parse("host=" + maximum + "x");
  CHECK((!excessive && excessive.error() == std::errc::filename_too_long));

  auto abstract = pg::Options::parse("postgres://%40weave-private/postgres?sslmode=disable");
  auto drive = pg::Options::parse("postgres://x/postgres?host=C%3A%2Fpg&sslmode=disable");
#if defined(_WIN32)
  CHECK((!abstract && abstract.error() == std::errc::operation_not_supported));
  REQUIRE(drive);
  CHECK(drive->host == "C:/pg");
#else
  REQUIRE(abstract);
  CHECK(abstract->host == "@weave-private");
  CHECK((!drive && drive.error() == std::errc::operation_not_supported));
#endif
}

TEST_CASE("PostgreSQL host balancing is explicit, typed and does not reorder parsed destinations")
{
  auto options = pg::Options::parse("postgres://one,two,one/database?load_balance_hosts=random");
  REQUIRE(options);
  if (!options)
    return;

  CHECK(options->host_balance == pg::HostBalance::random);
  REQUIRE(options->hosts.size() == 3);
  if (options->hosts.size() != 3)
    return;
  CHECK(options->hosts[0].name == "one");
  CHECK(options->hosts[1].name == "two");
  CHECK(options->hosts[2].name == "one");

  const std::array ordered{
    "load_balance_hosts=disable",
    "load_balance_hosts=''",
    "load_balance_hosts=random load_balance_hosts=disable"};
  for (auto text : ordered) {
    auto parsed = pg::Options::parse(text);
    REQUIRE(parsed);
    if (parsed)
      CHECK(parsed->host_balance == pg::HostBalance::ordered);
  }
}

TEST_CASE("PostgreSQL replication startup settings are explicit and typed")
{
  const std::array enabled{"true", "on", "yes", "1", "TRUE", "On", "YeS"};
  const std::array disabled{"false", "off", "no", "0", "FALSE", "Off", "nO"};
  for (auto value : enabled) {
    auto options = pg::Options::parse(std::string{"replication="} + value);
    REQUIRE(options);
    CHECK(options->replication == pg::Replication::physical);
  }
  for (auto value : disabled) {
    auto options = pg::Options::parse(std::string{"replication="} + value);
    REQUIRE(options);
    CHECK(options->replication == pg::Replication::disabled);
  }
  auto options = pg::Options::parse("postgres://localhost/postgres?replication=DaTaBaSe");
  REQUIRE(options);
  CHECK(options->replication == pg::Replication::database);

  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  options->user = "test";
  options->replication = static_cast<pg::Replication>(-1);
  auto invalid = ctx->run(pg::connect(*options));
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error() == std::errc::invalid_argument);

  options->replication = pg::Replication::physical;
  options->target_session = pg::TargetSession::primary;
  auto target = ctx->run(pg::connect(*options));
  REQUIRE_FALSE(target);
  CHECK(target.error() == std::errc::invalid_argument);
}

TEST_CASE("PostgreSQL direct TLS options require verified transport and retain their policy")
{
  const std::array legacy_inputs{"user=test", "user=test sslnegotiation=postgres", "user=test sslnegotiation=''"};
  for (auto input : legacy_inputs) {
    auto options = pg::Options::parse(input);
    REQUIRE(options);
    if (!options)
      return;
    CHECK(options->tls_negotiation == pg::TlsNegotiation::postgres);
    CHECK(options->info().tls_negotiation == pg::TlsNegotiation::postgres);
  }

  const std::array direct_inputs{
    "user=test sslnegotiation=direct",
    "user=test sslmode=verify-full sslnegotiation=direct",
    "postgresql://localhost/postgres?user=test&sslnegotiation=direct"};
  for (auto input : direct_inputs) {
    auto options = pg::Options::parse(input);
    REQUIRE(options);
    if (!options)
      return;
    CHECK_FALSE(options->plaintext);
    CHECK(options->tls_negotiation == pg::TlsNegotiation::direct);
    CHECK(options->info().tls_negotiation == pg::TlsNegotiation::direct);
  }

  const std::array invalid_inputs{
    "sslnegotiation=unknown",
    "sslnegotiation=DIRECT",
    "sslmode=disable sslnegotiation=direct"};
  for (auto input : invalid_inputs) {
    auto options = pg::Options::parse(input);
    CHECK((!options && options.error() == std::errc::invalid_argument));
  }

  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  if (!ctx)
    return;
  pg::Options options;
  options.user = "test";
  options.plaintext = true;
  options.tls_negotiation = pg::TlsNegotiation::direct;
  auto invalid = ctx->run(pg::connect(options));
  CHECK((!invalid && invalid.error() == std::errc::invalid_argument));
  CHECK(ctx->metrics().submitted == 0);
}

TEST_CASE("PostgreSQL keepalive options are typed and bounded without native setup during parsing")
{
  using namespace std::chrono_literals;
  auto options = pg::Options::parse(
    "keepalives=2 keepalives_idle=60 keepalives_interval=5 keepalives_count=3 tcp_user_timeout=0");
  REQUIRE(options);
  if (!options)
    return;
  CHECK(options->keep_alive.enabled);
  CHECK(options->keep_alive.idle == 60s);
  CHECK(options->keep_alive.interval == 5s);
  CHECK(options->keep_alive.probes == 3);
  CHECK(options->tcp_user_timeout == 0ms);

  options = pg::Options::parse("keepalives=0 keepalives_idle=2147483647 keepalives_count=2147483647");
  REQUIRE(options);
  if (!options)
    return;
  CHECK_FALSE(options->keep_alive.enabled);
  CHECK(options->keep_alive.idle.count() == 2147483647);
  CHECK(options->keep_alive.probes == 2147483647);
  options = pg::Options::parse(
    "keepalives='' keepalives_idle='' keepalives_interval='' keepalives_count='' tcp_user_timeout=''");
  REQUIRE(options);
  if (!options)
    return;
  CHECK(options->keep_alive.enabled);
  CHECK(options->keep_alive.idle == 0s);
  CHECK(options->keep_alive.interval == 0s);
  CHECK(options->keep_alive.probes == 0);

  const std::array invalid_fields{
    "keepalives=-1",
    "keepalives=word",
    "keepalives_idle=-1",
    "keepalives_interval=-1",
    "keepalives_count=-1",
    "keepalives_idle=2147483648",
    "keepalives_interval=1.5",
    "keepalives_count=2147483648",
    "tcp_user_timeout=-1",
    "tcp_user_timeout=2147483648"};
  for (auto input : invalid_fields) {
    auto parsed = pg::Options::parse(input);
    CHECK((!parsed && parsed.error() == std::errc::invalid_argument));
  }

  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  if (!ctx)
    return;
  options->user = "test";
  options->host = "not a host";
  options->keep_alive.idle = -1s;
  auto invalid = ctx->run(pg::connect(*options));
  CHECK((!invalid && invalid.error() == std::errc::invalid_argument));
  CHECK(ctx->metrics().submitted == 0);

  auto timeout = pg::Options::parse("tcp_user_timeout=1234");
#if defined(_WIN32)
  CHECK((!timeout && timeout.error() == std::errc::operation_not_supported));
  options->keep_alive.idle = 0s;
  options->tcp_user_timeout = 1234ms;
  auto unsupported = ctx->run(pg::connect(*options));
  CHECK((!unsupported && unsupported.error() == std::errc::operation_not_supported));
  CHECK(ctx->metrics().submitted == 0);
#else
  REQUIRE(timeout);
  if (timeout)
    CHECK(timeout->tcp_user_timeout == 1234ms);
#endif
}

TEST_CASE("PostgreSQL TLS modes are explicit, with verified defaults and redacted key-log configuration")
{
  const std::array modes{
    std::pair{"disable", pg::TlsMode::disable},
    std::pair{"allow", pg::TlsMode::allow},
    std::pair{"prefer", pg::TlsMode::prefer},
    std::pair{"require", pg::TlsMode::require},
    std::pair{"verify-ca", pg::TlsMode::verify_ca},
    std::pair{"verify-full", pg::TlsMode::verify_full}};
  for (const auto &[text, mode] : modes) {
    auto options = pg::Options::parse("sslmode=" + std::string{text});
    REQUIRE(options);
    CHECK(options->tls_mode == mode);
    CHECK(options->info().tls_mode == mode);
    CHECK(options->plaintext == (mode == pg::TlsMode::disable));
    pg::Options typed;
    typed.tls_mode = mode;
    auto snapshot = typed.info();
    CHECK(snapshot.plaintext == (mode == pg::TlsMode::disable));
    CHECK(snapshot.tls_options.has_value() == (mode != pg::TlsMode::disable));
    if (snapshot.tls_options) {
      const auto verification = mode == pg::TlsMode::verify_full ? weave::TlsVerification::hostname
        : mode == pg::TlsMode::verify_ca                         ? weave::TlsVerification::certificate
                                                                 : weave::TlsVerification::none;
      CHECK(snapshot.tls_options->verification == verification);
    }
  }
  pg::Options legacy;
  legacy.plaintext = true;
  CHECK(legacy.info().tls_mode == pg::TlsMode::disable);
  CHECK_FALSE(legacy.info().tls_options);
  auto defaults = pg::Options::parse("");
  REQUIRE(defaults);
  CHECK(defaults->tls_mode == pg::TlsMode::verify_full);
  auto logged = pg::Options::parse("sslkeylogfile='sensitive path.keys'");
  REQUIRE(logged);
  REQUIRE(logged->tls_options);
  CHECK(logged->tls_options->key_log_file == "sensitive path.keys");
  REQUIRE(logged->info().tls_options);
  CHECK(logged->info().tls_options->key_logging);
  auto required = pg::Options::parse("sslmode=require sslrootcert=private-ca.pem");
  REQUIRE(required);
  REQUIRE(required->tls_options);
  CHECK(required->tls_options->verification == weave::TlsVerification::certificate);
  CHECK_FALSE(pg::Options::parse("sslmode=require sslrootcert=system"));
  CHECK_FALSE(pg::Options::parse("sslmode=prefer sslnegotiation=direct"));
  CHECK_FALSE(pg::Options::parse("sslmode=allow sslnegotiation=direct"));
}
