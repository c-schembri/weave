#include "oauth_http.hpp"
#include "credential_allocations.hpp"
#include "tls_certificates.hpp"
#include <weave/postgres.hpp>
#include <weave/io.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/tls.hpp>
#include <cstdio>
#include <atomic>
#include <string>

namespace wire = weave::pg::detail;
namespace pg = weave::pg;
using namespace std::chrono_literals;

static std::atomic<unsigned> checks = 0;

static void check(bool condition, const char *what)
{
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "OAuth HTTPS failure: %s\n", what);
    std::_Exit(1);
  }
}

static std::span<const std::byte> bytes(std::string_view value)
{
  return std::as_bytes(std::span{value.data(), value.size()});
}

static void urls()
{
  struct Good {
    const char *url;
    const char *host;
    const char *authority;
    const char *target;
    weave::u16 port;
  };

  const std::array good{
    Good{"https://localhost", "localhost", "localhost", "/", 443},
    Good{"https://localhost/", "localhost", "localhost", "/", 443},
    Good{
      "https://LOCALHOST:00443/a%2Fb/../c?x=%0D%0A&y=z",
      "LOCALHOST",
      "LOCALHOST:00443",
      "/a%2Fb/../c?x=%0D%0A&y=z",
      443},
    Good{"https://[::1]:8443//a/?", "::1", "[::1]:8443", "//a/?", 8443},
    Good{"https://127.0.0.1?x=1", "127.0.0.1", "127.0.0.1", "/?x=1", 443},
    Good{"https://example.test///", "example.test", "example.test", "///", 443}};
  for (const auto &entry : good) {
    auto value = wire::oauth_https_url(entry.url);
    check(bool(value), entry.url);
    check(
      value->host == entry.host && value->authority == entry.authority && value->target == entry.target &&
        value->port == entry.port,
      entry.url);
  }
  const std::array hostile{
    "",
    "http://localhost/",
    "HTTPS://localhost/",
    "https://",
    "https://host:",
    "https://host:0/",
    "https://host:65536/",
    "https://host:-1/",
    "https://host:123a/",
    "https://user@host/",
    "https://@host/",
    "https://host/#",
    "https://host/#fragment",
    "https://[v1.a]/",
    "https://[::1%25lo]/",
    "https://[::1",
    "https://host/\r\nHost:evil",
    "https://host/a b",
    "https://host/%",
    "https://host/%GG",
    "https://ho%73t/",
    "https://127.1/",
    "https://2130706433/",
    "https://0177.0.0.1/",
    "https://host\\evil/",
    "https://a..b/",
    "https://-host/",
    "https://host-/",
    "https://.host/"};
  for (auto url : hostile)
    check(!wire::oauth_https_url(url), url);
  std::string large(8193, 'a');
  check(!wire::oauth_https_url(large), "URI size bound");
  std::string nul{"https://host/a\0b", 16};
  check(!wire::oauth_https_url(nul), "URI embedded NUL");
}

struct Case {
  std::string response;
  bool success;
  std::error_code error;
  std::size_t fragment = 0;
  bool close_notify = false;
  bool stall = false;
  bool post = false;
  weave::u16 status = 200;
  std::string body = "{}";
};

static weave::Task<void> peer(
  weave::TcpListener &listener,
  weave::TlsContext credentials,
  Case &test,
  std::string &observed)
{
  auto raw = co_await listener.accept({.no_delay = true});
  auto stream = co_await weave::tls::server(std::move(raw), credentials);
  std::array<std::byte, 2048> buffer;
  std::size_t expected = 0;
  for (;;) {
    auto size = co_await stream.read(buffer);
    if (!size)
      co_await weave::fail(std::errc::bad_message);
    observed.append(reinterpret_cast<const char *>(buffer.data()), size);
    auto header_end = observed.find("\r\n\r\n");
    if (header_end != std::string::npos) {
      expected = header_end + 4 + (test.post ? std::string_view{"client_id=probe&scope=a%20b"}.size() : 0);
      if (observed.size() >= expected)
        break;
    }
  }
  if (test.stall) {
    co_await weave::sleep_for(2s);
    co_return;
  }
  std::size_t offset = 0;
  while (offset < test.response.size()) {
    auto amount = test.fragment ? std::min(test.fragment, test.response.size() - offset) : test.response.size();
    co_await stream.write_all(bytes(std::string_view{test.response}.substr(offset, amount)));
    offset += amount;
  }
  if (test.close_notify)
    co_await stream.shutdown_send();
}

