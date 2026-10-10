#include <weave/postgres.hpp>

#include <array>
#include <atomic>
#include <source_location>
#include <cstdio>

static std::atomic<unsigned> checks{0};

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Certificate policy check failed: %s:%u\n", where.file_name(), where.line());
    std::exit(1);
  }
}

static void parsing()
{
  const std::array modes{
    std::pair{"disable", weave::TlsCertificateMode::disable},
    std::pair{"allow", weave::TlsCertificateMode::allow},
    std::pair{"require", weave::TlsCertificateMode::require},
    std::pair{"", weave::TlsCertificateMode::allow}};
  for (const auto &[name, mode] : modes) {
    auto options = weave::pg::Options::parse(std::string("user=weave sslcertmode='") + name + "'");
    check(options.has_value() && options->client_certificate == mode);
    check(options->info().client_certificate == mode && !options->tls_options);
  }
  auto uri = weave::pg::Options::parse("postgresql://weave@localhost/db?sslcertmode=disable");
  check(uri.has_value() && uri->client_certificate == weave::TlsCertificateMode::disable);
  const std::array invalid{"true", "0", "false", "REQUIRE", "optional"};
  for (const auto *name : invalid) {
    auto options = weave::pg::Options::parse(std::string("user=weave sslcertmode=") + name);
    check(!options && options.error() == std::errc::invalid_argument);
  }
}

int main(int argc, char **argv)
{
  if (argc == 4 && std::string_view(argv[1]) == "--load") {
    auto loaded = weave::pg::Options::load(argv[3]);
    const std::string_view expected = argv[2];
    if (expected == "invalid")
      check(!loaded && loaded.error() == std::errc::invalid_argument);
    else {
      const auto mode = expected == "disable" ? weave::TlsCertificateMode::disable
        : expected == "require"               ? weave::TlsCertificateMode::require
                                              : weave::TlsCertificateMode::allow;
      check(loaded.has_value() && loaded->client_certificate == mode && loaded->info().client_certificate == mode);
    }
    std::printf("Certificate loader: %u checks\n", checks.load());
    return 0;
  }
  check(argc == 1);
  parsing();
  std::printf("Certificate parser: %u checks\n", checks.load());
}
