#include <weave/postgres/connection.hpp>
#include <doctest/doctest.h>
#include <atomic>
#include <thread>

namespace pg = weave::pg;

static_assert(std::same_as<decltype(pg::Diagnostic{}.format()), weave::Result<std::string>>);

static pg::Diagnostic diagnostic()
{
  return {
    {{'S', "ERROR"},
      {'V', "ERROR"},
      {'C', "42601"},
      {'M', "primary"},
      {'D', "secret detail"},
      {'H', "hint"},
      {'W', "secret context"},
      {'P', "2"}}};
}

TEST_CASE("PostgreSQL diagnostic formatting has explicit disclosure policy and owning output")
{
  auto error = diagnostic();
  auto minimal = error.format();
  REQUIRE(minimal);
  CHECK(*minimal == "ERROR:  primary at character 2\n");
  CHECK(minimal->find("secret") == std::string::npos);
  auto state = error.format({.verbosity = pg::DiagnosticVerbosity::sqlstate});
  REQUIRE(state);
  CHECK(*state == "ERROR:  42601\n");

  auto full = error.format(
    {.verbosity = pg::DiagnosticVerbosity::verbose, .context = pg::DiagnosticContext::always, .query = "abc"});
  REQUIRE(full);
  CHECK(
    *full ==
    "ERROR:  42601: primary\nLINE 1: abc\n         ^\nDETAIL:  secret detail\nHINT:  hint\nCONTEXT:  secret context\n");
  auto copied = error;
  error.fields.clear();
  CHECK(*minimal == "ERROR:  primary at character 2\n");
  auto retained = copied.format();
  REQUIRE(retained);
  CHECK(*retained == *minimal);

  pg::Diagnostic notice{{{'S', "NOTICE"}, {'V', "NOTICE"}, {'C', "00000"}, {'M', "notice"}, {'W', "context"}}};
  auto hidden = notice.format(
    {.verbosity = pg::DiagnosticVerbosity::standard, .context = pg::DiagnosticContext::errors});
  REQUIRE(hidden);
  CHECK(*hidden == "NOTICE:  notice\n");
  auto shown = notice.format(
    {.verbosity = pg::DiagnosticVerbosity::standard, .context = pg::DiagnosticContext::always});
  REQUIRE(shown);
  CHECK(*shown == "NOTICE:  notice\nCONTEXT:  context\n");
  auto success_state = notice.format({.verbosity = pg::DiagnosticVerbosity::sqlstate});
  REQUIRE(success_state);
  CHECK(*success_state == "NOTICE:  00000\n");
}

TEST_CASE("PostgreSQL diagnostic bounds fail without returning a partial message")
{
  auto error = diagnostic();
  pg::DiagnosticFormat options{
    .verbosity = pg::DiagnosticVerbosity::verbose,
    .context = pg::DiagnosticContext::always,
    .query = "abc"};
  auto full = error.format(options);
  REQUIRE(full);
  options.output_bytes = full->size();
  auto exact = error.format(options);
  REQUIRE(exact);
  CHECK(*exact == *full);
  --options.output_bytes;
  auto bounded = error.format(options);
  REQUIRE_FALSE(bounded);
  CHECK(bounded.error() == pg::make_error_code(pg::Error::resource_limit));

  std::size_t input = options.query->size();
  for (const auto &[code, text] : error.fields)
    input += text.size();
  options.output_bytes = 16384;
  options.input_bytes = input;
  CHECK(error.format(options).has_value());
  --options.input_bytes;
  auto too_large = error.format(options);
  REQUIRE_FALSE(too_large);
  CHECK(too_large.error() == pg::make_error_code(pg::Error::resource_limit));

  auto empty = pg::Diagnostic{}.format({.input_bytes = 0, .output_bytes = 0});
  REQUIRE(empty);
  CHECK(empty->empty());
  pg::Diagnostic huge{{{'M', std::string(1024 * 1024, 'x')}}};
  auto long_output = huge.format();
  REQUIRE_FALSE(long_output);
  CHECK(long_output.error() == pg::make_error_code(pg::Error::resource_limit));
  huge.fields[0].second.push_back('x');
  auto long_input = huge.format({.output_bytes = 2 * 1024 * 1024});
  REQUIRE_FALSE(long_input);
  CHECK(long_input.error() == pg::make_error_code(pg::Error::resource_limit));
}

