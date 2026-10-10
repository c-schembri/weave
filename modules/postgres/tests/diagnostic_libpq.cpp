#include <weave/postgres.hpp>
#include <weave/port.hpp>
#include "wire.hpp"
#include <libpq-fe.h>
#include <atomic>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <source_location>
#include <thread>

namespace pg = weave::pg;
static unsigned checks = 0;

static void check(bool value, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "diagnostic check %u failed at %u\n", checks, location.line());
    std::exit(EXIT_FAILURE);
  }
}

struct Case {
  pg::Encoding encoding;
  bool fatal;
  std::string query;
  pg::Diagnostic diagnostic;
};

static std::vector<Case> cases(const char *path)
{
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  check(stream.good());
  auto size = stream.tellg();
  check(size >= 0 && size <= 1024 * 1024);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  stream.seekg(0);
  stream.read(reinterpret_cast<char *>(bytes.data()), size);
  check(stream.good());
  pg::detail::Reader reader{bytes};
  const auto count = reader.integer();
  check(count > 100 && count <= 2048);
  std::vector<Case> result;
  for (unsigned index = 0; index < count; ++index) {
    auto encoding = static_cast<pg::Encoding>(reader.integer(1));
    bool fatal = reader.integer(1) != 0;
    auto query = reader.string();
    auto body_size = reader.integer();
    auto diagnostic = pg::detail::diagnostic(reader.take(body_size));
    check(diagnostic.has_value());
    result.push_back({encoding, fatal, std::move(query), std::move(*diagnostic)});
  }
  check(reader.empty());
  return result;
}

static std::vector<std::string> native_format(const PGresult *result)
{
  const std::array verbosity{PQERRORS_TERSE, PQERRORS_DEFAULT, PQERRORS_VERBOSE, PQERRORS_SQLSTATE};
  const std::array context{PQSHOW_CONTEXT_NEVER, PQSHOW_CONTEXT_ERRORS, PQSHOW_CONTEXT_ALWAYS};
  std::vector<std::string> formatted;
  for (auto mode : verbosity) {
    for (auto visibility : context) {
      auto message = PQresultVerboseErrorMessage(result, mode, visibility);
      check(message != nullptr);
      formatted.emplace_back(message);
      PQfreemem(message);
    }
  }
  return formatted;
}

static void native_notice(void *state, const PGresult *result)
{
  *static_cast<std::vector<std::string> *>(state) = native_format(result);
}

static void local_controls()
{
  static_assert(std::same_as<decltype(pg::Diagnostic{}.format()), weave::Result<std::string>>);
  auto empty = pg::Diagnostic{}.format({.input_bytes = 0, .output_bytes = 0});
  check(empty && empty->empty());
  pg::Diagnostic diagnostic{
    {{'S', "ERROR"},
      {'V', "ERROR"},
      {'C', "42601"},
      {'M', "primary"},
      {'D', "secret detail"},
      {'H', "hint"},
      {'W', "secret context"},
      {'P', "2"}}};
  auto minimal = diagnostic.format();
  check(minimal && *minimal == "ERROR:  primary at character 2\n");
  check(minimal->find("secret") == std::string::npos);
  auto full = diagnostic.format(
    {.verbosity = pg::DiagnosticVerbosity::verbose, .context = pg::DiagnosticContext::always, .query = "abc"});
  check(full.has_value());
  check(full->find("LINE 1: abc\n         ^\n") != std::string::npos);
  auto exact = diagnostic.format(
    {.verbosity = pg::DiagnosticVerbosity::verbose,
      .context = pg::DiagnosticContext::always,
      .query = "abc",
      .output_bytes = full->size()});
  check(exact && *exact == *full);
  auto bounded = diagnostic.format(
    {.verbosity = pg::DiagnosticVerbosity::verbose,
      .context = pg::DiagnosticContext::always,
      .query = "abc",
      .output_bytes = full->size() - 1});
  check(!bounded && bounded.error() == pg::make_error_code(pg::Error::resource_limit));
  std::size_t input = 3;
  for (const auto &[code, value] : diagnostic.fields)
    input += value.size();
  check(diagnostic.format({.query = "abc", .input_bytes = input}).has_value());
  auto too_large = diagnostic.format({.query = "abc", .input_bytes = input - 1});
  check(!too_large && too_large.error() == pg::make_error_code(pg::Error::resource_limit));

  const std::array positions{"", "0", "-1", "+1", "1x", " 1", "18446744073709551616000"};
  for (auto text : positions) {
    auto bad = diagnostic;
    bad.fields.back().second = text;
    auto value = bad.format();
    check(!value && value.error() == std::errc::invalid_argument);
  }
  const std::array encodings{
    std::string{"\0", 1},
    std::string{"\xc0\x80", 2},
    std::string{"\xed\xa0\x80", 3},
    std::string{"\x80", 1}};
  for (const auto &bytes : encodings) {
    auto bad = diagnostic;
    bad.fields.emplace_back('X', bytes);
    auto value = bad.format();
    check(!value && value.error() == std::errc::illegal_byte_sequence);
    value = diagnostic.format({.query = bytes});
    check(!value && value.error() == std::errc::illegal_byte_sequence);
  }
  auto repeated = diagnostic;
  repeated.fields.emplace_back('S', "FATAL");
  check(repeated.format().error() == std::errc::invalid_argument);
  check(pg::Diagnostic{{{'\0', "zero"}}}.format().error() == std::errc::invalid_argument);
  check(pg::Diagnostic{{{'C', "bad"}}}.format().error() == std::errc::invalid_argument);
  check(!diagnostic.format({.verbosity = static_cast<pg::DiagnosticVerbosity>(-1)}));
  check(!diagnostic.format({.context = static_cast<pg::DiagnosticContext>(3)}));
  check(!diagnostic.format({.encoding = static_cast<pg::Encoding>(255)}));
  pg::Diagnostic maximum;
  for (unsigned code = 1; code <= 255; ++code) {
    if (code == 'C')
      maximum.fields.emplace_back(static_cast<char>(code), "00000");
    else if (code == 'P' || code == 'p')
      maximum.fields.emplace_back(static_cast<char>(code), "1");
    else
      maximum.fields.emplace_back(static_cast<char>(code), "");
  }
  check(maximum.format().has_value());
  maximum.fields.emplace_back('X', "overflow");
  check(maximum.format().error() == pg::make_error_code(pg::Error::resource_limit));
  pg::Diagnostic huge{{{'M', std::string(1024 * 1024, 'x')}}};
  check(huge.format().error() == pg::make_error_code(pg::Error::resource_limit));
  huge.fields[0].second.push_back('x');
  check(huge.format({.output_bytes = 2 * 1024 * 1024}).error() == pg::make_error_code(pg::Error::resource_limit));

  std::atomic<bool> valid{true};
  std::vector<std::jthread> threads;
  for (unsigned worker = 0; worker < 4; ++worker) {
    threads.emplace_back([&] {
      for (unsigned iteration = 0; iteration < 2000; ++iteration) {
        auto text = diagnostic.format(
          {.verbosity = pg::DiagnosticVerbosity::verbose, .context = pg::DiagnosticContext::always, .query = "abc"});
        if (!text || *text != *full) {
          valid.store(false);
          return;
        }
      }
    });
  }
  threads.clear();
  check(valid.load());
  pg::Diagnostic first{{{'S', "ERROR"}, {'M', "primary"}, {'P', "1"}}};
  auto explicit_empty = first.format({.verbosity = pg::DiagnosticVerbosity::standard, .query = std::string_view{}});
  check(explicit_empty && *explicit_empty == "ERROR:  primary\nLINE 1: \n        ^\n");
  auto unspecified = first.format({.verbosity = pg::DiagnosticVerbosity::standard});
  check(unspecified && *unspecified == "ERROR:  primary at character 1\n");
}