static weave::Task<void> scenario(weave::TlsContext client, weave::TlsContext server, Case &test)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
  auto port = listener.local_port();
  wire::OAuthHttpRequest request;
  request.url = "https://127.0.0.1:" + std::to_string(port) + "/tenant/token?mode=device";
  request.post = test.post;
  if (request.post) {
    constexpr std::string_view form = "client_id=probe&scope=a%20b";
    request.body.assign(form.begin(), form.end());
    constexpr std::string_view basic = "Basic cHJvYmU6c2VjcmV0";
    request.authorization.assign(basic.begin(), basic.end());
  }
  std::unique_ptr<fixture::CredentialAllocation> body_storage;
  std::unique_ptr<fixture::CredentialAllocation> auth_storage;
  if (request.post) {
    body_storage = std::make_unique<fixture::CredentialAllocation>(
      std::string_view{request.body.data(), request.body.size()});
    auth_storage = std::make_unique<fixture::CredentialAllocation>(
      std::string_view{request.authorization.data(), request.authorization.size()});
  }
  std::string observed;
  auto acquire = [&]() -> weave::Task<void> {
    auto result = co_await weave::as_result(wire::oauth_https(std::move(request), client, test.stall ? 1s : 5s));
    if (test.success) {
      if (!result) {
        std::fprintf(stderr, "unexpected HTTP failure: %s\n", result.error().message().c_str());
        check(false, test.response.c_str());
      }
      check(result->status == test.status, "HTTP status");
      check(std::string_view{result->body.data(), result->body.size()} == test.body, "HTTP body");
    } else {
      check(!result, test.response.c_str());
      if (test.error)
        check(result.error() == test.error, "Expected HTTP error category");
    }
  };
  auto serve = [&]() -> weave::Task<void> {
    static_cast<void>(co_await weave::as_result(peer(listener, server, test, observed)));
  };
  co_await weave::when_all(serve(), acquire());
  if (test.post) {
    check(body_storage->cleansed(), "Sent form storage cleansed");
    check(auth_storage->cleansed(), "Sent authorization storage cleansed");
  }
  auto verb = test.post ? "POST" : "GET";
  std::string expected = std::string{verb} +
    " /tenant/token?mode=device HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
    "\r\nAccept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
  if (test.post)
    expected += "Authorization: Basic cHJvYmU6c2VjcmV0\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                "Content-Length: 27\r\n";
  expected += "\r\n";
  if (test.post)
    expected += "client_id=probe&scope=a%20b";
  check(observed == expected, "Exact request bytes");
}

static weave::Task<void> pending(weave::TlsContext client, weave::TlsContext server, std::shared_ptr<bool> acquired)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
  auto peer_task = [&]() -> weave::Task<void> {
    auto raw = co_await listener.accept();
    auto stream = co_await weave::tls::server(std::move(raw), server);
    std::array<std::byte, 2048> buffer;
    static_cast<void>(co_await stream.read(buffer));
    *acquired = true;
    co_await weave::sleep_for(1h);
  };
  auto request_task = [&]() -> weave::Task<void> {
    wire::OAuthHttpRequest request;
    request.url = "https://127.0.0.1:" + std::to_string(listener.local_port()) + "/pending";
    static_cast<void>(co_await wire::oauth_https(std::move(request), client, 1h));
  };
  co_await weave::when_all(peer_task(), request_task());
}

static weave::Task<void> owned_case(weave::TlsContext client, weave::TlsContext server, Case test)
{
  co_await weave::timeout(10s, scenario(client, server, test));
}

static weave::Task<void> rejected_transport(
  weave::TlsContext client,
  weave::TlsContext server,
  std::error_code expected)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
  std::size_t application_bytes = 0;
  auto serve = [&]() -> weave::Task<void> {
    auto raw = co_await listener.accept();
    auto handshake = co_await weave::as_result(weave::tls::server(std::move(raw), server));
    if (handshake) {
      std::array<std::byte, 2048> buffer;
      auto read = co_await weave::as_result(handshake->read(buffer));
      if (read)
        application_bytes = *read;
    }
  };
  auto acquire = [&]() -> weave::Task<void> {
    wire::OAuthHttpRequest request;
    request.url = "https://127.0.0.1:" + std::to_string(listener.local_port()) + "/";
    auto result = co_await weave::as_result(wire::oauth_https(std::move(request), client, 5s));
    check(!result && result.error() == expected, "TLS/ALPN policy rejection");
  };
  co_await weave::when_all(serve(), acquire());
  check(application_bytes == 0, "No HTTP request before TLS/ALPN validation");
}

