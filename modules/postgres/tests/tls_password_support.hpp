#pragma once

#include <weave/postgres.hpp>
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
#include <weave/runtime.hpp>
#include "runtime_fixture.hpp"
#endif
#include <weave/port.hpp>
#include "tls_encrypted_certificates.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <source_location>

static std::atomic<unsigned> checks{0};

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "PostgreSQL key provider check failed: %s:%u\n", where.file_name(), where.line());
    std::exit(1);
  }
}

static weave::TlsClientOptions client_options(const fixture::Certificates &files, const std::string &key)
{
  weave::TlsClientOptions options;
  options.ca_file = files.ca;
  options.certificate_file = files.client;
  options.private_key_file = key;
  return options;
}
