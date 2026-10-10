#include <weave/postgres/connection.hpp>
#include <libpq-fe.h>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace pg = weave::pg;
using Snapshot = std::
  variant<pg::ResultSet, pg::Outcome, pg::CopyFormat, pg::PipelineResult, pg::Diagnostic, std::error_code>;
static unsigned checks = 0;

static void check(bool value)
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Native status check failed: %u\n", checks);
    std::exit(EXIT_FAILURE);
  }
}

static pg::ResultSet result(pg::ResultKind kind)
{
  pg::ResultSet value;
  value.kind = kind;
  return value;
}

static pg::PipelineResult barrier()
{
  pg::PipelineResult value;
  value.kind = pg::PipelineKind::sync;
  value.transaction = pg::Transaction::failed;
  return value;
}

int main()
{
  check(PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);

  struct Mapping {
    ExecStatusType native;
    const char *native_name;
    Snapshot snapshot;
    std::string_view name;
  };

  pg::Outcome aborted;
  aborted.aborted = true;
  const std::array mappings{
    Mapping{PGRES_EMPTY_QUERY, "PGRES_EMPTY_QUERY", result(pg::ResultKind::empty_query), "empty_query"},
    Mapping{PGRES_COMMAND_OK, "PGRES_COMMAND_OK", result(pg::ResultKind::command), "command"},
    Mapping{PGRES_COMMAND_OK, "PGRES_COMMAND_OK", result(pg::ResultKind::description), "description"},
    Mapping{PGRES_COMMAND_OK, "PGRES_COMMAND_OK", result(pg::ResultKind::acknowledgment), "acknowledgment"},
    Mapping{PGRES_TUPLES_OK, "PGRES_TUPLES_OK", result(pg::ResultKind::tuples), "tuples"},
    Mapping{PGRES_COPY_IN, "PGRES_COPY_IN", pg::CopyFormat{.direction = pg::CopyDirection::input}, "copy_input"},
    Mapping{PGRES_COPY_OUT, "PGRES_COPY_OUT", pg::CopyFormat{.direction = pg::CopyDirection::output}, "copy_output"},
    Mapping{PGRES_COPY_BOTH, "PGRES_COPY_BOTH", pg::CopyFormat{.direction = pg::CopyDirection::both}, "copy_both"},
    Mapping{PGRES_BAD_RESPONSE, "PGRES_BAD_RESPONSE", pg::make_error_code(pg::Error::protocol), "protocol_error"},
    Mapping{PGRES_FATAL_ERROR, "PGRES_FATAL_ERROR", pg::sql_error("22012"), "sql_error"},
    Mapping{PGRES_FATAL_ERROR, "PGRES_FATAL_ERROR", std::make_error_code(std::errc::connection_reset), "error"},
    Mapping{PGRES_NONFATAL_ERROR, "PGRES_NONFATAL_ERROR", pg::Diagnostic{{{'S', "NOTICE"}}}, "diagnostic"},
    Mapping{PGRES_SINGLE_TUPLE, "PGRES_SINGLE_TUPLE", result(pg::ResultKind::row_chunk), "row_chunk"},
    Mapping{PGRES_TUPLES_CHUNK, "PGRES_TUPLES_CHUNK", result(pg::ResultKind::row_chunk), "row_chunk"},
    Mapping{PGRES_PIPELINE_SYNC, "PGRES_PIPELINE_SYNC", barrier(), "pipeline_sync"},
    Mapping{PGRES_PIPELINE_ABORTED, "PGRES_PIPELINE_ABORTED", std::move(aborted), "aborted"}};
  for (const auto &mapping : mappings) {
    check(std::string_view{PQresStatus(mapping.native)} == mapping.native_name);
    auto handle = PQmakeEmptyPGresult(nullptr, mapping.native);
    check(handle != nullptr);
    check(PQresultStatus(handle) == mapping.native);
    auto name = std::visit(
      [](const auto &value) {
        return pg::status_name(value);
      },
      mapping.snapshot);
    check(name == mapping.name);
    PQclear(handle);
    check(name == mapping.name);
  }
  std::printf("Native status enum/snapshot controls passed: %u checks; libpq %d\n", checks, PQlibVersion());
}