static std::vector<Case> protocol_cases()
{
  const std::string headers = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
  std::vector<Case> cases{
    {headers + "Content-Length: 2\r\n\r\n{}", true},
    {headers + "Content-Length: 2\r\n\r\n{}", true, {}, 1},
    {headers + "Content-Length: 2\r\n\r\n{}", true, {}, 7, false, false, true},
    {headers + "Transfer-Encoding: chunked\r\n\r\n1\r\n{\r\n1\r\n}\r\n0\r\n\r\n", true, {}, 1},
    {headers + "\r\n{}", true, {}, 0, true},
    {"HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n" + headers + "Content-Length: 2\r\n\r\n{}", true, {}, 1},
    {headers + "Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}", false, pg::Error::protocol},
    {headers + "Content-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n",
      false,
      pg::Error::protocol},
    {headers + "Content-Length: 2\r\n\r\n{}evil", false, pg::Error::protocol},
    {headers + "Content-Length: 3\r\n\r\n{}", false, pg::Error::protocol, 0, true},
    {headers + "\r\n{}", false, weave::TlsError::truncated},
    {headers + "Transfer-Encoding: chunked\r\n\r\nZ\r\n{}\r\n0\r\n\r\n", false, pg::Error::protocol},
    {headers + "Transfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\nX-Test: a\r\n\r\n", false, pg::Error::protocol},
    {headers + "Content-Encoding: gzip\r\nContent-Length: 2\r\n\r\n{}", false, pg::Error::protocol},
    {headers + "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n{}", false, pg::Error::protocol},
    {"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 2\r\n\r\n{}", false, pg::Error::protocol},
    {"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}", false, pg::Error::protocol},
    {"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n", false, pg::Error::protocol},
    {headers + "Content-Length: 65537\r\n\r\n", false, pg::Error::resource_limit},
    {headers + "X-Large: " + std::string(8193, 'a') + "\r\nContent-Length: 2\r\n\r\n{}",
      false,
      pg::Error::resource_limit},
    {headers + "Transfer-Encoding: chunked\r\n\r\n10001\r\n" + std::string(65537, 'a') + "\r\n0\r\n\r\n",
      false,
      pg::Error::resource_limit},
    {headers + "Content-Length: 2\r\n\r\n{}", false, std::make_error_code(std::errc::timed_out), 0, false, true}};
  cases.push_back(
    {"HTTP/1.0 200 OK\r\nContent-Type: application/json; charset=\"utf-8\"\r\n\r\n{}", true, {}, 1, true});
  cases.push_back(
    {"HTTP/1.1 302 Found\r\nContent-Type: application/json\r\nLocation: https://attacker/\r\n"
     "Content-Length: 2\r\n\r\n{}",
      true,
      {},
      0,
      false,
      false,
      false,
      302});
  cases.push_back(
    {"HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}",
      true,
      {},
      0,
      false,
      false,
      false,
      400});
  std::string intermediate = "HTTP/1.1 100 Continue\r\n\r\n";
  std::string four = intermediate + intermediate + intermediate + intermediate;
  cases.push_back({four + headers + "Content-Length: 2\r\n\r\n{}", true});
  cases.push_back({four + intermediate + headers + "Content-Length: 2\r\n\r\n{}", false, pg::Error::resource_limit});
  cases.push_back(
    {"HTTP/2.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}", false, pg::Error::protocol});
  cases.push_back({headers + "Content-Length: -1\r\n\r\n{}", false, pg::Error::protocol});
  cases.push_back({headers + "Transfer-Encoding: chunked\r\n\r\n2\n{}\n0\n\n", false, pg::Error::protocol});
  cases.push_back({headers + "Transfer-Encoding: gzip\r\n\r\n{}", false, pg::Error::protocol, 0, true});
  cases.push_back(
    {"HTTP/1.1 200 OK\nContent-Type: application/json\nContent-Length: 2\n\n{}", false, pg::Error::protocol});
  cases.push_back({headers + "X-Empty:\r\nContent-Length: 2\r\n\r\n{}", true, {}, 1});
  std::string body(65536, 'a');
  cases.push_back({headers + "Content-Length: 65536\r\n\r\n" + body, true, {}, 2048, false, false, false, 200, body});
  std::string many_headers;
  for (unsigned index = 0; index < 64; ++index)
    many_headers += "X-Test: a\r\n";
  cases.push_back({headers + many_headers + "Content-Length: 2\r\n\r\n{}", false, pg::Error::resource_limit});
  std::string huge_headers;
  for (unsigned index = 0; index < 5; ++index)
    huge_headers += "X-Test: " + std::string(8000, 'a') + "\r\n";
  cases.push_back({headers + huge_headers + "Content-Length: 2\r\n\r\n{}", false, pg::Error::resource_limit});
  const std::array bad_statuses{"099", "600", "999"};
  for (auto status : bad_statuses)
    cases.push_back(
      {std::string{"HTTP/1.1 "} + status +
          " Bad\r\nContent-Type: application/json\r\n"
          "Content-Length: 2\r\n\r\n{}",
        false,
        pg::Error::protocol});
  cases.push_back(
    {headers + "Transfer-Encoding: gzip, chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n", false, pg::Error::protocol});
  cases.push_back(
    {headers +
        "Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n"
        "2\r\n{}\r\n0\r\n\r\n",
      false,
      pg::Error::protocol});
  std::string expensive_chunks;
  for (unsigned index = 0; index < 600; ++index)
    expensive_chunks += "1;extension=" + std::string(512, 'a') + "\r\n{\r\n";
  cases.push_back(
    {headers + "Transfer-Encoding: chunked\r\n\r\n" + expensive_chunks + "0\r\n\r\n",
      false,
      pg::Error::resource_limit});
  return cases;
}

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
static void runtime_tests(weave::TlsContext client, weave::TlsContext server, const std::vector<Case> &cases)
{
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    check(bool(runtime), "Runtime");

    for (unsigned index = 0; index < 8; ++index) {
      auto test = cases[index % 6];
      auto result = runtime->run(weave::timeout(10s, scenario(client, server, test)));
      check(bool(result), "Runtime HTTPS");
    }

    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 32; ++index) {
      auto job = runtime->spawn(owned_case(client, server, cases[index % 6]));
      check(bool(job), "Concurrent spawn");
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs)
      check(bool(std::move(job).get()), "Concurrent HTTPS completed");
    runtime->join();
  }
}
#endif