TEST_CASE("PostgreSQL diagnostic fields and positions are checked even when not displayed")
{
  const std::array positions{"", "0", "-1", "+1", "1x", " 1", "18446744073709551616000"};
  for (auto text : positions) {
    auto error = diagnostic();
    error.fields.back().second = text;
    auto formatted = error.format({.verbosity = pg::DiagnosticVerbosity::sqlstate});
    REQUIRE_FALSE(formatted);
    CHECK(formatted.error() == std::errc::invalid_argument);
  }
  const std::array invalid_text{
    std::string{"\0", 1},
    std::string{"\xc0\x80", 2},
    std::string{"\xed\xa0\x80", 3},
    std::string{"\x80", 1}};
  for (const auto &bytes : invalid_text) {
    auto error = diagnostic();
    error.fields.emplace_back('X', bytes);
    auto formatted = error.format();
    REQUIRE_FALSE(formatted);
    CHECK(formatted.error() == std::errc::illegal_byte_sequence);
    auto query = diagnostic().format({.query = bytes});
    REQUIRE_FALSE(query);
    CHECK(query.error() == std::errc::illegal_byte_sequence);
  }

  auto repeated = diagnostic();
  repeated.fields.emplace_back('S', "FATAL");
  auto duplicate = repeated.format();
  REQUIRE_FALSE(duplicate);
  CHECK(duplicate.error() == std::errc::invalid_argument);
  CHECK_FALSE(pg::Diagnostic{{{'\0', "zero"}}}.format());
  CHECK_FALSE(pg::Diagnostic{{{'C', "bad"}}}.format());
  CHECK_FALSE(diagnostic().format({.verbosity = static_cast<pg::DiagnosticVerbosity>(-1)}));
  CHECK_FALSE(diagnostic().format({.context = static_cast<pg::DiagnosticContext>(3)}));
  CHECK_FALSE(diagnostic().format({.encoding = static_cast<pg::Encoding>(255)}));

  pg::Diagnostic maximum;
  for (unsigned code = 1; code <= 255; ++code) {
    if (code == 'C')
      maximum.fields.emplace_back(static_cast<char>(code), "00000");
    else if (code == 'P' || code == 'p')
      maximum.fields.emplace_back(static_cast<char>(code), "1");
    else
      maximum.fields.emplace_back(static_cast<char>(code), "");
  }
  CHECK(maximum.format().has_value());
  maximum.fields.emplace_back('X', "overflow");
  auto count = maximum.format();
  REQUIRE_FALSE(count);
  CHECK(count.error() == pg::make_error_code(pg::Error::resource_limit));
}

TEST_CASE("PostgreSQL query text is explicit and cursor positions are character based")
{
  pg::Diagnostic first{{{'S', "ERROR"}, {'M', "primary"}, {'P', "1"}}};
  auto supplied = first.format({.verbosity = pg::DiagnosticVerbosity::standard, .query = std::string_view{}});
  REQUIRE(supplied);
  CHECK(*supplied == "ERROR:  primary\nLINE 1: \n        ^\n");
  auto unspecified = first.format({.verbosity = pg::DiagnosticVerbosity::standard});
  REQUIRE(unspecified);
  CHECK(*unspecified == "ERROR:  primary at character 1\n");

  first.fields.back().second = "2";
  auto wide = first.format(
    {.verbosity = pg::DiagnosticVerbosity::standard,
      .query = "\xe4\xb8\xad"
               "x"});
  REQUIRE(wide);
  CHECK(
    *wide ==
    "ERROR:  primary\nLINE 1: \xe4\xb8\xad"
    "x\n          ^\n");
  auto legacy = first.format(
    {.verbosity = pg::DiagnosticVerbosity::standard,
      .encoding = pg::Encoding::sjis,
      .query = "\x83\x41"
               "x"});
  REQUIRE(legacy);
  CHECK(
    *legacy ==
    "ERROR:  primary\nLINE 1: \x83\x41"
    "x\n          ^\n");
}

TEST_CASE("PostgreSQL immutable diagnostic formatting has no shared output state")
{
  auto error = diagnostic();
  pg::DiagnosticFormat options{
    .verbosity = pg::DiagnosticVerbosity::verbose,
    .context = pg::DiagnosticContext::always,
    .query = "abc"};
  auto expected = error.format(options);
  REQUIRE(expected);
  std::atomic<bool> valid{true};
  std::vector<std::jthread> threads;
  for (unsigned worker = 0; worker < 4; ++worker) {
    threads.emplace_back([&] {
      for (unsigned iteration = 0; iteration < 2000; ++iteration) {
        auto text = error.format(options);
        if (!text || *text != *expected) {
          valid.store(false);
          return;
        }
      }
    });
  }
  threads.clear();
  CHECK(valid.load());
}
