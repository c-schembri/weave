#define SECURITY_WIN32
#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include <weave/log.hpp>
#include "wire.hpp"
#include "tls_certificates.hpp"
#include <windows.h>
#include <security.h>

namespace pg = weave::pg;
namespace native = weave::pg::detail;
using namespace std::chrono_literals;

struct Acceptor {
  CredHandle credentials{};
  CtxtHandle context{};
  bool acquired = false;
  bool initialized = false;
  bool complete = false;
  bool kerberos = false;

  ~Acceptor()
  {
    if (initialized)
      DeleteSecurityContext(&context);
    if (acquired)
      FreeCredentialsHandle(&credentials);
  }

  bool start()
  {
    TimeStamp expiry{};
    acquired = AcquireCredentialsHandleW(
                 nullptr,
                 const_cast<wchar_t *>(L"Negotiate"),
                 SECPKG_CRED_INBOUND,
                 nullptr,
                 nullptr,
                 nullptr,
                 nullptr,
                 &credentials,
                 &expiry) == SEC_E_OK;
    return acquired;
  }

  weave::Result<native::Bytes> next(std::span<std::byte> token)
  {
    SecBuffer input{static_cast<ULONG>(token.size()), SECBUFFER_TOKEN, token.data()};
    SecBufferDesc received{SECBUFFER_VERSION, 1, &input};
    SecBuffer output{0, SECBUFFER_TOKEN, nullptr};
    SecBufferDesc sent{SECBUFFER_VERSION, 1, &output};
    CtxtHandle next{};
    ULONG flags = 0;
    TimeStamp expiry{};
    const auto status = AcceptSecurityContext(
      &credentials,
      initialized ? &context : nullptr,
      &received,
      ASC_REQ_ALLOCATE_MEMORY,
      SECURITY_NETWORK_DREP,
      &next,
      &sent,
      &flags,
      &expiry);
    if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED) {
      context = next;
      initialized = true;
    }
    native::Bytes reply;
    if (output.pvBuffer) {
      auto *data = static_cast<std::byte *>(output.pvBuffer);
      reply.assign(data, data + output.cbBuffer);
      SecureZeroMemory(output.pvBuffer, output.cbBuffer);
      FreeContextBuffer(output.pvBuffer);
    }
    if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED)
      return std::unexpected(std::error_code(static_cast<int>(status), std::system_category()));
    complete = status == SEC_E_OK;
    if (complete) {
      SecPkgContext_NegotiationInfoW information{};
      const auto queried = QueryContextAttributesW(&context, SECPKG_ATTR_NEGOTIATION_INFO, &information);
      if (queried != SEC_E_OK)
        return std::unexpected(std::error_code(static_cast<int>(queried), std::system_category()));
      kerberos = information.PackageInfo && information.PackageInfo->Name &&
        _wcsicmp(information.PackageInfo->Name, L"Kerberos") == 0;
      const bool ntlm = information.PackageInfo && information.PackageInfo->Name &&
        _wcsicmp(information.PackageInfo->Name, L"NTLM") == 0;
      if (information.PackageInfo)
        FreeContextBuffer(information.PackageInfo);
      if (!kerberos && !ntlm)
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
    }
    return reply;
  }
};

static weave::Task<void> server(weave::TcpListener &listener, const weave::TlsContext &tls, bool strict, bool &kerberos)
{
  auto transport = co_await listener.accept();
  std::array<std::byte, 8> ssl;
  co_await transport.read_exactly(ssl);
  native::Reader request{ssl};
  if (request.integer() != 8 || request.integer() != 80877103)
    co_await weave::fail(std::errc::bad_message);
  const std::array supported{std::byte{'S'}};
  co_await transport.write_all(supported);
  auto client = co_await weave::tls::server(std::move(transport), tls);
  std::array<std::byte, 4> header;
  co_await client.read_exactly(header);
  native::Reader size{header};
  native::Bytes startup(size.integer() - 4);
  co_await client.read_exactly(startup);
  Acceptor acceptor;
  if (!acceptor.start())
    co_await weave::fail(std::errc::permission_denied);
  native::Writer challenge;
  native::Writer kind;
  kind.integer(9);
  challenge.message('R', kind);
  co_await client.write_all(challenge.bytes);
  for (unsigned step = 0; step < 8; ++step) {
    std::array<std::byte, 5> packet;
    auto read = co_await weave::as_result(client.read_exactly(packet));
    if (!read) {
      if (strict && step != 0)
        co_return;
      co_await weave::fail(read.error());
    }
    native::Reader message{packet};
    if (message.integer(1) != 'p')
      co_await weave::fail(std::errc::bad_message);
    native::Bytes proof(message.integer() - 4);
    co_await client.read_exactly(proof);
    auto reply = acceptor.next(proof);
    if (!reply)
      co_await weave::fail(reply.error());
    if (!reply->empty()) {
      native::Writer response;
      native::Writer continuation;
      continuation.integer(8);
      continuation.raw(*reply);
      response.message('R', continuation);
      co_await client.write_all(response.bytes);
    }
    if (acceptor.complete) {
      kerberos = acceptor.kerberos;
      native::Writer response;
      native::Writer authenticated;
      authenticated.integer(0);
      response.message('R', authenticated);
      native::Writer ready;
      ready.integer('I', 1);
      response.message('Z', ready);
      auto written = co_await weave::as_result(client.write_all(response.bytes));
      const bool rejected = strict && !kerberos;
      if (!written && !rejected)
        co_await weave::fail(written.error());
      std::array<std::byte, 5> finish;
      auto terminated = co_await weave::as_result(client.read_exactly(finish));
      if (rejected) {
        if (terminated)
          co_await weave::fail(std::errc::bad_message);
        co_return;
      }
      if (!terminated)
        co_await weave::fail(terminated.error());
      if (finish[0] != std::byte{'X'})
        co_await weave::fail(std::errc::bad_message);
      // PostgreSQL Terminate closes immediately; consume the peer's close_notify, not a reciprocal write.
      std::array<std::byte, 1> eof;
      if (co_await client.read(eof) != 0)
        co_await weave::fail(std::errc::bad_message);
      co_return;
    }
  }
  co_await weave::fail(std::errc::bad_message);
}