int main(int argc, char **argv)
{
  check(argc == 4);
  check(std::setlocale(LC_ALL, "C") != nullptr);
  check(PQlibVersion() == std::atoi(argv[3]));
  local_controls();
  auto samples = cases(argv[2]);
  auto port = weave::parse_port(argv[1]);
  check(port.has_value());
  pg::Options options;
  options.host = "127.0.0.1";
  options.port = *port;
  options.user = "probe";
  options.plaintext = true;
  pg::Diagnostic notice;
  auto database = pg::BlockingConnection::connect(std::move(options), [&](const pg::Diagnostic &value) noexcept {
    notice = value;
  });
  check(database.has_value());

  const std::array<const char *, 8>
    keys{"host", "port", "user", "dbname", "sslmode", "gssencmode", "connect_timeout", nullptr};
  const std::array<const char *, 8> values{"127.0.0.1", argv[1], "probe", "probe", "disable", "disable", "10", nullptr};
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{PQconnectdbParams(keys.data(), values.data(), 0), &PQfinish};
  check(connection && PQstatus(connection.get()) == CONNECTION_OK);
  std::vector<std::string> received_notice;
  PQsetNoticeReceiver(connection.get(), &native_notice, &received_notice);
  const std::array verbosity{
    pg::DiagnosticVerbosity::terse,
    pg::DiagnosticVerbosity::standard,
    pg::DiagnosticVerbosity::verbose,
    pg::DiagnosticVerbosity::sqlstate};
  const std::array visibility{
    pg::DiagnosticContext::never,
    pg::DiagnosticContext::errors,
    pg::DiagnosticContext::always};

  for (std::size_t index = 0; index < samples.size(); ++index) {
    const auto &sample = samples[index];
    auto result = database->query_outcomes(sample.query);
    check(result && result->size() == 1);
    auto error = sample.fatal ? (*result)[0].error : notice;
    check(error.fields == sample.diagnostic.fields);
    received_notice.clear();
    std::unique_ptr<PGresult, decltype(&PQclear)> native{PQexec(connection.get(), sample.query.c_str()), &PQclear};
    check(native && PQresultStatus(native.get()) == (sample.fatal ? PGRES_FATAL_ERROR : PGRES_COMMAND_OK));
    auto formatted = sample.fatal ? native_format(native.get()) : received_notice;
    check(formatted.size() == 12);
    unsigned offset = 0;
    for (auto mode : verbosity) {
      for (auto context : visibility) {
        auto message = error.format(
          {.verbosity = mode, .context = context, .encoding = sample.encoding, .query = sample.query});
        if (!message || *message != formatted[offset]) {
          std::fprintf(
            stderr,
            "case %zu, mode %u, context %u mismatch\n",
            index,
            static_cast<unsigned>(mode),
            static_cast<unsigned>(context));
          std::fprintf(
            stderr,
            "weave: %s\nnative: %s\n",
            message ? message->c_str() : "<format failure>",
            formatted[offset].c_str());
        }
        check(message && *message == formatted[offset]);
        ++offset;
      }
    }
    auto retained = error;
    auto copied = retained.format(
      {.verbosity = pg::DiagnosticVerbosity::verbose,
        .context = pg::DiagnosticContext::always,
        .encoding = sample.encoding,
        .query = sample.query});
    check(copied && *copied == formatted[8]);
  }
  std::printf("%u checks; %zu wire cases x 12 formats; native libpq %d\n", checks, samples.size(), PQlibVersion());
}
