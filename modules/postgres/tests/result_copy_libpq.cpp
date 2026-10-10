#include <libpq-fe.h>
#include <libpq-events.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <source_location>

static unsigned checks = 0;

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Native result-copy check failed: %u\n", where.line());
    std::exit(1);
  }
}

struct Stats {
  unsigned creates = 0;
  unsigned copies = 0;
  unsigned destroys = 0;
  unsigned marker = 42;
  bool reject = false;
};

static int observer(PGEventId kind, void *argument, void *state) noexcept
{
  auto &stats = *static_cast<Stats *>(state);
  if (kind == PGEVT_RESULTCREATE) {
    auto &event = *static_cast<PGEventResultCreate *>(argument);
    ++stats.creates;
    check(PQresultSetInstanceData(event.result, observer, &stats.marker) != 0);
  } else if (kind == PGEVT_RESULTCOPY) {
    auto &event = *static_cast<PGEventResultCopy *>(argument);
    ++stats.copies;
    check(PQresultInstanceData(event.src, observer) == &stats.marker);
    check(PQresultInstanceData(event.dest, observer) == nullptr);
    if (stats.reject)
      return 0;
    check(PQresultSetInstanceData(event.dest, observer, &stats.marker) != 0);
  } else if (kind == PGEVT_RESULTDESTROY) {
    ++stats.destroys;
  }
  return 1;
}

int main()
{
  check(PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);
  Stats stats;
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{PQconnectStart("not_a_keyword=value"), PQfinish};
  check(connection && PQstatus(connection.get()) == CONNECTION_BAD);
  check(PQsocket(connection.get()) == -1);
  check(PQregisterEventProc(connection.get(), observer, "copy-control", &stats) != 0);
  const std::array selections{false, true};
  const std::array statuses{
    PGRES_EMPTY_QUERY,
    PGRES_COMMAND_OK,
    PGRES_TUPLES_OK,
    PGRES_SINGLE_TUPLE,
    PGRES_TUPLES_CHUNK,
    PGRES_FATAL_ERROR};
  for (auto status : statuses) {
    std::unique_ptr<PGresult, decltype(&PQclear)> source{PQmakeEmptyPGresult(connection.get(), status), PQclear};
    check(bool(source));
    std::array<PGresAttDesc, 3> columns{
      {{const_cast<char *>("binary"), 42, -2, 1, 17, -1, 7},
        {const_cast<char *>("empty"), 43, 2, 0, 25, -1, -1},
        {const_cast<char *>("null"), 0, 0, 0, 25, -1, -1}}};
    check(PQsetResultAttrs(source.get(), static_cast<int>(columns.size()), columns.data()) != 0);
    std::array<char, 3> binary{'a', '\0', 'b'};
    std::array<char, 1> empty{'\0'};
    check(PQsetvalue(source.get(), 0, 0, binary.data(), static_cast<int>(binary.size())) != 0);
    check(PQsetvalue(source.get(), 0, 1, empty.data(), 0) != 0);
    check(PQsetvalue(source.get(), 0, 2, nullptr, -1) != 0);
    check(PQfireResultCreateEvents(connection.get(), source.get()) != 0);
    for (bool columns_selected : selections) {
      for (bool rows_selected : selections) {
        for (bool events_selected : selections) {
          auto copied = stats.copies;
          auto destroyed = stats.destroys;
          int flags = (columns_selected ? PG_COPYRES_ATTRS : 0) | (rows_selected ? PG_COPYRES_TUPLES : 0) |
            (events_selected ? PG_COPYRES_EVENTS : 0);
          {
            std::unique_ptr<PGresult, decltype(&PQclear)> result{PQcopyResult(source.get(), flags), PQclear};
            check(bool(result));
            check(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
            check(std::strcmp(PQcmdStatus(result.get()), PQcmdStatus(source.get())) == 0);
            check(std::strlen(PQresultErrorMessage(result.get())) == 0);
            check(PQnfields(result.get()) == ((columns_selected || rows_selected) ? 3 : 0));
            check(PQntuples(result.get()) == (rows_selected ? 1 : 0));
            if (columns_selected || rows_selected) {
              for (int index = 0; index < 3; ++index) {
                check(std::strcmp(PQfname(result.get(), index), PQfname(source.get(), index)) == 0);
                check(PQftable(result.get(), index) == PQftable(source.get(), index));
                check(PQftablecol(result.get(), index) == PQftablecol(source.get(), index));
                check(PQfformat(result.get(), index) == PQfformat(source.get(), index));
                check(PQftype(result.get(), index) == PQftype(source.get(), index));
                check(PQfsize(result.get(), index) == PQfsize(source.get(), index));
                check(PQfmod(result.get(), index) == PQfmod(source.get(), index));
              }
            }
            if (rows_selected) {
              check(PQgetlength(result.get(), 0, 0) == 3);
              check(std::memcmp(PQgetvalue(result.get(), 0, 0), binary.data(), 3) == 0);
              check(PQgetisnull(result.get(), 0, 1) == 0 && PQgetlength(result.get(), 0, 1) == 0);
              check(PQgetisnull(result.get(), 0, 2) != 0);
              check(PQsetvalue(result.get(), 0, 0, empty.data(), 0) != 0);
              check(PQgetlength(source.get(), 0, 0) == 3);
            }
            check(stats.copies == copied + static_cast<unsigned>(events_selected));
            check(PQresultInstanceData(result.get(), observer) == (events_selected ? &stats.marker : nullptr));
          }
          check(stats.destroys == destroyed + static_cast<unsigned>(events_selected));
        }
      }
    }
    stats.reject = true;
    auto destroyed = stats.destroys;
    {
      std::unique_ptr<PGresult, decltype(&PQclear)> rejected{PQcopyResult(source.get(), PG_COPYRES_EVENTS), PQclear};
      check(rejected && PQresultInstanceData(rejected.get(), observer) == nullptr);
      auto copied = stats.copies;
      std::unique_ptr<PGresult, decltype(&PQclear)> next{PQcopyResult(rejected.get(), PG_COPYRES_EVENTS), PQclear};
      check(next && stats.copies == copied);
    }
    check(stats.destroys == destroyed);
    stats.reject = false;
  }
  check(stats.creates == statuses.size());
  std::printf("Native selective-copy controls passed: %u checks; libpq: %d\n", checks, PQlibVersion());
}