static void transport_policy_tests(
  weave::Context &ctx,
  const fixture::Certificates &certificates,
  weave::TlsContext server)
{
  auto untrusted = weave::TlsContext::client({.ca_file = certificates.untrusted});
  check(bool(untrusted), "Untrusted CA context");
  auto verification_error = weave::make_error_code(weave::TlsError::certificate_verification);
  auto rejected = ctx.run(weave::timeout(10s, rejected_transport(*untrusted, server, verification_error)));
  check(bool(rejected), "CA failure drain");

  auto http2_client = weave::TlsContext::client({.ca_file = certificates.ca, .alpn = {"h2"}});
  auto http2_server = weave::TlsContext::server(
    {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key, .alpn = {"h2"}});
  check(bool(http2_client) && bool(http2_server), "HTTP/2 contexts");
  auto protocol_error = pg::make_error_code(pg::Error::protocol);
  rejected = ctx.run(weave::timeout(10s, rejected_transport(*http2_client, *http2_server, protocol_error)));
  check(bool(rejected), "ALPN failure drain");
}

static void lifetime_tests(weave::TlsContext client, weave::TlsContext server)
{
  {
    constexpr std::string_view secret = "Basic dW5zdGFydGVkOnNlY3JldA==";
    wire::OAuthHttpRequest request;
    request.url = "https://127.0.0.1:1/";
    request.authorization.assign(secret.begin(), secret.end());
    fixture::CredentialAllocation allocation({request.authorization.data(), request.authorization.size()});
    {
      auto task = wire::oauth_https(std::move(request), client, 1s);
    }
    check(allocation.cleansed(), "Unstarted HTTP task owns and cleanses credentials");
  }
  const std::array shutdowns{false, true};
  for (bool shutdown : shutdowns) {
    auto ctx = weave::Context::create();
    check(bool(ctx), "Lifetime Context");
    auto acquired = std::make_shared<bool>(false);
    std::weak_ptr weak = acquired;
    auto job = ctx->spawn(pending(client, server, acquired));
    check(bool(job), "Pending job");
    auto wait = [&]() -> weave::Task<void> {
      while (!*acquired)
        co_await weave::sleep_for(1ms);
    };
    auto started = ctx->run(weave::timeout(5s, wait()));
    check(bool(started), "Pending HTTPS started");
    if (shutdown) {
      ctx->shutdown();
    } else {
      job->cancel();
      auto join = [&]() -> weave::Task<void> {
        auto result = co_await weave::as_result(std::move(*job));
        check(!result && result.error() == std::errc::operation_canceled, "Cancellation/drain");
      };
      check(bool(ctx->run(join())), "Join canceled graph");
    }
    acquired.reset();
    check(weak.expired(), "Pending frame ownership released");
  }
}

int main()
{
  urls();
  fixture::Certificates certificates;
  auto client = weave::TlsContext::client({.ca_file = certificates.ca, .alpn = {"http/1.1"}});
  auto server = weave::TlsContext::server(
    {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key, .alpn = {"http/1.1"}});
  check(bool(client) && bool(server), "TLS credentials");
  auto ctx = weave::Context::create();
  check(bool(ctx), "Context");

  auto cases = protocol_cases();
  for (auto &test : cases) {
    auto result = ctx->run(weave::timeout(10s, scenario(*client, *server, test)));
    if (!result)
      std::fprintf(stderr, "OAuth HTTPS scenario: %s\n", result.error().message().c_str());
    check(bool(result), "Scenario completion");
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  runtime_tests(*client, *server, cases);
#endif
  transport_policy_tests(*ctx, certificates, *server);
  lifetime_tests(*client, *server);
  std::printf("OAuth HTTPS: %u checks, %zu protocol cases\n", checks.load(), cases.size());
}
