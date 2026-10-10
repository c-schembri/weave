#include "gss_pool.hpp"
#include "gss_acceptor.hpp"
#include <weave/io/context.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/scope.hpp>
#include <dlfcn.h>
#include <cstdio>

namespace pg = weave::pg;
namespace native = weave::pg::detail;
using Acceptor = pg::test::GssAcceptor;

struct Audit {
  void (*mark_io)() = nullptr;
  unsigned (*created)() = nullptr;
  unsigned (*destroyed)() = nullptr;
  unsigned (*violations)() = nullptr;

  bool load()
  {
    mark_io = reinterpret_cast<decltype(mark_io)>(dlsym(RTLD_DEFAULT, "weave_test_gss_io"));
    created = reinterpret_cast<decltype(created)>(dlsym(RTLD_DEFAULT, "weave_test_gss_created"));
    destroyed = reinterpret_cast<decltype(destroyed)>(dlsym(RTLD_DEFAULT, "weave_test_gss_destroyed"));
    violations = reinterpret_cast<decltype(violations)>(dlsym(RTLD_DEFAULT, "weave_test_gss_violations"));
    return mark_io && created && destroyed && violations;
  }
};

static bool exchange(native::Gss &client, Acceptor &server, const std::string &cache)
{
  native::GssOptions options;
  options.credential_cache = cache;
  options.protect = true;
  auto first = client.start("localhost", pg::Authentication::gss, options);
  if (!first || !server.start())
    return false;
  auto reply = server.accept(first->bytes);
  if (!reply || reply->empty())
    return false;
  auto final = client.next(*reply);
  return final && final->complete && final->mutual && final->bytes.empty() && client.plaintext_limit() != 0 &&
    client.plaintext_limit() <= native::gss_record_limit;
}

static bool protection(std::string_view mode, const std::string &cache)
{
  native::Gss client;
  Acceptor server;
  if (!exchange(client, server, cache))
    return false;
  native::SecretStorage<std::byte> plaintext(client.plaintext_limit(), std::byte{0x53});
  if (mode == "round-trip") {
    const std::array sizes{std::size_t{0}, std::size_t{1}, std::size_t{4096}, client.plaintext_limit()};
    for (unsigned iteration = 0; iteration < 16; ++iteration) {
      for (auto size : sizes) {
        auto input = std::span{plaintext}.first(size);
        auto outgoing = client.wrap(input);
        if (!outgoing || outgoing->empty() || outgoing->size() > native::gss_record_limit ||
          !server.unwrap(*outgoing, input))
          return false;
        auto incoming = server.wrap(input);
        if (!incoming)
          return false;
        auto received = client.unwrap(*incoming);
        if (!received || !std::ranges::equal(*received, input))
          return false;
      }
    }
    return client.complete();
  }
  if (mode == "write-bound") {
    plaintext.resize(client.plaintext_limit() + 1);
    auto result = client.wrap(plaintext);
    return !result && result.error() == pg::Error::resource_limit && client.complete();
  }

  auto record = server.wrap(plaintext, mode != "integrity-only");
  if (!record)
    return false;
  if (mode == "tamper")
    record->back() ^= std::byte{1};
  if (mode == "truncate")
    record->resize(record->size() / 2);
  if (mode == "empty")
    record->clear();
  if (mode == "oversized")
    record->resize(native::gss_record_limit + 1);
  if (mode == "gap") {
    record = server.wrap(plaintext);
    if (!record)
      return false;
  }
  if (mode == "replay" && !client.unwrap(*record))
    return false;
  auto result = client.unwrap(*record);
  return !result && !client.complete() && !client.wrap(plaintext) && !client.unwrap(*record);
}

static weave::Task<void> send_record(
  native::GssSession &session,
  Acceptor &server,
  const native::SecretStorage<std::byte> &plaintext)
{
  auto record = co_await session.wrap(plaintext);
  if (!server.unwrap(record, plaintext))
    co_await weave::fail(pg::Error::authentication);
  static_cast<void>(session.diagnostic());
  static_cast<void>(session.complete());
}

static weave::Task<void> receive_record(
  native::GssSession &session,
  native::SecretStorage<std::byte> incoming,
  const native::SecretStorage<std::byte> &plaintext)
{
  auto received = co_await session.unwrap(incoming);
  if (!std::ranges::equal(received, plaintext))
    co_await weave::fail(pg::Error::authentication);
  static_cast<void>(session.diagnostic());
  static_cast<void>(session.complete());
}

