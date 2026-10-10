#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/tls.hpp>
#include "credential_allocations.hpp"
#include <array>

enum class Rejection {
  validation,
  policy,
  identity
};

template <class Options>
static Options rejected_options(Rejection reason)
{
  Options options;
  options.private_key_password = std::string(127, 't');
  if (reason != Rejection::validation) {
    options.certificate_file = "missing-credential-test-certificate.pem";
    options.private_key_file = "missing-credential-test-private-key.pem";
  }
  if (reason == Rejection::policy)
    options.min_version = static_cast<weave::TlsVersion>(255);

  return options;
}

TEST_CASE("TLS factories cleanse owned passphrases on every setup rejection")
{
  const std::array reasons{Rejection::validation, Rejection::policy, Rejection::identity};
  for (auto reason : reasons) {
    CAPTURE(static_cast<int>(reason));
    auto client = rejected_options<weave::TlsClientOptions>(reason);
    fixture::CredentialAllocation client_password{client.private_key_password};
    auto client_result = weave::TlsContext::client(std::move(client));
    CHECK_FALSE(client_result);
    CHECK(client_password.cleansed());

    auto server = rejected_options<weave::TlsServerOptions>(reason);
    fixture::CredentialAllocation server_password{server.private_key_password};
    auto server_result = weave::TlsContext::server(std::move(server));
    CHECK_FALSE(server_result);
    CHECK(server_password.cleansed());
  }
}
