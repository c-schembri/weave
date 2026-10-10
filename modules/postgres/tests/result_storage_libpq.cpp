#include <libpq-fe.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>

using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
static unsigned checks = 0;

static void check(bool condition, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "Native result storage check failed: %u\n", location.line());
    std::exit(1);
  }
}

static void values(const PGresult &result)
{
  check(PQresultStatus(&result) == PGRES_TUPLES_OK);
  check(PQnfields(&result) == 2 && PQntuples(&result) == 2);
  check(std::string_view{PQfname(&result, 0)} == "payload");
  check(PQfformat(&result, 0) == 1 && PQftype(&result, 0) == 17);
  check(PQgetlength(&result, 0, 0) == 4);
  check(std::string_view{PQgetvalue(&result, 0, 0), 4} == std::string_view{"a\0b\xff", 4});
  check(PQgetisnull(&result, 0, 1));
  check(!PQgetisnull(&result, 1, 1) && PQgetlength(&result, 1, 1) == 0);
  check(PQgetlength(&result, 1, 0) == 4096);
  check(std::string_view{PQgetvalue(&result, 1, 0), 4096} == std::string(4096, 'x'));
}

int main()
{
  check(PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);
  Result source{PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK), PQclear};
  check(source != nullptr);
  const auto empty = PQresultMemorySize(source.get());
  check(empty > 0 && PQresultMemorySize(source.get()) == empty);

  std::string payload_name{"payload"};
  std::string other_name{"other"};
  std::array<PGresAttDesc, 2> columns{
    PGresAttDesc{payload_name.data(), 41, 1, 1, 17, -1, -1},
    PGresAttDesc{other_name.data(), 0, 0, 0, 25, -1, -1}};
  check(PQsetResultAttrs(source.get(), static_cast<int>(columns.size()), columns.data()));
  const auto metadata = PQresultMemorySize(source.get());
  check(metadata >= empty);

  std::array<char, 4> binary{'a', '\0', 'b', static_cast<char>(0xff)};
  std::string large(4096, 'x');
  char zero = '\0';
  check(PQsetvalue(source.get(), 0, 0, binary.data(), static_cast<int>(binary.size())));
  check(PQsetvalue(source.get(), 0, 1, nullptr, -1));
  check(PQsetvalue(source.get(), 1, 0, large.data(), static_cast<int>(large.size())));
  check(PQsetvalue(source.get(), 1, 1, &zero, 0));
  values(*source);
  const auto populated = PQresultMemorySize(source.get());
  check(populated > metadata);
  binary.fill('z');
  large.assign(4096, 'z');
  values(*source);

  Result copy{PQcopyResult(source.get(), PG_COPYRES_TUPLES), PQclear};
  Result schema{PQcopyResult(source.get(), PG_COPYRES_ATTRS), PQclear};
  check(copy && schema);
  values(*copy);
  check(PQntuples(schema.get()) == 0 && PQnfields(schema.get()) == 2);
  const auto copied = PQresultMemorySize(copy.get());
  check(copied > 0 && PQresultMemorySize(source.get()) == populated);
  check(PQgetvalue(copy.get(), 1, 0) != PQgetvalue(source.get(), 1, 0));

  auto *extra = PQresultAlloc(source.get(), 65536);
  check(extra != nullptr);
  const auto reserved = PQresultMemorySize(source.get());
  check(reserved >= populated + 65536);
  check(PQresultMemorySize(copy.get()) == copied);
  source.reset();
  values(*copy);
  check(PQresultMemorySize(copy.get()) == copied);
  check(std::string_view{PQfname(schema.get(), 0)} == "payload");

  std::printf("Native result storage: libpq=%d, %u checks\n", PQlibVersion(), checks);
}