static weave::Task<void> client(pg::Options options, bool strict, const bool &kerberos)
{
  auto result = co_await weave::as_result(pg::connect(options));
  if (strict && !kerberos) {
    if (result || result.error() != pg::Error::authentication)
      co_await weave::fail(std::errc::bad_message);
    co_return;
  }
  if (!result)
    co_await weave::fail(result.error());
  if (result->authentication_method() != pg::Authentication::sspi)
    co_await weave::fail(std::errc::bad_message);
  co_await result->finish();
}

static weave::Task<void> exchange(
  const pg::GssContext &gss,
  const weave::TlsContext &server_tls,
  const weave::TlsContext &client_tls,
  bool strict)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
  pg::Options options{
    .host = "localhost",
    .port = listener.local_port(),
    .user = "test",
    .database = "postgres",
    .tls = client_tls};
  options.hosts = {{.name = "localhost", .port = options.port, .address = *weave::IpAddress::parse("127.0.0.1")}};
  options.max_protocol = pg::ProtocolVersion::v30;
  options.gss = gss;
  options.gss_mutual = strict;
  options.authentication.methods = {pg::Authentication::sspi};
  bool kerberos = false;
  co_await weave::when_all(server(listener, server_tls, strict, kerberos), client(options, strict, kerberos));
}

struct Token {
  HANDLE value = nullptr;

  ~Token()
  {
    if (value)
      CloseHandle(value);
  }
};

static weave::Result<pg::GssContext> identity()
{
  const auto denied = std::unexpected(std::make_error_code(std::errc::permission_denied));
  Token original;
  Token limited;
  Token impersonation;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &original.value))
    return denied;
  const bool duplicated = DuplicateTokenEx(
                            original.value,
                            TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE,
                            nullptr,
                            SecurityIdentification,
                            TokenImpersonation,
                            &limited.value) != FALSE &&
    DuplicateTokenEx(
      original.value,
      TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE,
      nullptr,
      SecurityImpersonation,
      TokenImpersonation,
      &impersonation.value) != FALSE;
  if (!duplicated)
    return denied;
  if (!SetThreadToken(nullptr, limited.value))
    return denied;
  auto rejected = pg::GssContext::create();
  const bool restored = RevertToSelf() != FALSE;
  if (!restored || rejected || rejected.error() != std::errc::permission_denied)
    return denied;
  if (!SetThreadToken(nullptr, impersonation.value))
    return denied;
  auto captured = pg::GssContext::create();
  const bool reverted = RevertToSelf() != FALSE;
  if (!reverted)
    return denied;
  return captured;
}

int main()
{
  auto gss = identity();
  if (!gss)
    return weave::report_error(gss.error());
  fixture::Certificates certificates;
  auto server_tls = weave::TlsContext::server(
    {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key});
  auto client_tls = weave::TlsContext::client({.ca_file = certificates.ca});
  if (!server_tls || !client_tls)
    return 1;
  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());
  const std::array policies{false, true};
  for (bool strict : policies) {
    for (unsigned repetition = 0; repetition < 20; ++repetition) {
      auto result = ctx->run(weave::timeout(10s, exchange(*gss, *server_tls, *client_tls, strict)));
      if (!result) {
        std::fprintf(stderr, "Policy=%d repetition=%u\n", strict, repetition);
        return weave::report_error(result.error());
      }
    }
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    if (!runtime)
      return weave::report_error(runtime.error());
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 32; ++index) {
      auto job = runtime->spawn(exchange(*gss, *server_tls, *client_tls, false));
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
#endif
  std::puts("SSPI PostgreSQL TLS startups and effective-token capture passed");
}