static weave::Task<void> session_exchange(
  Audit &audit,
  pg::GssContext provider,
  bool explicit_close,
  bool failed = false)
{
  audit.mark_io();
  native::GssSession session;
  native::GssOptions options;
  options.protect = true;
  auto first = co_await session.start(provider, "localhost", pg::Authentication::gss, options);
  Acceptor server;
  if (!server.start())
    co_await weave::fail(pg::Error::authentication);
  auto reply = server.accept(first.bytes);
  if (!reply)
    co_await weave::fail(reply.error());
  auto final = co_await session.next(*reply);
  if (!final.complete || session.plaintext_limit() == 0)
    co_await weave::fail(pg::Error::authentication);

  native::SecretStorage<std::byte> plaintext(session.plaintext_limit(), std::byte{0x67});
  for (unsigned iteration = 0; iteration < 8; ++iteration) {
    auto incoming = server.wrap(plaintext);
    if (!incoming)
      co_await weave::fail(incoming.error());
    if (failed) {
      incoming->back() ^= std::byte{1};
      co_await session.unwrap(*incoming);
      co_await weave::fail(std::errc::bad_message);
    }
    co_await weave::when_all(
      send_record(session, server, plaintext),
      receive_record(session, std::move(*incoming), plaintext));
  }
  if (explicit_close)
    co_await session.close();
}

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
static weave::Task<void> jobs(weave::TaskScope &scope, Audit &audit, pg::GssContext provider)
{
  audit.mark_io();
  for (unsigned index = 0; index < 32; ++index) {
    auto submitted = scope.spawn(session_exchange(audit, provider, index % 2 == 0));
    if (!submitted)
      co_await weave::fail(submitted.error());
  }
}
#endif

static bool retirement(std::string_view mode, const std::string &cache)
{
  Audit audit;
  if (!audit.load())
    return false;
  const auto prior_created = audit.created();
  const auto prior_destroyed = audit.destroyed();
  std::optional<pg::GssContext> provider;
  {
    auto setup = pg::GssContext::create({.workers = 2, .capacity = 64, .credential_cache = cache});
    if (!setup)
      return false;
    provider = *setup;
  }
  if (mode == "retirement-context" || mode == "retirement-failure") {
    auto context = weave::Context::create();
    if (!context)
      return false;
    for (unsigned index = 0; index < 32; ++index) {
      const bool failed = mode == "retirement-failure";
      auto result = context->run(session_exchange(audit, *provider, !failed && index % 2 == 0, failed));
      if (failed) {
        if (result || result.error() == std::errc::bad_message)
          return false;
      } else if (!result) {
        std::fprintf(stderr, "Context session: %s\n", result.error().message().c_str());
        return false;
      }
    }
  } else {
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
    auto scheduler = mode == "retirement-affine" ? weave::Scheduler::worker_affine : weave::Scheduler::work_stealing;
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    if (!runtime)
      return false;
    auto result = runtime->run(weave::scope([&](weave::TaskScope &scope) {
      return jobs(scope, audit, *provider);
    }));
    if (!result) {
      std::fprintf(stderr, "Runtime session: %s\n", result.error().message().c_str());
      return false;
    }
#else
    return false;
#endif
  }
  // Context/Runtime has already died. Last-owner teardown must drain transferred
  // native destruction without posting through either execution domain.
  provider.reset();
  return audit.created() - prior_created == 32 && audit.destroyed() - prior_destroyed == 32 && audit.violations() == 0;
}

int main(int argc, char **argv)
{
  if (argc != 3)
    return 2;
  const std::string_view mode = argv[1];
  const std::string cache = argv[2];
  if (mode.starts_with("retirement-")) {
    const std::array modes{"retirement-context", "retirement-failure", "retirement-affine", "retirement-stealing"};
    if (std::find(modes.begin(), modes.end(), mode) == modes.end())
      return 2;
    if (!retirement(mode, cache))
      return 1;
    std::puts("32 provider-backed sessions retired without I/O-thread native calls");
    return 0;
  }

  const std::array
    modes{"round-trip", "write-bound", "tamper", "truncate", "empty", "oversized", "integrity-only", "replay", "gap"};
  if (std::find(modes.begin(), modes.end(), mode) == modes.end())
    return 2;
  for (unsigned repetition = 0; repetition < 8; ++repetition) {
    if (!protection(mode, cache)) {
      std::fprintf(stderr, "Protection failed: %s, repetition %u\n", argv[1], repetition);
      return 1;
    }
  }
  std::puts("8 native Kerberos protected-record exchanges passed");
}
