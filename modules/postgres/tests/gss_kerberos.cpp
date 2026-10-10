#include "gss.hpp"
#include "gss_acceptor.hpp"
#include <gssapi/gssapi.h>
#include <openssl/crypto.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <thread>

namespace pg = weave::pg;
namespace native = weave::pg::detail;

using Acceptor = pg::test::GssAcceptor;

static bool provider_exchange(std::string_view mode, const std::string &cache)
{
  native::Gss client;
  native::GssOptions options;
  options.credential_cache = cache;
  options.delegate = mode == "delegate";
  if (mode == "wrong-service")
    options.service = "absent_service";
  auto first = client.start("localhost", pg::Authentication::gss, options);
  if (!first) {
    const bool expected = mode == "missing" || mode == "wrong-service";
    return expected && !client.complete() && !client.diagnostic().empty() && first.error().message() != "Success";
  }
  if (mode == "missing" || mode == "wrong-service" || first->bytes.empty() ||
    client.start("localhost", pg::Authentication::gss, options))
    return false;

  if (mode == "empty" || mode == "oversized") {
    native::SecretStorage<std::byte> invalid(mode == "empty" ? 0 : 65537);
    return !client.next(invalid) && !client.next(first->bytes) && !client.complete();
  }

  Acceptor server;
  if (!server.start())
    return false;
  auto outgoing = std::move(first->bytes);
  for (unsigned step = 0; step < 8; ++step) {
    auto reply = server.accept(outgoing);
    if (!reply || reply->empty())
      return false;
    if (mode == "corrupt")
      reply->front() ^= std::byte{1};
    if (mode == "proof-corrupt" || mode == "corrupt-handoff")
      reply->back() ^= std::byte{1};
    if (mode == "truncated")
      reply->resize(reply->size() / 2);

    weave::Result<native::GssToken> token;
    std::string captured;
    auto advance = [&] {
      token = client.next(*reply);
      captured = client.diagnostic();
    };
    if (mode == "handoff" || mode == "corrupt-handoff") {
      std::thread worker(advance);
      worker.join();
    } else {
      advance();
    }
    if (!token) {
      const bool expected = mode == "corrupt" || mode == "proof-corrupt" || mode == "corrupt-handoff" ||
        mode == "truncated";
      return expected && token.error().message() != "Success" && !captured.empty() && !client.next(*reply) &&
        !client.complete() && captured == client.diagnostic();
    }
    if (mode == "corrupt" || mode == "proof-corrupt" || mode == "corrupt-handoff" || mode == "truncated")
      return false;
    outgoing = std::move(token->bytes);
    if (server.complete && token->complete) {
      const bool delegation = mode == "delegate";
      return outgoing.empty() && token->mutual && token->mechanism == native::GssToken::Mechanism::kerberos &&
        token->delegated == delegation && (server.delegated != GSS_C_NO_CREDENTIAL) == delegation &&
        client.complete() && !client.next(*reply) && captured == client.diagnostic();
    }
    if (outgoing.empty())
      return false;
  }
  return false;
}

int main(int argc, char **argv)
{
  if (argc != 3)
    return 2;
  const std::string_view mode = argv[1];
  const std::string cache = argv[2];
  const std::array modes{
    "kerberos",
    "corrupt",
    "proof-corrupt",
    "truncated",
    "missing",
    "wrong-service",
    "empty",
    "oversized",
    "handoff",
    "corrupt-handoff",
    "delegate"};
  if (std::find(modes.begin(), modes.end(), mode) == modes.end())
    return 2;
  for (unsigned repetition = 0; repetition < 20; ++repetition) {
    if (!provider_exchange(mode, cache)) {
      std::fprintf(stderr, "Kerberos provider case failed: %s, repetition %u\n", argv[1], repetition);
      return 1;
    }
  }
  std::puts("20 native Kerberos provider exchanges passed");
}
