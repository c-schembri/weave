#include <libpq-fe.h>
#include <libpq-events.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>

static unsigned checks = 0;

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Native attachment check failed: %u\n", where.line());
    std::exit(1);
  }
}

struct State {
  std::array<unsigned, 3> created{};
  std::array<unsigned, 3> destroyed{};
  std::array<unsigned, 3> values{100, 101, 102};
  std::array<bool, 3> reject{};
};

template <unsigned Index>
static int observer(PGEventId kind, void *argument, void *context) noexcept
{
  auto &state = *static_cast<State *>(context);
  if (kind == PGEVT_RESULTCREATE) {
    auto &event = *static_cast<PGEventResultCreate *>(argument);
    check(event.result && event.conn);
    check(PQnfields(event.result) == 1 && PQntuples(event.result) == 1);
    check(PQgetlength(event.result, 0, 0) == 3);
    ++state.created[Index];
    if (state.reject[Index])
      return 0;
    check(PQresultSetInstanceData(event.result, observer<Index>, &state.values[Index]) != 0);
  } else if (kind == PGEVT_RESULTDESTROY) {
    ++state.destroyed[Index];
  }
  return 1;
}

int main()
{
  check(PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);
  State state;
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{PQconnectStart("not_a_keyword=value"), PQfinish};
  check(connection && PQstatus(connection.get()) == CONNECTION_BAD && PQsocket(connection.get()) == -1);
  check(PQregisterEventProc(connection.get(), observer<0>, "first", &state) != 0);
  check(PQregisterEventProc(connection.get(), observer<1>, "second", &state) != 0);
  check(PQregisterEventProc(connection.get(), observer<2>, "third", &state) != 0);
  const std::array statuses{
    PGRES_EMPTY_QUERY,
    PGRES_COMMAND_OK,
    PGRES_TUPLES_OK,
    PGRES_SINGLE_TUPLE,
    PGRES_TUPLES_CHUNK,
    PGRES_FATAL_ERROR};
  for (auto status : statuses) {
    state.created.fill(0);
    state.destroyed.fill(0);
    state.reject = {true, true, false};
    {
      std::unique_ptr<PGresult, decltype(&PQclear)> result{PQmakeEmptyPGresult(connection.get(), status), PQclear};
      check(bool(result));
      PGresAttDesc column{const_cast<char *>("binary"), 42, -2, 1, 17, -1, 7};
      check(PQsetResultAttrs(result.get(), 1, &column) != 0);
      std::array<char, 3> binary{'a', '\0', 'b'};
      check(PQsetvalue(result.get(), 0, 0, binary.data(), static_cast<int>(binary.size())) != 0);
      check(state.created == std::array<unsigned, 3>{0, 0, 0});
      check(PQfireResultCreateEvents(connection.get(), result.get()) == 0);
      check(state.created == std::array<unsigned, 3>{1, 1, 1});
      check(PQresultInstanceData(result.get(), observer<0>) == nullptr);
      check(PQresultInstanceData(result.get(), observer<1>) == nullptr);
      check(PQresultInstanceData(result.get(), observer<2>) == &state.values[2]);
      check(PQfireResultCreateEvents(connection.get(), result.get()) == 0);
      check(state.created == std::array<unsigned, 3>{2, 2, 1});
      state.reject[0] = false;
      check(PQfireResultCreateEvents(connection.get(), result.get()) == 0);
      check(state.created == std::array<unsigned, 3>{3, 3, 1});
      state.reject[1] = false;
      check(PQfireResultCreateEvents(connection.get(), result.get()) != 0);
      check(state.created == std::array<unsigned, 3>{3, 4, 1});
      check(PQfireResultCreateEvents(connection.get(), result.get()) != 0);
      check(state.created == std::array<unsigned, 3>{3, 4, 1});
      check(PQresultStatus(result.get()) == status);
    }
    check(state.destroyed == std::array<unsigned, 3>{1, 1, 1});
  }
  std::printf("Native application-result controls passed: %u checks; libpq: %d\n", checks, PQlibVersion());
}
